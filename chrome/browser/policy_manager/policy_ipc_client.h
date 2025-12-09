// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_POLICY_MANAGER_POLICY_IPC_CLIENT_H_
#define CHROME_BROWSER_POLICY_MANAGER_POLICY_IPC_CLIENT_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
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

// Client for communicating with external policy manager via HTTP REST API.
// This allows real-time policy updates from an external management server.
//
// HTTP is more stable and reliable than Named Pipes:
// - Connection pooling and keep-alive
// - Built-in timeout and retry handling
// - No pipe busy/timeout issues
// - Works across all platforms
// - Easy to debug with standard HTTP tools
class PolicyIPCClient {
 public:
  PolicyIPCClient();
  ~PolicyIPCClient();

  PolicyIPCClient(const PolicyIPCClient&) = delete;
  PolicyIPCClient& operator=(const PolicyIPCClient&) = delete;

  // Initialize connection to HTTP policy server
  // server_url: Base URL (e.g., "http://localhost:8765")
  bool Initialize(const std::string& server_url);

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

  // ========== ANTI-BYPASS / FAIL-CLOSED SECURITY ==========

  // Enable fail-closed mode (block all when server unavailable)
  // This prevents bypass by killing the server
  void SetFailClosedMode(bool enabled);

  // Check if fail-closed mode is enabled
  bool IsFailClosedMode() const;

  // Get time since last successful server contact
  base::TimeDelta GetTimeSinceLastContact() const;

  // Check if server is responsive (heartbeat)
  bool IsServerAlive();

  // Set grace period before enforcing fail-closed (default: 30s)
  void SetGracePeriod(base::TimeDelta grace_period);

  // Check if we're in lockdown mode (server offline + grace period expired)
  bool IsInLockdownMode() const;

  // ========== AUTO-RECONNECT ==========

  // Try to reconnect to server (called automatically on connection failure)
  bool TryReconnect();

  // Enable/disable auto-reconnect (default: enabled)
  void SetAutoReconnect(bool enabled);

  // Get singleton instance
  static PolicyIPCClient* GetInstance();

 private:
  // Send HTTP POST request and receive JSON response
  // Returns true if request succeeded, false otherwise
  bool SendHttpPost(const std::string& endpoint,
                    const std::string& request_json,
                    std::string* response_json);

  // Worker to poll policy changes
  void WatchPolicyChanges();

  // Schedule next policy check
  void ScheduleNextPolicyCheck(int delay_ms);

  std::string server_url_;     // HTTP server base URL (e.g., "http://localhost:8765")
  std::string profile_name_;   // For tracking which profile is making requests
  bool connected_ = false;
  bool watching_ = false;
  int poll_interval_ms_ = 5000;

  // Listeners for policy updates
  std::vector<PolicyUpdateCallback> policy_listeners_;

  // Track policy version to detect changes
  int last_policy_version_ = 0;

  // ========== FAIL-CLOSED SECURITY STATE ==========

  // Enable fail-closed mode (block all when server offline)
  bool fail_closed_mode_ = true;  // DEFAULT: ENABLED for security

  // Time of last successful server contact
  base::TimeTicks last_successful_contact_;

  // Grace period before enforcing strict lockdown (default: 30 seconds)
  base::TimeDelta grace_period_ = base::Seconds(30);

  // Track connection failures
  int consecutive_failures_ = 0;
  static constexpr int kMaxFailuresBeforeLockdown = 3;

  // ========== AUTO-RECONNECT STATE ==========

  // Enable auto-reconnect on connection failure
  bool auto_reconnect_enabled_ = true;  // DEFAULT: ENABLED

  // Time of last reconnect attempt (to prevent spam)
  base::TimeTicks last_reconnect_attempt_;

  // Minimum delay between reconnect attempts (default: 5 seconds)
  base::TimeDelta reconnect_delay_ = base::Seconds(5);

  // ========== URL CACHE (to prevent repeated server calls) ==========
  struct CachedDecision {
    PolicyDecision decision;
    base::TimeTicks timestamp;
  };
  
  // Cache URL decisions for 30 seconds to reduce server load
  std::unordered_map<std::string, CachedDecision> url_cache_;
  base::TimeDelta cache_ttl_ = base::Seconds(30);
  
  // Get cached decision if still valid
  bool GetCachedDecision(const std::string& cache_key, PolicyDecision* decision);
  
  // Store decision in cache
  void CacheDecision(const std::string& cache_key, const PolicyDecision& decision);

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<PolicyIPCClient> weak_factory_{this};
};

}  // namespace policy_manager

#endif  // CHROME_BROWSER_POLICY_MANAGER_POLICY_IPC_CLIENT_H_
