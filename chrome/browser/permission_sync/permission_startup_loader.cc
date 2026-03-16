// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permission_sync/permission_startup_loader.h"

#include <cstdlib>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "build/build_config.h"
#include "chrome/browser/permission_sync/permission_cache_manager.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace permission_sync {

namespace {

// Environment variable name. GravityBrowser sets this when spawning
// Chromium with Base64-encoded permission JSON.
constexpr char kEnvVarName[] = "GRAVITY_STARTUP_RULES";

// Clear environment variable from current process.
// This reduces the window where rules are visible via Process Explorer.
// Note: child processes already spawned will retain their copy.
void ClearEnvVar() {
#if BUILDFLAG(IS_WIN)
  ::SetEnvironmentVariableA(kEnvVarName, nullptr);
#else
  ::unsetenv(kEnvVarName);
#endif
}

}  // namespace

// static
PermissionStartupLoader::LoadResult PermissionStartupLoader::Load(
    PermissionCacheManager* cache_manager) {
  DCHECK(cache_manager);

  // Source 1: Environment variable (primary — fastest, no I/O)
  LoadResult result = TryLoadFromEnvVar(cache_manager);
  if (result.success) {
    return result;
  }

  // Source 2: File cache (future — implement when needed)
  // LoadResult file_result = TryLoadFromFile(cache_manager, profile_path);
  // if (file_result.success) return file_result;

  // No startup data available.
  LOG(INFO) << "[PermissionStartupLoader] No startup rules found. "
            << "Browser will wait for WebSocket sync.";

  LoadResult no_data;
  no_data.success = false;
  no_data.source = "none";
  no_data.error = "No GRAVITY_STARTUP_RULES env var found";
  return no_data;
}

// static
PermissionStartupLoader::LoadResult
PermissionStartupLoader::TryLoadFromEnvVar(
    PermissionCacheManager* cache_manager) {
  LoadResult result;
  result.source = "env_var";

  // Read env var. std::getenv reads from process memory — no I/O,
  // no ScopedAllowBlocking needed. ~0.001ms.
  const char* env_value = std::getenv(kEnvVarName);
  if (!env_value || env_value[0] == '\0') {
    result.success = false;
    result.error = "GRAVITY_STARTUP_RULES not set";
    return result;
  }

  std::string base64_data(env_value);

  // SECURITY: Clear env var immediately after reading.
  // Minimizes window where Process Explorer can see the data.
  ClearEnvVar();

  LOG(INFO) << "[PermissionStartupLoader] Found env var ("
            << base64_data.size() << " chars Base64)";

  // Decode Base64.
  std::string json_data;
  if (!base::Base64Decode(base64_data, &json_data)) {
    LOG(ERROR) << "[PermissionStartupLoader] Base64 decode failed";
    result.success = false;
    result.error = "Base64 decode failed";
    return result;
  }

  // Parse JSON.
  auto parsed = base::JSONReader::Read(json_data, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[PermissionStartupLoader] Invalid JSON in env var";
    result.success = false;
    result.error = "Invalid JSON";
    return result;
  }

  const base::Value::Dict& dict = parsed->GetDict();

  // Validate required fields.
  if (!ValidatePermissionJson(dict)) {
    LOG(ERROR) << "[PermissionStartupLoader] JSON validation failed";
    result.success = false;
    result.error = "JSON missing required fields";
    return result;
  }

  // Load into cache manager.
  if (!cache_manager->LoadFullPermissionSet(dict)) {
    LOG(ERROR) << "[PermissionStartupLoader] LoadFullPermissionSet failed";
    result.success = false;
    result.error = "Cache load failed";
    return result;
  }

  // Transition directly to SYNCHRONIZED.
  // Bypass state machine validation: at startup, state is DISCONNECTED
  // and there's no valid DISCONNECTED → SYNCHRONIZED transition.
  // This is intentional — env var is a trusted local source, not a
  // network message. State machine validates WebSocket protocol flow.
  cache_manager->SetSynchronizedFromStartup();

  result.success = true;
  result.version = cache_manager->GetPermissionVersion();
  result.rule_count = cache_manager->GetRuleCount();

  LOG(INFO) << "[PermissionStartupLoader] Loaded from env var: "
            << result.rule_count << " rules, version "
            << result.version;

  return result;
}

// static
bool PermissionStartupLoader::ValidatePermissionJson(
    const base::Value::Dict& dict) {
  // Must have version (integer).
  if (!dict.FindInt("version")) {
    return false;
  }
  // Must have default_policy (string).
  if (!dict.FindString("default_policy")) {
    return false;
  }
  // Must have rules (list) — can be empty.
  if (!dict.FindList("rules")) {
    return false;
  }
  return true;
}

}  // namespace permission_sync
