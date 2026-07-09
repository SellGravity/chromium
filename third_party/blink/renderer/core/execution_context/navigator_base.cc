// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/execution_context/navigator_base.h"

#include "base/feature_list.h"
#include "build/build_config.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/navigator_concurrent_hardware.h"
#include "third_party/blink/renderer/core/probe/core_probes.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN)
#include <sys/utsname.h>
#include "third_party/blink/renderer/platform/wtf/thread_specific.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"
#endif

namespace blink {

namespace {

String GetReducedNavigatorPlatform() {
#if BUILDFLAG(IS_ANDROID)
  return "Linux armv8l";
#elif BUILDFLAG(IS_MAC)
  return "MacIntel";
#elif BUILDFLAG(IS_WIN)
  return "Win32";
#elif BUILDFLAG(IS_FUCHSIA)
  return "";
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  return "Linux x86_64";
#elif BUILDFLAG(IS_IOS)
  return "iPhone";
#else
#error Unsupported platform
#endif
}

}  // namespace

NavigatorBase::NavigatorBase(ExecutionContext* context)
    : NavigatorLanguage(context), ExecutionContextClient(context) {}

String NavigatorBase::userAgent() const {
  ExecutionContext* execution_context = GetExecutionContext();
  // SessionNoiseCache (antidetect spoofed UA) always takes priority
  const std::string& noise_ua = SessionNoiseCache::GetInstance().GetUserAgent();
  if (!noise_ua.empty()) {
    return String::FromUTF8(noise_ua);
  }
  // Fall back to DevTools probe override (only if no antidetect UA is set)
  if (execution_context) {
    String probe_ua;
    probe::ApplyUserAgentOverride(probe::ToCoreProbeSink(execution_context), &probe_ua);
    if (!probe_ua.empty()) {
      return probe_ua;
    }
  }
  return execution_context ? execution_context->UserAgent() : String();
}

String NavigatorBase::platform() const {
  ExecutionContext* execution_context = GetExecutionContext();
  String ua_str = this->userAgent();
  if (!ua_str.empty()) {
    if (ua_str.Contains("Android")) return "Linux armv8l";
    if (ua_str.Contains("iPhone") || ua_str.Contains("iPad")) return "iPhone";
    if (ua_str.Contains("Windows")) return "Win32";
    if (ua_str.Contains("Macintosh") || ua_str.Contains("Mac OS X")) return "MacIntel";
    if (ua_str.Contains("CrOS")) return "Chrome OS";
    if (ua_str.Contains("Linux") || ua_str.Contains("X11")) return "Linux x86_64";
  }

#if BUILDFLAG(IS_ANDROID)
  // For user-agent reduction phase 6, Android platform should be frozen
  // string, see https://www.chromium.org/updates/ua-reduction/.
  if (RuntimeEnabledFeatures::ReduceUserAgentAndroidVersionDeviceModelEnabled(
          execution_context)) {
    return GetReducedNavigatorPlatform();
  }
#else
  // For user-agent reduction phase 5, all desktop platform should be frozen
  // string, see https://www.chromium.org/updates/ua-reduction/.
  if (RuntimeEnabledFeatures::ReduceUserAgentPlatformOsCpuEnabled(
          execution_context)) {
    return GetReducedNavigatorPlatform();
  }
#endif

  return NavigatorID::platform();
}

void NavigatorBase::Trace(Visitor* visitor) const {
  ScriptWrappable::Trace(visitor);
  NavigatorLanguage::Trace(visitor);
  ExecutionContextClient::Trace(visitor);
  Supplementable<NavigatorBase>::Trace(visitor);
}

unsigned int NavigatorBase::hardwareConcurrency() const {
  unsigned int hardware_concurrency =
      NavigatorConcurrentHardware::hardwareConcurrency();

  probe::ApplyHardwareConcurrencyOverride(
      probe::ToCoreProbeSink(GetExecutionContext()), hardware_concurrency);
  return hardware_concurrency;
}

ExecutionContext* NavigatorBase::GetUAExecutionContext() const {
  return GetExecutionContext();
}

UserAgentMetadata NavigatorBase::GetUserAgentMetadata() const {
  ExecutionContext* execution_context = GetExecutionContext();
  return execution_context ? execution_context->GetUserAgentMetadata()
                           : blink::UserAgentMetadata();
}

String NavigatorBase::GetUserAgent() const {
  return userAgent();
}

}  // namespace blink
