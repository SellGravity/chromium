// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/navigator_ua_data.h"

#include "base/compiler_specific.h"
#include "base/task/single_thread_task_runner.h"
#include <cstring>
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/privacy_budget/identifiability_metric_builder.h"
#include "third_party/blink/public/common/privacy_budget/identifiability_study_settings.h"
#include "third_party/blink/public/common/privacy_budget/identifiable_surface.h"
#include "third_party/blink/public/common/privacy_budget/identifiable_token.h"
#include "third_party/blink/public/common/privacy_budget/identifiable_token_builder.h"
#include "third_party/blink/public/mojom/use_counter/metrics/web_feature.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_ua_data_values.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/dactyloscoper.h"
#include "third_party/blink/renderer/core/frame/web_feature_forward.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace blink {

namespace {

// Record identifiability study metrics for a single field requested by a
// getHighEntropyValues() call if the user is in the study.
void MaybeRecordMetric(bool record_identifiability,
                       const String& hint,
                       const IdentifiableToken token,
                       ExecutionContext* execution_context) {
  if (!record_identifiability) [[likely]] {
    return;
  }
  auto identifiable_surface = IdentifiableSurface::FromTypeAndToken(
      IdentifiableSurface::Type::kNavigatorUAData_GetHighEntropyValues,
      IdentifiableToken(hint.Utf8()));
  IdentifiabilityMetricBuilder(execution_context->UkmSourceID())
      .Add(identifiable_surface, token)
      .Record(execution_context->UkmRecorder());
}

void MaybeRecordMetric(bool record_identifiability,
                       const String& hint,
                       const String& value,
                       ExecutionContext* execution_context) {
  MaybeRecordMetric(record_identifiability, hint,
                    IdentifiableToken(value.Utf8()), execution_context);
}

void MaybeRecordMetric(bool record_identifiability,
                       const String& hint,
                       const Vector<String>& strings,
                       ExecutionContext* execution_context) {
  if (!record_identifiability) [[likely]] {
    return;
  }
  IdentifiableTokenBuilder token_builder;
  for (const auto& s : strings) {
    token_builder.AddAtomic(s.Utf8());
  }
  MaybeRecordMetric(record_identifiability, hint, token_builder.GetToken(),
                    execution_context);
}

// ---------------------------------------------------------------------------
// UA string parsing helpers for SyncWithUserAgent()
// ---------------------------------------------------------------------------

// Extract Chrome major version from UA string, e.g.
// "...Chrome/120.0.6099.71 Safari/537.36" -> "120"
static std::string ExtractChromeMajorVersion(const std::string& ua) {
  const char* kKey = "Chrome/";
  auto pos = ua.find(kKey);
  if (pos == std::string::npos)
    return "";
  pos += strlen(kKey);
  auto dot = ua.find('.', pos);
  if (dot == std::string::npos)
    return ua.substr(pos);
  return ua.substr(pos, dot - pos);
}


static UserAgentBrandList GetConfiguredBrandsFor(const String& full_ua) {
  std::string ua = full_ua.Utf8();
  std::string major = ExtractChromeMajorVersion(ua);
  UserAgentBrandList brands;
  // Greased brand to prevent sniffing
  brands.push_back({"Not/A)Brand", "8"});
  brands.push_back({"Chromium", major});
  brands.push_back({"Google Chrome", major});
  return brands;
}

// Returns platform name, e.g. "Windows", "macOS", "Linux", "Android"
static String GetConfiguredPlatformFor(const String& full_ua) {
  std::string ua = full_ua.Utf8();
  if (ua.find("Android") != std::string::npos)
    return "Android";
  if (ua.find("iPhone") != std::string::npos ||
      ua.find("iPad") != std::string::npos)
    return "iOS";
  if (ua.find("Windows") != std::string::npos)
    return "Windows";
  if (ua.find("Macintosh") != std::string::npos ||
      ua.find("Mac OS X") != std::string::npos)
    return "macOS";
  if (ua.find("Linux") != std::string::npos ||
      ua.find("X11") != std::string::npos)
    return "Linux";
  if (ua.find("CrOS") != std::string::npos)
    return "Chrome OS";
  return "";
}

// Returns platform version, e.g. "10.0.0" for Windows 10
static String GetConfiguredPlatformVersionFor(const String& full_ua) {
  std::string ua = full_ua.Utf8();
  // Windows NT x.y
  auto pos = ua.find("Windows NT ");
  if (pos != std::string::npos) {
    pos += strlen("Windows NT ");
    auto end = ua.find_first_of(";)", pos);
    if (end == std::string::npos)
      end = ua.size();
    std::string nt_ver = ua.substr(pos, end - pos);
    // Map NT version to Windows marketing version as a simple passthrough
    // (clients interpret this; "10.0.0" is standard for Win10/11)
    return String::FromUTF8(nt_ver + ".0");
  }
  // Android x.y
  pos = ua.find("Android ");
  if (pos != std::string::npos) {
    pos += strlen("Android ");
    auto end = ua.find_first_of(";)", pos);
    if (end == std::string::npos)
      end = ua.size();
    return String::FromUTF8(ua.substr(pos, end - pos));
  }
  // Mac OS X x_y_z
  pos = ua.find("Mac OS X ");
  if (pos != std::string::npos) {
    pos += strlen("Mac OS X ");
    auto end = ua.find_first_of(")", pos);
    if (end == std::string::npos)
      end = ua.size();
    std::string ver = ua.substr(pos, end - pos);
    for (char& c : ver)
      if (c == '_') c = '.';
    return String::FromUTF8(ver);
  }
  return "";
}

static bool IsUserAgentMobile(const String& full_ua) {
  std::string ua = full_ua.Utf8();
  return ua.find("Mobile") != std::string::npos;
}

static String GetConfiguredArchitectureFor(const String& full_ua) {
  std::string ua = full_ua.Utf8();
  if (ua.find("arm") != std::string::npos ||
      ua.find("ARM") != std::string::npos ||
      ua.find("aarch64") != std::string::npos)
    return "arm";
  if (ua.find("x86_64") != std::string::npos ||
      ua.find("Win64") != std::string::npos ||
      ua.find("WOW64") != std::string::npos ||
      ua.find("x64") != std::string::npos)
    return "x86";
  return "x86";
}

static String GetConfiguredBitnessFor(const String& full_ua) {
  if (IsUserAgentMobile(full_ua))
    return " ";  // Mobile is always 32-bit for UAData
  std::string ua = full_ua.Utf8();
  if (ua.find("Win64") != std::string::npos ||
      ua.find("WOW64") != std::string::npos ||
      ua.find("x86_64") != std::string::npos ||
      ua.find("x64") != std::string::npos ||
      ua.find("aarch64") != std::string::npos)
    return "64";
  return "32";
}

// Model is only relevant for mobile; empty on desktop
static String GetConfiguredModelFor(const String& full_ua) {
  // Android model: "...Android 13; Pixel 7 Build/..." -> "Pixel 7"
  std::string ua = full_ua.Utf8();
  auto start = ua.find("Android ");
  if (start != std::string::npos) {
    auto semi = ua.find(';', start);
    if (semi != std::string::npos) {
      auto model_start = semi + 2;
      auto model_end = ua.find_first_of(";)", model_start);
      if (model_end == std::string::npos)
        model_end = ua.size();
      // Strip "Build/..." suffix if present
      auto build = ua.find(" Build/", model_start);
      if (build != std::string::npos && build < model_end)
        model_end = build;
      if (model_end > model_start)
        return String::FromUTF8(ua.substr(model_start, model_end - model_start));
    }
  }
  return "";
}

static bool IsUserAgentWoW64(const String& full_ua) {
  std::string ua = full_ua.Utf8();
  return ua.find("WOW64") != std::string::npos;
}

}  // namespace

NavigatorUAData::NavigatorUAData(ExecutionContext* context)
    : ExecutionContextClient(context) {
  NavigatorUABrandVersion* dict = NavigatorUABrandVersion::Create();
  dict->setBrand("");
  dict->setVersion("");
  empty_brand_set_.push_back(dict);
}

void NavigatorUAData::AddBrandVersion(const String& brand,
                                      const String& version) {
  NavigatorUABrandVersion* dict = NavigatorUABrandVersion::Create();
  dict->setBrand(brand);
  dict->setVersion(version);
  brand_set_.push_back(dict);
}

void NavigatorUAData::AddBrandFullVersion(const String& brand,
                                          const String& version) {
  NavigatorUABrandVersion* dict = NavigatorUABrandVersion::Create();
  dict->setBrand(brand);
  dict->setVersion(version);
  full_version_list_.push_back(dict);
}

void NavigatorUAData::SetBrandVersionList(
    const UserAgentBrandList& brand_version_list) {
  for (const auto& brand_version : brand_version_list) {
    AddBrandVersion(String::FromUTF8(brand_version.brand),
                    String::FromUTF8(brand_version.version));
  }
}

void NavigatorUAData::SetFullVersionList(
    const UserAgentBrandList& full_version_list) {
  for (const auto& brand_version : full_version_list) {
    AddBrandFullVersion(String::FromUTF8(brand_version.brand),
                        String::FromUTF8(brand_version.version));
  }
}

void NavigatorUAData::SetMobile(bool mobile) {
  is_mobile_ = mobile;
}

void NavigatorUAData::SetPlatform(const String& brand, const String& version) {
  platform_ = brand;
  platform_version_ = version;
}

void NavigatorUAData::SetArchitecture(const String& architecture) {
  architecture_ = architecture;
}

void NavigatorUAData::SetModel(const String& model) {
  model_ = model;
}

void NavigatorUAData::SetUAFullVersion(const String& ua_full_version) {
  ua_full_version_ = ua_full_version;
}

void NavigatorUAData::SetBitness(const String& bitness) {
  bitness_ = bitness;
}

void NavigatorUAData::SetWoW64(bool wow64) {
  is_wow64_ = wow64;
}

void NavigatorUAData::SetFormFactors(Vector<String> form_factors) {
  form_factors_ = std::move(form_factors);
}
void NavigatorUAData::SyncWithUserAgent(const String& full_user_agent) {
  SetBrandVersionList(GetConfiguredBrandsFor(full_user_agent));
  SetPlatform(GetConfiguredPlatformFor(full_user_agent),
              GetConfiguredPlatformVersionFor(full_user_agent));
  SetMobile(IsUserAgentMobile(full_user_agent));
  SetArchitecture(GetConfiguredArchitectureFor(full_user_agent));
  SetBitness(GetConfiguredBitnessFor(full_user_agent));
  SetModel(GetConfiguredModelFor(full_user_agent));
  SetWoW64(IsUserAgentWoW64(full_user_agent));
}
bool NavigatorUAData::mobile() const {
  if (GetExecutionContext()) {
    return is_mobile_;
  }
  return false;
}

const HeapVector<Member<NavigatorUABrandVersion>>& NavigatorUAData::brands()
    const {
  constexpr auto identifiable_surface = IdentifiableSurface::FromTypeAndToken(
      IdentifiableSurface::Type::kWebFeature,
      WebFeature::kNavigatorUAData_Brands);

  ExecutionContext* context = GetExecutionContext();
  if (context) {
    // Record IdentifiabilityStudy metrics if the client is in the study.
    if (IdentifiabilityStudySettings::Get()->ShouldSampleSurface(
            identifiable_surface)) [[unlikely]] {
      IdentifiableTokenBuilder token_builder;
      for (const auto& brand : brand_set_) {
        token_builder.AddValue(brand->hasBrand());
        if (brand->hasBrand())
          token_builder.AddAtomic(brand->brand().Utf8());
        token_builder.AddValue(brand->hasVersion());
        if (brand->hasVersion())
          token_builder.AddAtomic(brand->version().Utf8());
      }
      IdentifiabilityMetricBuilder(context->UkmSourceID())
          .Add(identifiable_surface, token_builder.GetToken())
          .Record(context->UkmRecorder());
    }

    return brand_set_;
  }

  return empty_brand_set_;
}

const String& NavigatorUAData::platform() const {
  if (GetExecutionContext()) {
    return platform_;
  }
  return g_empty_string;
}

bool AllowedToCollectHighEntropyValues(ExecutionContext* execution_context) {
  // To determine whether a document is allowed to use the get high-entropy
  // client hints returned by navigator.userAgentData.getHighEntropyValues(),
  // check the following:

  // 1. Check if our RuntimeEnabledFeature is enabled
  // Note: We return true if not enabled because the default allowlist is "*",
  // this permissions-policy allows a document to restrict it.
  // TODO(crbug.com/388538952): remove this after it ships to stable
  if (!RuntimeEnabledFeatures::
          ClientHintUAHighEntropyValuesPermissionPolicyEnabled()) {
    return true;
  }

  // 2. If Permissions Policy is enabled, return the policy for
  // "ch-ua-high-entropy-values" feature.
  return execution_context->IsFeatureEnabled(
      network::mojom::PermissionsPolicyFeature::kClientHintUAHighEntropyValues,
      ReportOptions::kReportOnFailure,
      "Collection of high-entropy user-agent client hints is disabled for "
      "this document.");
}

ScriptPromise<UADataValues> NavigatorUAData::getHighEntropyValues(
    ScriptState* script_state,
    const Vector<String>& hints) const {
  auto* resolver =
      MakeGarbageCollected<ScriptPromiseResolver<UADataValues>>(script_state);
  auto promise = resolver->Promise();
  auto* execution_context =
      ExecutionContext::From(script_state);  // GetExecutionContext();
  DCHECK(execution_context);

  bool record_identifiability =
      IdentifiabilityStudySettings::Get()->ShouldSampleType(
          IdentifiableSurface::Type::kNavigatorUAData_GetHighEntropyValues);
  UADataValues* values = MakeGarbageCollected<UADataValues>();
  // TODO: It'd be faster to compare hint when turning |hints| into an
  // AtomicString vector and turning the const string literals |hint| into
  // AtomicStrings as well.

  // According to
  // https://wicg.github.io/ua-client-hints/#getHighEntropyValues, the
  // low-entropy brands, mobile and platform hints should always be included for
  // convenience.

  // Use `brands()` and not `brand_set_` directly since the former also
  // records IdentifiabilityStudy metrics.
  values->setBrands(brands());
  values->setMobile(is_mobile_);
  values->setPlatform(platform_);
  // Record IdentifiabilityStudy metrics for `mobile()` and `platform()` (the
  // `brands()` part is already recorded inside that function).
  Dactyloscoper::RecordDirectSurface(
      GetExecutionContext(), WebFeature::kNavigatorUAData_Mobile, mobile());
  Dactyloscoper::RecordDirectSurface(
      GetExecutionContext(), WebFeature::kNavigatorUAData_Platform, platform());

  // If the "ch-ua-high-entropy-values" permission policy is enabled for a
  // document, add high-entropy client hints to values (if requested)
  if (AllowedToCollectHighEntropyValues(execution_context)) {
    for (const String& hint : hints) {
      if (hint == "platformVersion") {
        values->setPlatformVersion(platform_version_);
        MaybeRecordMetric(record_identifiability, hint, platform_version_,
                          execution_context);
      } else if (hint == "architecture") {
        values->setArchitecture(architecture_);
        MaybeRecordMetric(record_identifiability, hint, architecture_,
                          execution_context);
      } else if (hint == "model") {
        values->setModel(model_);
        MaybeRecordMetric(record_identifiability, hint, model_,
                          execution_context);
      } else if (hint == "uaFullVersion") {
        values->setUaFullVersion(ua_full_version_);
        MaybeRecordMetric(record_identifiability, hint, ua_full_version_,
                          execution_context);
      } else if (hint == "bitness") {
        values->setBitness(bitness_);
        MaybeRecordMetric(record_identifiability, hint, bitness_,
                          execution_context);
      } else if (hint == "fullVersionList") {
        values->setFullVersionList(full_version_list_);
      } else if (hint == "wow64") {
        values->setWow64(is_wow64_);
        MaybeRecordMetric(record_identifiability, hint, is_wow64_ ? "?1" : "?0",
                          execution_context);
      } else if (hint == "formFactors") {
        values->setFormFactors(form_factors_);
        MaybeRecordMetric(record_identifiability, hint, form_factors_,
                          execution_context);
      }
    }
  }

  execution_context->GetTaskRunner(TaskType::kPermission)
      ->PostTask(
          FROM_HERE,
          BindOnce([](ScriptPromiseResolver<UADataValues>* resolver,
                      UADataValues* values) { resolver->Resolve(values); },
                   WrapPersistent(resolver), WrapPersistent(values)));

  return promise;
}

ScriptObject NavigatorUAData::toJSON(ScriptState* script_state) const {
  V8ObjectBuilder builder(script_state);
  builder.AddVector<NavigatorUABrandVersion>("brands", brands());
  builder.AddBoolean("mobile", mobile());
  builder.AddString("platform", platform());

  // Record IdentifiabilityStudy metrics for `mobile()` and `platform()`
  // (the `brands()` part is already recorded inside that function).
  Dactyloscoper::RecordDirectSurface(
      GetExecutionContext(), WebFeature::kNavigatorUAData_Mobile, mobile());
  Dactyloscoper::RecordDirectSurface(
      GetExecutionContext(), WebFeature::kNavigatorUAData_Platform, platform());

  return builder.ToScriptObject();
}

void NavigatorUAData::Trace(Visitor* visitor) const {
  visitor->Trace(brand_set_);
  visitor->Trace(full_version_list_);
  visitor->Trace(empty_brand_set_);
  ScriptWrappable::Trace(visitor);
  ExecutionContextClient::Trace(visitor);
}

}  // namespace blink
