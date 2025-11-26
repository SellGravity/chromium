// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_POLICY_MANAGER_POLICY_IPC_CLIENT_H_
#define CHROME_BROWSER_POLICY_MANAGER_POLICY_IPC_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/thread.h"
#include "base/values.h"

namespace policy_manager {

// Decision returned by IPC policy check
struct PolicyDecision {
  bool allow = true;
  std::string reason;
};

// Callback for async policy checks
using PolicyCheckCallback =
    base::OnceCallback<void(const PolicyDecision& decision)>;

// Callback when policy is updated from server
using PolicyUpdateCallback =
    base::RepeatingCallback<void(const base::Value::Dict&)>;

// Client for communicating with external policy manager via Named Pipe (IPC).
// This allows real-time policy updates from an external management server.
class PolicyIPCClient {
 public:
  PolicyIPCClient();
  ~PolicyIPCClient();

  PolicyIPCClient(const PolicyIPCClient&) = delete;
  PolicyIPCClient& operator=(const PolicyIPCClient&) = delete;

  // Initialize connection to named pipe
  bool Initialize(const std::string& pipe_name);

  // Synchronous URL check - blocks until response
  PolicyDecision CheckURLSync(const std::string& url);

  // Synchronous URL check with explicit profile name (for multi-profile support)
  PolicyDecision CheckURLSync(const std::string& url,
                              const std::string& profile_name);

  // Asynchronous URL check
  void CheckURLAsync(const std::string& url, PolicyCheckCallback callback);

  // Get full policy dictionary for a role
  base::Value::Dict GetPolicyDictSync(const std::string& role);

  // Register listener for policy updates
  void RegisterPolicyUpdateListener(PolicyUpdateCallback callback);

  // Start polling server for policy changes
  void StartPolicyWatcher(int poll_interval_ms = 5000);

  // Stop watching policy changes
  void StopPolicyWatcher();

  // Check if connected to server
  bool IsConnected() const;

  // Set profile name for tracking (displayed in server logs)
  void SetProfileName(const std::string& profile_name);

  // Get singleton instance
  static PolicyIPCClient* GetInstance();

 private:
  // Send message to pipe and receive response
  bool SendMessage(const std::string& request_json,
                   std::string* response_json);

  // Worker to poll policy changes
  void WatchPolicyChanges();

  // Schedule next policy check
  void ScheduleNextPolicyCheck(int delay_ms);

  std::string pipe_name_;
  std::string profile_name_;  // For tracking which profile is making requests
  bool connected_ = false;
  bool watching_ = false;
  int poll_interval_ms_ = 5000;

  // Listeners for policy updates
  std::vector<PolicyUpdateCallback> policy_listeners_;

  // Track policy version to detect changes
  int last_policy_version_ = 0;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<PolicyIPCClient> weak_factory_{this};
};

}  // namespace policy_manager

#endif  // CHROME_BROWSER_POLICY_MANAGER_POLICY_IPC_CLIENT_H_
