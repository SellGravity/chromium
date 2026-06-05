// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permission_sync/permission_cache_manager.h"
#include "chrome/browser/permission_sync/permission_sync_client.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"

namespace permission_sync {

// ============================================================================
// RuleSnapshot (immutable, shared across readers)
// ============================================================================

struct PermissionCacheManager::RuleSnapshot {
  RuleList all_rules;
  DomainRuleMap domain_rules;
  RuleList wildcard_rules;
  int version = 0;
  PermissionAction default_policy = PermissionAction::ALLOW;
};

// ============================================================================
// PendingRequest
// ============================================================================

struct PermissionCacheManager::PendingRequest {
  std::string domain;
  ResourceType resource_type;
  PendingCallback callback;
};

// ============================================================================
// Struct Constructors / Destructors (required by Chromium clang plugin)
// ============================================================================

PermissionRule::PermissionRule() = default;
PermissionRule::PermissionRule(const PermissionRule&) = default;
PermissionRule::PermissionRule(PermissionRule&&) = default;
PermissionRule& PermissionRule::operator=(const PermissionRule&) = default;
PermissionRule& PermissionRule::operator=(PermissionRule&&) = default;
PermissionRule::~PermissionRule() = default;

PermissionDecision::PermissionDecision() = default;
PermissionDecision::~PermissionDecision() = default;

// ============================================================================
// Construction / Destruction
// ============================================================================

PermissionCacheManager::PermissionCacheManager() {
  LOG(INFO) << "[PermissionCacheManager] Created. State: DISCONNECTED";
}

PermissionCacheManager::~PermissionCacheManager() {
  // SyncClient must be destroyed before CacheManager since it holds
  // a raw_ptr to us.
  sync_client_.reset();
}

void PermissionCacheManager::SetSyncClient(
    std::unique_ptr<PermissionSyncClient> client) {
  sync_client_ = std::move(client);
}

void PermissionCacheManager::Shutdown() {
  LOG(INFO) << "[PermissionCacheManager] Shutdown.";

  // Disconnect WebSocket before clearing cache.
  if (sync_client_) {
    sync_client_->Disconnect();
    sync_client_.reset();
  }

  // Stop timeout timer first.
  pending_timeout_timer_.Stop();

  // Resolve any pending requests with BLOCK before clearing cache.
  TimeoutPendingRequests();

  {
    base::AutoLock lock(lock_);
    snapshot_.reset();
    permission_version_.store(0, std::memory_order_release);
    default_policy_.store(PermissionAction::ALLOW,
                          std::memory_order_release);
  }
  connection_state_.store(ConnectionState::DISCONNECTED,
                          std::memory_order_release);
}

// ============================================================================
// Startup Rules (Decouple from WebSocket)
// ============================================================================

void PermissionCacheManager::SetSynchronizedFromStartup() {
  // Direct atomic store — bypass IsValidTransition because
  // DISCONNECTED → SYNCHRONIZED is not a valid WebSocket
  // protocol transition, but IS valid for startup loading.
  connection_state_.store(ConnectionState::SYNCHRONIZED,
                          std::memory_order_release);
  has_startup_rules_.store(true, std::memory_order_release);
  has_ever_synchronized_.store(true, std::memory_order_release);

  LOG(INFO) << "[PermissionCacheManager] SYNCHRONIZED from startup "
            << "(bypassed state machine — env var source)";

  // Drain any requests that might have queued during
  // the brief window between factory creation and this call.
  DrainPendingRequests();
}

bool PermissionCacheManager::HasStartupRules() const {
  return has_startup_rules_.load(std::memory_order_acquire);
}

// ============================================================================
// State Machine (FRS Section 4)
// ============================================================================

ConnectionState PermissionCacheManager::GetConnectionState() const {
  return connection_state_.load(std::memory_order_acquire);
}

// static
const char* PermissionCacheManager::StateToString(ConnectionState state) {
  // Fix #9: Safe switch-based conversion instead of array indexing.
  switch (state) {
    case ConnectionState::DISCONNECTED:
      return "DISCONNECTED";
    case ConnectionState::CONNECTING:
      return "CONNECTING";
    case ConnectionState::SYNCING:
      return "SYNCING";
    case ConnectionState::SYNCHRONIZED:
      return "SYNCHRONIZED";
  }
  return "UNKNOWN";
}

// static
bool PermissionCacheManager::IsValidTransition(ConnectionState from,
                                                ConnectionState to) {
  // Fix #5: Validate state transitions per FRS Section 4.
  switch (from) {
    case ConnectionState::DISCONNECTED:
      return to == ConnectionState::CONNECTING;
    case ConnectionState::CONNECTING:
      return to == ConnectionState::SYNCING ||
             to == ConnectionState::DISCONNECTED;
    case ConnectionState::SYNCING:
      return to == ConnectionState::SYNCHRONIZED ||
             to == ConnectionState::DISCONNECTED;
    case ConnectionState::SYNCHRONIZED:
      return to == ConnectionState::SYNCING ||
             to == ConnectionState::DISCONNECTED;
  }
  return false;
}

void PermissionCacheManager::SetConnectionState(ConnectionState new_state) {
  ConnectionState old_state =
      connection_state_.load(std::memory_order_acquire);

  // Fix #5: Validate transition.
  if (!IsValidTransition(old_state, new_state)) {
    LOG(ERROR) << "[PermissionCacheManager] Invalid state transition: "
               << StateToString(old_state) << " → "
               << StateToString(new_state) << " (rejected)";
    return;
  }

  connection_state_.store(new_state, std::memory_order_release);

  DVLOG(1) << "[Cache] State: " << StateToString(old_state)
            << " -> " << StateToString(new_state);

  // Fix #1: When transitioning to SYNCHRONIZED, drain pending requests.
  if (new_state == ConnectionState::SYNCHRONIZED) {
    has_ever_synchronized_.store(true, std::memory_order_release);
    DrainPendingRequests();
  }
}

bool PermissionCacheManager::HasEverSynchronized() const {
  return has_ever_synchronized_.load(std::memory_order_acquire);
}

// ============================================================================
// Cache Management (FRS Section 3.4.3)
// ============================================================================

namespace {

// Parse ResourceType from string.
ResourceType ParseResourceType(const std::string& type_str) {
  if (type_str == "MAIN_FRAME") return ResourceType::MAIN_FRAME;
  if (type_str == "SUB_FRAME") return ResourceType::SUB_FRAME;
  if (type_str == "SCRIPT") return ResourceType::SCRIPT;
  if (type_str == "IMAGE") return ResourceType::IMAGE;
  if (type_str == "STYLESHEET") return ResourceType::STYLESHEET;
  if (type_str == "XHR") return ResourceType::XHR;
  if (type_str == "MEDIA") return ResourceType::MEDIA;
  if (type_str == "WEBSOCKET") return ResourceType::WEBSOCKET;
  return ResourceType::OTHER;
}

// Parse PermissionAction from string.
PermissionAction ParseAction(const std::string& action_str) {
  if (action_str == "ALLOW") return PermissionAction::ALLOW;
  return PermissionAction::BLOCK;
}

// Parse a single rule from JSON dict.
std::optional<PermissionRule> ParseRule(const base::Value::Dict& rule_dict) {
  const std::string* id = rule_dict.FindString("id");
  const std::string* pattern = rule_dict.FindString("pattern");
  const std::string* action_str = rule_dict.FindString("action");

  if (!id || !pattern || !action_str) {
    LOG(WARNING) << "[PermissionCacheManager] Skipping rule: missing fields";
    return std::nullopt;
  }

  PermissionRule rule;
  rule.id = *id;
  rule.pattern = *pattern;
  rule.action = ParseAction(*action_str);
  rule.priority = rule_dict.FindInt("priority").value_or(0);

  // Parse resource_types array (optional — empty means all types)
  const base::Value::List* types_list =
      rule_dict.FindList("resource_types");
  if (types_list) {
    for (const auto& type_val : *types_list) {
      if (type_val.is_string()) {
        rule.resource_types.insert(ParseResourceType(type_val.GetString()));
      }
    }
  }

  return rule;
}

}  // namespace

bool PermissionCacheManager::LoadFullPermissionSet(
    const std::string& json_data) {
  auto parsed = base::JSONReader::Read(json_data, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[PermissionCacheManager] LoadFullPermissionSet: "
               << "invalid JSON data";
    return false;
  }
  return LoadFullPermissionSet(parsed->GetDict());
}

bool PermissionCacheManager::LoadFullPermissionSet(
    const base::Value::Dict& root) {
  // FRS 3.4.3: Parse and validate BEFORE acquiring write lock.
  int new_version = root.FindInt("version").value_or(0);

  const std::string* default_str = root.FindString("default_policy");
  PermissionAction new_default = PermissionAction::ALLOW;
  if (default_str) {
    new_default = ParseAction(*default_str);
  }

  RuleList new_rules;
  const base::Value::List* rules_list = root.FindList("rules");
  if (rules_list) {
    new_rules.reserve(rules_list->size());
    for (const auto& rule_val : *rules_list) {
      if (rule_val.is_dict()) {
        auto rule = ParseRule(rule_val.GetDict());
        if (rule.has_value()) {
          new_rules.push_back(std::move(rule.value()));
        }
      }
    }
  }

  LOG(INFO) << "[PermissionCacheManager] Full sync: " << new_rules.size()
            << " rules, version " << new_version;

  auto new_snapshot =
      BuildSnapshot(std::move(new_rules), new_version, new_default);

  {
    base::AutoLock lock(lock_);
    snapshot_ = std::move(new_snapshot);
    permission_version_.store(new_version, std::memory_order_release);
    default_policy_.store(new_default, std::memory_order_release);
  }

  return true;
}

bool PermissionCacheManager::ApplyDeltaUpdate(const std::string& json_data) {
  auto parsed = base::JSONReader::Read(json_data, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[PermissionCacheManager] ApplyDeltaUpdate: invalid JSON";
    return false;
  }
  return ApplyDeltaUpdate(parsed->GetDict());
}

bool PermissionCacheManager::ApplyDeltaUpdate(
    const base::Value::Dict& root) {
  // FRS 3.3.2: Validate BEFORE acquiring lock.
  int new_version = root.FindInt("version").value_or(0);
  const std::string* change_type = root.FindString("change_type");

  if (!change_type) {
    LOG(ERROR) << "[PermissionCacheManager] ApplyDeltaUpdate: "
               << "missing change_type";
    return false;
  }

  int current_version = permission_version_.load(std::memory_order_acquire);

  // Idempotent: if we already have this version, treat as success
  // (server retry of an already-applied update).
  if (new_version == current_version) {
    LOG(INFO) << "[PermissionCacheManager] Delta update already applied: "
              << "version " << new_version << " (duplicate/retry, ignoring)";
    return true;
  }

  // Version gap: incoming must be exactly current + 1.
  if (new_version != current_version + 1) {
    LOG(WARNING) << "[PermissionCacheManager] Version gap detected: "
                 << "current=" << current_version
                 << " incoming=" << new_version
                 << ". Full sync required.";
    return false;
  }

  const base::Value::List* affected = root.FindList("affected_rules");
  if (!affected) {
    LOG(ERROR) << "[PermissionCacheManager] ApplyDeltaUpdate: "
               << "missing affected_rules";
    return false;
  }

  {
    base::AutoLock lock(lock_);

    RuleList rules;
    PermissionAction current_default = PermissionAction::ALLOW;
    if (snapshot_) {
      rules = snapshot_->all_rules;
      current_default = snapshot_->default_policy;
    }

    if (*change_type == "ADD" || *change_type == "MODIFY") {
      for (const auto& rule_val : *affected) {
        if (!rule_val.is_dict()) continue;
        auto rule = ParseRule(rule_val.GetDict());
        if (!rule.has_value()) continue;

        std::erase_if(rules, [&](const PermissionRule& r) {
          return r.id == rule->id;
        });
        rules.push_back(std::move(rule.value()));
      }
    } else if (*change_type == "DELETE") {
      for (const auto& rule_val : *affected) {
        if (!rule_val.is_dict()) continue;
        const std::string* rule_id = rule_val.GetDict().FindString("id");
        if (rule_id) {
          std::erase_if(rules, [&](const PermissionRule& r) {
            return r.id == *rule_id;
          });
        }
      }
    }

    auto new_snapshot =
        BuildSnapshot(std::move(rules), new_version, current_default);
    snapshot_ = std::move(new_snapshot);

    permission_version_.store(new_version, std::memory_order_release);
  }

  LOG(INFO) << "[PermissionCacheManager] Delta update applied: "
            << *change_type << ", version " << new_version;
  return true;
}

// ============================================================================
// Permission Check (FRS Section 3.4.2)
// ============================================================================

void PermissionCacheManager::EvaluateRequestAsync(
    const std::string& domain,
    ResourceType resource_type,
    PendingCallback callback) {
  // Fix #1: Single async API. No PENDING action returned.
  ConnectionState state = GetConnectionState();

  if (state == ConnectionState::SYNCHRONIZED) {
    // Hot path (99.9%): evaluate from cache, invoke callback synchronously.
    // No overhead compared to a sync call.
    std::shared_ptr<const RuleSnapshot> snap;
    {
      base::AutoLock lock(lock_);
      snap = snapshot_;
    }

    if (snap) {
      std::move(callback).Run(
          EvaluateAgainstSnapshot(*snap, domain, resource_type));
    } else {
      // Snapshot null but SYNCHRONIZED — shouldn't happen. Fail-secure.
      PermissionDecision decision;
      decision.action = PermissionAction::BLOCK;
      decision.reason = "SYNCHRONIZED but cache empty (internal error)";
      std::move(callback).Run(std::move(decision));
    }
    return;
  }

  if (state == ConnectionState::DISCONNECTED) {
    // KEY CHANGE: If we have startup rules, use them instead of BLOCK.
    // This is the core fix for the 1001 deadlock — browser continues
    // operating with cached rules even when WebSocket is down.
    if (HasStartupRules()) {
      std::shared_ptr<const RuleSnapshot> snap;
      {
        base::AutoLock lock(lock_);
        snap = snapshot_;
      }
      if (snap) {
        std::move(callback).Run(
            EvaluateAgainstSnapshot(*snap, domain, resource_type));
        return;
      }
    }

    // No startup rules + DISCONNECTED = true fail-secure.
    PermissionDecision decision;
    decision.action = PermissionAction::BLOCK;
    decision.reason = "DISCONNECTED: no cached rules available";
    std::move(callback).Run(std::move(decision));
    return;
  }

  // CONNECTING | SYNCING → queue request, callback deferred.
  QueuePendingRequest(domain, resource_type, std::move(callback));
}

// ============================================================================
// Synchronous Cache-Only Evaluation
// ============================================================================

PermissionDecision PermissionCacheManager::EvaluateFromCache(
    const std::string& domain,
    ResourceType resource_type) {
  std::shared_ptr<const RuleSnapshot> snap;
  {
    base::AutoLock lock(lock_);
    snap = snapshot_;
  }

  if (snap) {
    return EvaluateAgainstSnapshot(*snap, domain, resource_type);
  }

  // Fail-secure: no snapshot available.
  PermissionDecision decision;
  decision.action = PermissionAction::BLOCK;
  decision.reason = "Cache empty (internal error)";
  return decision;
}
// ============================================================================
// Snapshot-Based Evaluation (lock-free, static)
// fix matching-domain logic, including wildcard support (*.example.com)
// ============================================================================

// static
PermissionDecision PermissionCacheManager::EvaluateAgainstSnapshot(
    const RuleSnapshot& snapshot,
    const std::string& domain,
    ResourceType resource_type) {
  PermissionDecision decision;

  // Convert domain to lowercase for case-insensitive matching.
  std::string lower_domain = base::ToLowerASCII(domain);

  // Best match tracking (highest priority rule wins across all lookups).
  const PermissionRule* best_match = nullptr;

  // O(1) exact domain lookup + parent domain walk.
  // For "www.facebook.com", checks: "www.facebook.com", "facebook.com"
  // This ensures rule "facebook.com" also blocks "www.facebook.com",
  // "m.facebook.com", etc. — standard URL filtering behavior.
  std::string lookup_domain = lower_domain;
  while (!lookup_domain.empty()) {
    auto it = snapshot.domain_rules.find(lookup_domain);
    if (it != snapshot.domain_rules.end()) {
      for (const auto& rule : it->second) {
        if (!MatchesResourceType(rule, resource_type))
          continue;
        if (!best_match || rule.priority > best_match->priority) {
          best_match = &rule;
        }
      }
    }
    // Strip one subdomain level: "www.facebook.com" → "facebook.com"
    size_t dot_pos = lookup_domain.find('.');
    if (dot_pos == std::string::npos) {
      break;  // No more subdomains to strip (e.g., "com")
    }
    lookup_domain = lookup_domain.substr(dot_pos + 1);
    // Stop at TLD (single label like "com" has no dots)
    if (lookup_domain.find('.') == std::string::npos) {
      break;
    }
  }

  // Check wildcard rules (*.domain patterns).
  for (const auto& rule : snapshot.wildcard_rules) {
    if (!MatchesDomain(lower_domain, rule.pattern))
      continue;
    if (!MatchesResourceType(rule, resource_type))
      continue;
    if (!best_match || rule.priority > best_match->priority) {
      best_match = &rule;
    }
  }

  // Apply matched rule or default policy.
  if (best_match) {
    decision.action = best_match->action;
    decision.matched_rule_id = best_match->id;
    decision.reason = "Matched rule: " + best_match->pattern;
  } else {
    decision.action = snapshot.default_policy;
    decision.reason = "Default policy";
  }

  return decision;
}


// ============================================================================
// Pending Queue Management (Fix #1)
// ============================================================================

void PermissionCacheManager::QueuePendingRequest(
    const std::string& domain,
    ResourceType resource_type,
    PendingCallback callback) {
  {
    base::AutoLock lock(pending_lock_);

    // Start timeout timer on first pending request.
    if (pending_requests_.empty()) {
      pending_timeout_timer_.Start(
          FROM_HERE, base::Seconds(kPendingTimeoutSeconds),
          base::BindOnce(&PermissionCacheManager::TimeoutPendingRequests,
                         base::Unretained(this)));
    }

    PendingRequest request;
    request.domain = domain;
    request.resource_type = resource_type;
    request.callback = std::move(callback);
    pending_requests_.push_back(std::move(request));
  }

  DVLOG(1) << "[PermissionCacheManager] Queued pending request for: "
           << domain;
}

void PermissionCacheManager::DrainPendingRequests() {
  // Stop timeout timer.
  pending_timeout_timer_.Stop();

  // Grab current snapshot.
  std::shared_ptr<const RuleSnapshot> snap;
  {
    base::AutoLock lock(lock_);
    snap = snapshot_;
  }

  // Move pending requests out under lock, then evaluate without lock.
  std::vector<PendingRequest> requests;
  {
    base::AutoLock lock(pending_lock_);
    requests = std::move(pending_requests_);
    pending_requests_.clear();
  }

  if (requests.empty()) return;

  LOG(INFO) << "[PermissionCacheManager] Draining " << requests.size()
            << " pending requests";

  for (auto& request : requests) {
    if (snap) {
      std::move(request.callback)
          .Run(EvaluateAgainstSnapshot(*snap, request.domain,
                                       request.resource_type));
    } else {
      PermissionDecision decision;
      decision.action = PermissionAction::BLOCK;
      decision.reason = "Cache empty after sync (internal error)";
      std::move(request.callback).Run(std::move(decision));
    }
  }
}

void PermissionCacheManager::TimeoutPendingRequests() {
  std::vector<PendingRequest> requests;
  {
    base::AutoLock lock(pending_lock_);
    requests = std::move(pending_requests_);
    pending_requests_.clear();
  }

  if (requests.empty()) return;

  LOG(WARNING) << "[PermissionCacheManager] Timing out "
               << requests.size() << " pending requests after "
               << kPendingTimeoutSeconds << "s";

  for (auto& request : requests) {
    PermissionDecision decision;
    decision.action = PermissionAction::BLOCK;
    decision.reason = "Timeout: sync did not complete within " +
                      std::to_string(kPendingTimeoutSeconds) + "s";
    std::move(request.callback).Run(std::move(decision));
  }
}

// ============================================================================
// Version & Policy Accessors
// ============================================================================

int PermissionCacheManager::GetPermissionVersion() const {
  return permission_version_.load(std::memory_order_acquire);
}

void PermissionCacheManager::SetDefaultPolicy(PermissionAction policy) {
  base::AutoLock lock(lock_);
  default_policy_.store(policy, std::memory_order_release);
}

PermissionAction PermissionCacheManager::GetDefaultPolicy() const {
  return default_policy_.load(std::memory_order_acquire);
}

size_t PermissionCacheManager::GetRuleCount() const {
  base::AutoLock lock(lock_);
  return snapshot_ ? snapshot_->all_rules.size() : 0;
}

// ============================================================================
// Internal Helpers
// ============================================================================

// static
std::string PermissionCacheManager::ExtractDomain(
    const std::string& pattern) {
  std::string lower = base::ToLowerASCII(pattern);
  // Remove wildcard prefix: "*.example.com" → "example.com"
  if (lower.length() > 2 && lower[0] == '*' && lower[1] == '.') {
    return lower.substr(2);
  }
  // Remove protocol prefix if present: "https://example.com" → "example.com"
  size_t proto_end = lower.find("://");
  if (proto_end != std::string::npos) {
    lower = lower.substr(proto_end + 3);
  }
  // Remove path: "example.com/path" → "example.com"
  size_t path_start = lower.find('/');
  if (path_start != std::string::npos) {
    lower = lower.substr(0, path_start);
  }
  return lower;
}

// static
bool PermissionCacheManager::MatchesDomain(const std::string& domain,
                                            const std::string& pattern) {
  std::string lower_pattern = base::ToLowerASCII(pattern);

  // Catch-all wildcard
  if (lower_pattern == "*") return true;

  // Wildcard subdomain: "*.example.com" matches "sub.example.com"
  // and "example.com" itself.
  if (lower_pattern.length() > 2 &&
      lower_pattern[0] == '*' && lower_pattern[1] == '.') {
    std::string base_domain = lower_pattern.substr(2);
    // Exact match: "example.com" matches "*.example.com"
    if (domain == base_domain) return true;
    // Subdomain match: "sub.example.com" ends with ".example.com"
    // Fix #2: Proper dot-boundary check. "notevil.com" does NOT match
    // "*.evil.com" because the char before "evil.com" must be '.'.
    return domain.length() > base_domain.length() &&
           domain.compare(domain.length() - base_domain.length(),
                          base_domain.length(), base_domain) == 0 &&
           domain[domain.length() - base_domain.length() - 1] == '.';
  }

  // Fix #2: Exact domain match ONLY — no substring match.
  // "evil.com" pattern must NOT match "notevil.com".
  std::string pattern_domain = ExtractDomain(lower_pattern);
  return domain == pattern_domain;
}

// static
bool PermissionCacheManager::MatchesResourceType(
    const PermissionRule& rule,
    ResourceType resource_type) {
  // Empty set = rule applies to all resource types.
  if (rule.resource_types.empty()) return true;
  return rule.resource_types.contains(resource_type);
}

// static
void PermissionCacheManager::SortRulesByPriority(RuleList& rules) {
  std::sort(rules.begin(), rules.end(),
            [](const PermissionRule& a, const PermissionRule& b) {
              return a.priority > b.priority;  // Descending
            });
}

// static
std::shared_ptr<const PermissionCacheManager::RuleSnapshot>
PermissionCacheManager::BuildSnapshot(RuleList rules,
                                       int version,
                                       PermissionAction default_policy) {
  auto snapshot = std::make_shared<RuleSnapshot>();
  snapshot->all_rules = std::move(rules);
  snapshot->version = version;
  snapshot->default_policy = default_policy;

  // Build indexes from all_rules.
  for (const auto& rule : snapshot->all_rules) {
    if (rule.pattern.length() > 2 &&
        rule.pattern[0] == '*' && rule.pattern[1] == '.') {
      // Wildcard pattern → wildcard list (full copy for snapshot ownership)
      snapshot->wildcard_rules.push_back(rule);
    } else if (rule.pattern == "*") {
      // Catch-all → wildcard list
      snapshot->wildcard_rules.push_back(rule);
    } else {
      // Exact domain → indexed by domain for O(1) lookup
      std::string domain = ExtractDomain(rule.pattern);
      snapshot->domain_rules[domain].push_back(rule);
    }
  }

  // Sort all rule lists by priority (descending).
  for (auto& [domain, domain_rules] : snapshot->domain_rules) {
    SortRulesByPriority(domain_rules);
  }
  SortRulesByPriority(snapshot->wildcard_rules);

  DVLOG(1) << "[PermissionCacheManager] Snapshot built: "
           << snapshot->domain_rules.size() << " domains, "
           << snapshot->wildcard_rules.size() << " wildcard rules, "
           << "version " << version;

  return snapshot;
}

}  // namespace permission_sync
