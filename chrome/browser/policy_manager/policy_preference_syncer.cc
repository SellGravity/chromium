// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy_manager/policy_preference_syncer.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"

namespace policy_manager {

PolicyPreferenceSyncer::PolicyPreferenceSyncer(PrefService* prefs)
    : prefs_(prefs) {}

PolicyPreferenceSyncer::~PolicyPreferenceSyncer() = default;

void PolicyPreferenceSyncer::Initialize(const std::string& pipe_name,
                                        const std::string& profile_name) {
  if (pipe_name.empty()) {
    LOG(INFO) << "[PolicyPreferenceSyncer] No pipe name specified, skipping";
    return;
  }

  ipc_client_ = std::make_unique<PolicyIPCClient>();

  // Set profile name before connecting
  if (!profile_name.empty()) {
    ipc_client_->SetProfileName(profile_name);
  }

  if (!ipc_client_->Initialize(pipe_name)) {
    LOG(WARNING) << "[PolicyPreferenceSyncer] Failed to connect to IPC server";
    ipc_client_.reset();
    return;
  }

  // Get initial policy
  auto initial_policy = ipc_client_->GetPolicyDictSync("user_role");
  if (!initial_policy.empty()) {
    OnPolicyUpdated(initial_policy);
  }

  // Register for updates
  ipc_client_->RegisterPolicyUpdateListener(
      base::BindRepeating(&PolicyPreferenceSyncer::OnPolicyUpdated,
                          weak_factory_.GetWeakPtr()));

  // Start watching for changes (poll every 5 seconds)
  ipc_client_->StartPolicyWatcher(5000);

  active_ = true;
  LOG(INFO) << "[PolicyPreferenceSyncer] Initialized and watching for updates";
}

void PolicyPreferenceSyncer::OnPolicyUpdated(const base::Value::Dict& policy) {
  LOG(INFO) << "[PolicyPreferenceSyncer] Policy updated from server";

  SyncURLBlocklist(policy);
  SyncURLAllowlist(policy);
  SyncFeatures(policy);
  SyncHomepage(policy);
}

void PolicyPreferenceSyncer::SyncURLBlocklist(const base::Value::Dict& policy) {
  const auto* blocklist = policy.FindList("URLBlocklist");
  if (!blocklist) {
    return;
  }

  LOG(INFO) << "[PolicyPreferenceSyncer] Updating URLBlocklist with "
            << blocklist->size() << " entries";

  // Get existing blocklist
  base::Value::List new_blocklist;
  for (const auto& item : *blocklist) {
    if (item.is_string()) {
      new_blocklist.Append(item.GetString());
    }
  }

  // Update preference
  prefs_->SetList(prefs::kProfileURLBlocklist, std::move(new_blocklist));
}

void PolicyPreferenceSyncer::SyncURLAllowlist(const base::Value::Dict& policy) {
  const auto* allowlist = policy.FindList("URLAllowlist");
  if (!allowlist) {
    return;
  }

  LOG(INFO) << "[PolicyPreferenceSyncer] Updating URLAllowlist with "
            << allowlist->size() << " entries";

  base::Value::List new_allowlist;
  for (const auto& item : *allowlist) {
    if (item.is_string()) {
      new_allowlist.Append(item.GetString());
    }
  }

  // Sync URLAllowlist to preferences (whitelist has priority over blacklist)
  prefs_->SetList(prefs::kProfileURLAllowlist, std::move(new_allowlist));
}

void PolicyPreferenceSyncer::SyncFeatures(const base::Value::Dict& policy) {
  const auto* features = policy.FindDict("Features");
  if (!features) {
    return;
  }

  LOG(INFO) << "[PolicyPreferenceSyncer] Updating features";

  // Example feature sync - add more as needed
  if (auto enable_history = features->FindBool("EnableHistory")) {
    // prefs_->SetBoolean("profile.features.enable_history", *enable_history);
    LOG(INFO) << "[PolicyPreferenceSyncer] EnableHistory: " << *enable_history;
  }

  if (auto enable_downloads = features->FindBool("EnableDownloads")) {
    // prefs_->SetBoolean("profile.features.enable_downloads",
    // *enable_downloads);
    LOG(INFO) << "[PolicyPreferenceSyncer] EnableDownloads: "
              << *enable_downloads;
  }
}

void PolicyPreferenceSyncer::SyncHomepage(const base::Value::Dict& policy) {
  const auto* homepage = policy.FindString("HomepageLocation");
  if (!homepage) {
    return;
  }

  LOG(INFO) << "[PolicyPreferenceSyncer] Updating homepage to: " << *homepage;
  // prefs_->SetString("homepage", *homepage);
}

bool PolicyPreferenceSyncer::IsActive() const {
  return active_;
}

}  // namespace policy_manager
