// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_POLICY_MANAGER_POLICY_PREFERENCE_SYNCER_H_
#define CHROME_BROWSER_POLICY_MANAGER_POLICY_PREFERENCE_SYNCER_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/policy_manager/policy_ipc_client.h"

class PrefService;

namespace policy_manager {

// Synchronizes policy from IPC server to PrefService.
// This component bridges the external policy manager with Chrome's
// preference system, allowing real-time policy updates.
class PolicyPreferenceSyncer {
 public:
  explicit PolicyPreferenceSyncer(PrefService* prefs);
  ~PolicyPreferenceSyncer();

  PolicyPreferenceSyncer(const PolicyPreferenceSyncer&) = delete;
  PolicyPreferenceSyncer& operator=(const PolicyPreferenceSyncer&) = delete;

  // Initialize syncer and start watching for policy updates
  void Initialize(const std::string& pipe_name,
                  const std::string& profile_name = "");

  // Handle policy update from IPC
  void OnPolicyUpdated(const base::Value::Dict& policy);

  // Check if syncer is active
  bool IsActive() const;

 private:
  // Parse and sync URL blocklist from policy to prefs
  void SyncURLBlocklist(const base::Value::Dict& policy);

  // Parse and sync URL allowlist from policy to prefs
  void SyncURLAllowlist(const base::Value::Dict& policy);

  // Sync feature flags from policy
  void SyncFeatures(const base::Value::Dict& policy);

  // Sync homepage setting
  void SyncHomepage(const base::Value::Dict& policy);

  raw_ptr<PrefService> prefs_;
  std::unique_ptr<PolicyIPCClient> ipc_client_;
  bool active_ = false;

  base::WeakPtrFactory<PolicyPreferenceSyncer> weak_factory_{this};
};

}  // namespace policy_manager

#endif  // CHROME_BROWSER_POLICY_MANAGER_POLICY_PREFERENCE_SYNCER_H_
