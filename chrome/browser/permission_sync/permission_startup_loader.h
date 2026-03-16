// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_STARTUP_LOADER_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_STARTUP_LOADER_H_

#include <string>

#include "base/values.h"

namespace permission_sync {

class PermissionCacheManager;

// ============================================================================
// PermissionStartupLoader
// ============================================================================
// Loads permission rules at startup WITHOUT any network dependency.
// Attempts two sources in order:
//   1. Environment variable GRAVITY_STARTUP_RULES (Base64-encoded JSON)
//   2. (Future) Fallback file cache
//
// This class exists solely to decouple browser startup from WebSocket/NC
// stability. After loading, the env var is cleared from process memory.
//
// Thread safety: All methods run on UI thread during factory construction.
//
class PermissionStartupLoader {
 public:
  // Result of attempting to load startup rules.
  struct LoadResult {
    bool success = false;
    std::string source;           // "env_var", "file_cache", or "none"
    int version = 0;
    size_t rule_count = 0;
    std::string error;            // Empty on success
  };

  // Attempts to load rules into |cache_manager| from available sources.
  // On success, cache_manager transitions to SYNCHRONIZED.
  // On failure, cache_manager stays DISCONNECTED (will need WebSocket).
  //
  // Must be called on UI thread, during factory construction.
  static LoadResult Load(PermissionCacheManager* cache_manager);

 private:
  // Try loading from GRAVITY_STARTUP_RULES environment variable.
  // Decodes Base64 → JSON → calls LoadFullPermissionSet.
  // Clears the env var after reading (security: minimize exposure window).
  static LoadResult TryLoadFromEnvVar(PermissionCacheManager* cache_manager);

  // Validate that parsed JSON has required fields.
  static bool ValidatePermissionJson(const base::Value::Dict& dict);
};

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_STARTUP_LOADER_H_
