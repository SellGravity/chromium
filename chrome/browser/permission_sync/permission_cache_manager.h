// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_CACHE_MANAGER_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_CACHE_MANAGER_H_

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/keyed_service/core/keyed_service.h"


namespace permission_sync {

class PermissionSyncClient;

// ============================================================================
// Connection State Machine (FRS Section 4)
// ============================================================================
// Valid state transitions (any other transition is rejected):
//   DISCONNECTED → CONNECTING  (process start / reconnect timer)
//   CONNECTING   → SYNCING     (CONNECTED message received)
//   CONNECTING   → DISCONNECTED (connection fails / timeout)
//   SYNCING      → SYNCHRONIZED (full sync received + parsed)
//   SYNCING      → DISCONNECTED (connection lost during sync)
//   SYNCHRONIZED → DISCONNECTED (connection lost)
//   SYNCHRONIZED → SYNCING      (version mismatch in heartbeat)
// ============================================================================
enum class ConnectionState {
  DISCONNECTED,   // No WebSocket connection. Initial state.
  CONNECTING,     // WebSocket connection in progress. Handshake not complete.
  SYNCING,        // Connected, awaiting full permission sync.
  SYNCHRONIZED,   // Fully operational. Cache is valid and up-to-date.
};

// ============================================================================
// Permission Action (FRS Section 3.2.2)
// ============================================================================
enum class PermissionAction {
  ALLOW,
  BLOCK,
};

// ============================================================================
// Resource Types (FRS Section 3.2.2)
// ============================================================================
enum class ResourceType {
  MAIN_FRAME,
  SUB_FRAME,
  SCRIPT,
  IMAGE,
  STYLESHEET,
  XHR,
  MEDIA,
  WEBSOCKET,
  OTHER,
};

// ============================================================================
// Permission Rule (FRS Section 3.2.2)
// ============================================================================
// Each rule defines an access control entry for URL pattern matching.
// Rules are evaluated by priority (higher = first). First match wins.
struct PermissionRule {
  PermissionRule();
  PermissionRule(const PermissionRule&);
  PermissionRule(PermissionRule&&);
  PermissionRule& operator=(const PermissionRule&);
  PermissionRule& operator=(PermissionRule&&);
  ~PermissionRule();

  std::string id;       // Unique rule identifier (UUID)
  std::string pattern;  // URL matching pattern (*.example.com, example.com)
  PermissionAction action = PermissionAction::BLOCK;
  int priority = 0;     // Higher value = evaluated first
  std::unordered_set<ResourceType> resource_types;  // Empty = all types
};

// ============================================================================
// Permission Check Result
// ============================================================================
struct PermissionDecision {
  PermissionDecision();
  ~PermissionDecision();

  PermissionAction action = PermissionAction::ALLOW;
  std::string matched_rule_id;  // Empty if default policy applied
  std::string reason;
};

// ============================================================================
// PermissionCacheManager (FRS Section 3.4)
// ============================================================================
// Manages in-memory permission cache for a browser profile.
// Provides O(1) average-case URL permission lookups.
//
// Thread Safety (FRS Section 3.4.1 & 3.4.3):
//   - Reads use a snapshot-based pattern: grab std::shared_ptr<const
//     RuleSnapshot> under lock (sub-microsecond), then evaluate entirely
//     outside the lock. Multiple readers never block each other, and
//     readers are never stalled by writes.
//   - Writes (LoadFullPermissionSet / ApplyDeltaUpdate) acquire an
//     exclusive lock, build a new immutable snapshot, and atomically
//     swap the shared_ptr.
//
// Usage:
//   manager->EvaluateRequestAsync("facebook.com",
//                                 ResourceType::MAIN_FRAME,
//                                 base::BindOnce(&OnDecision));
//
class PermissionCacheManager : public KeyedService {
 public:
  using PendingCallback = base::OnceCallback<void(PermissionDecision)>;

  PermissionCacheManager();
  ~PermissionCacheManager() override;

  PermissionCacheManager(const PermissionCacheManager&) = delete;
  PermissionCacheManager& operator=(const PermissionCacheManager&) = delete;

  // KeyedService:
  void Shutdown() override;

  // Transfers ownership of the SyncClient to this CacheManager.
  // Called by PermissionCacheManagerFactory after creation.
  void SetSyncClient(std::unique_ptr<PermissionSyncClient> client);

  // Returns a raw pointer to the owned SyncClient.
  // Used by factory's deferred connect to call Connect().
  PermissionSyncClient* GetSyncClient() { return sync_client_.get(); }

  // Called by PermissionStartupLoader to set SYNCHRONIZED state
  // at startup, bypassing normal state machine validation.
  // This is safe because:
  //   1. Only called once, during factory construction
  //   2. Only called after LoadFullPermissionSet succeeds
  //   3. Env var is a trusted local source (not network data)
  void SetSynchronizedFromStartup();

  // Returns true if rules were loaded at startup (env var or file).
  // When true, DISCONNECTED state should NOT block browser —
  // use cached rules instead.
  bool HasStartupRules() const;

  // Returns a WeakPtr for use in PostDelayedTask (deferred WS connect).
  base::WeakPtr<PermissionCacheManager> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // --- State Machine (FRS Section 4) ---

  // Returns the current connection state. Thread-safe (atomic read).
  ConnectionState GetConnectionState() const;

  // Transitions to a new connection state. Thread-safe (atomic write).
  // Validates transitions per FRS Section 4. Invalid transitions are
  // rejected with LOG(ERROR).
  // When transitioning to SYNCHRONIZED, drains any pending requests.
  void SetConnectionState(ConnectionState new_state);

  // --- Cache Management (FRS Section 3.4.3) ---

  // Replaces the entire permission cache with data from a full sync.
  // Builds a new immutable RuleSnapshot and swaps atomically.
  // Returns true on success, false on parse/validation failure.
  //
  // Prefer the Dict& overload from PermissionSyncClient to avoid
  // redundant serialize/deserialize roundtrips.
  bool LoadFullPermissionSet(const std::string& json_data);
  bool LoadFullPermissionSet(const base::Value::Dict& data);

  // Applies an incremental update to the cache.
  // Builds a new immutable RuleSnapshot and swaps atomically.
  // Returns true on success, false on parse failure or version gap.
  // When false is returned due to version gap, caller should request
  // a full sync.
  bool ApplyDeltaUpdate(const std::string& json_data);
  bool ApplyDeltaUpdate(const base::Value::Dict& data);

  // --- Permission Check (FRS Section 3.4.2) ---

  // Main API for permission checks. Single async interface.
  //
  // Behavior per connection state:
  //   SYNCHRONIZED → Callback invoked synchronously with cache result.
  //   CONNECTING   → Request queued, callback deferred until sync.
  //   SYNCING      → Request queued, callback deferred until sync.
  //   DISCONNECTED → Callback invoked synchronously with BLOCK.
  //
  // Pending requests time out after kPendingTimeoutSeconds (5s) per
  // FRS Section 3.5.1. Timed-out requests receive BLOCK.
  //
  // Thread-safe: snapshot grabbed under lock, evaluated outside lock.
  void EvaluateRequestAsync(const std::string& domain,
                            ResourceType resource_type,
                            PendingCallback callback);

  // Synchronous cache-only evaluation. Does NOT check connection state
  // or queue requests. Caller MUST verify state == SYNCHRONIZED before
  // calling. Returns BLOCK if cache is empty (fail-secure).
  //
  // Thread-safe: grabs snapshot under lock, evaluates lock-free.
  PermissionDecision EvaluateFromCache(const std::string& domain,
                                       ResourceType resource_type);

  // --- Version (FRS Section 3.2.1) ---

  // Returns the current permission version. Thread-safe (atomic read).
  int GetPermissionVersion() const;

  // --- Default Policy ---

  void SetDefaultPolicy(PermissionAction policy);
  PermissionAction GetDefaultPolicy() const;

  // Returns true if at least one successful sync has completed.
  // Used to differentiate startup-DISCONNECTED (defer) from
  // connection-loss-DISCONNECTED (fail-secure block).
  bool HasEverSynchronized() const;

  // --- Stats ---
  size_t GetRuleCount() const;

 private:
  // Immutable snapshot of permission rules (FRS 3.4.1).
  // Once created, never modified. Shared across readers via shared_ptr.
  struct RuleSnapshot;

  // Internal rule storage types.
  using RuleList = std::vector<PermissionRule>;
  using DomainRuleMap = std::unordered_map<std::string, RuleList>;

  // Pending request held during CONNECTING/SYNCING states.
  struct PendingRequest;

  // Timeout for pending requests (FRS 3.5.1: 5 seconds).
  static constexpr int kPendingTimeoutSeconds = 5;

  // --- Private Evaluation ---

  // Evaluates a request against a snapshot. No locking needed.
  static PermissionDecision EvaluateAgainstSnapshot(
      const RuleSnapshot& snapshot,
      const std::string& domain,
      ResourceType resource_type);

  // --- Pending Queue Management ---

  // Queues a request for later evaluation.
  void QueuePendingRequest(const std::string& domain,
                           ResourceType resource_type,
                           PendingCallback callback);

  // Evaluates and resolves all pending requests against current snapshot.
  // Called when state transitions to SYNCHRONIZED.
  void DrainPendingRequests();

  // Resolves all pending requests with BLOCK (timeout or shutdown).
  void TimeoutPendingRequests();

  // --- State Validation (FRS Section 4) ---

  static bool IsValidTransition(ConnectionState from, ConnectionState to);
  static const char* StateToString(ConnectionState state);

  // --- Domain Matching Helpers ---

  static std::string ExtractDomain(const std::string& pattern);

  // Returns true if |pattern| matches |domain|.
  // Security: uses exact match or proper subdomain boundary (dot-separated).
  // Never uses substring match.
  static bool MatchesDomain(const std::string& domain,
                             const std::string& pattern);

  static bool MatchesResourceType(const PermissionRule& rule,
                                   ResourceType resource_type);
  static void SortRulesByPriority(RuleList& rules);

  // Builds a new immutable RuleSnapshot from the given rules.
  static std::shared_ptr<const RuleSnapshot> BuildSnapshot(
      RuleList rules,
      int version,
      PermissionAction default_policy);

  // --- Connection State (atomic, no lock needed) ---
  std::atomic<ConnectionState> connection_state_{
      ConnectionState::DISCONNECTED};

  // --- Snapshot (protected by lock_) ---
  // Reads: grab shared_ptr under lock (sub-µs), evaluate outside lock.
  // Writes: build new snapshot, swap under lock.
  mutable base::Lock lock_;
  std::shared_ptr<const RuleSnapshot> snapshot_ GUARDED_BY(lock_);

  // Version and default policy — updated INSIDE lock_ scope to avoid
  // race conditions (fix #3). Atomic for lock-free reads by heartbeat.
  std::atomic<int> permission_version_{0};
  std::atomic<PermissionAction> default_policy_{PermissionAction::ALLOW};

  // Tracks whether at least one sync has completed (startup vs reconnect).
  std::atomic<bool> has_ever_synchronized_{false};

  // True if rules were loaded from startup source (env var / file).
  // Distinct from has_ever_synchronized_: this specifically tracks
  // whether the INITIAL load came from a non-WebSocket source.
  std::atomic<bool> has_startup_rules_{false};

  // --- Pending Request Queue ---
  base::Lock pending_lock_;
  std::vector<PendingRequest> pending_requests_ GUARDED_BY(pending_lock_);
  base::OneShotTimer pending_timeout_timer_;

  // --- WebSocket Client (owned) ---
  std::unique_ptr<PermissionSyncClient> sync_client_;

  base::WeakPtrFactory<PermissionCacheManager> weak_factory_{this};
};

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_CACHE_MANAGER_H_
