// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy_manager/policy_ipc_client.h"

#include <windows.h>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/json_common.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"

namespace policy_manager {

namespace {

// Singleton instance
PolicyIPCClient* g_instance = nullptr;

constexpr int kBufferSize = 4096;

}  // namespace

PolicyIPCClient::PolicyIPCClient() {
  DCHECK(!g_instance);
  g_instance = this;
}

PolicyIPCClient::~PolicyIPCClient() {
  StopPolicyWatcher();
  g_instance = nullptr;
}

// static
PolicyIPCClient* PolicyIPCClient::GetInstance() {
  return g_instance;
}

bool PolicyIPCClient::Initialize(const std::string& pipe_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  pipe_name_ = pipe_name;
  connected_ = true;  // Mark as connected to allow SendMessage

  // Test connection with a real heartbeat message (not just open/close)
  base::Value::Dict request;
  request.Set("action", "get_policy_version");

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  if (!SendMessage(request_json, &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to connect to pipe: " << pipe_name;
    connected_ = false;
    return false;
  }

  // SendMessage already sets last_successful_contact_ and consecutive_failures_
  LOG(INFO) << "[PolicyIPCClient] Connected to policy pipe: " << pipe_name;
  return true;
}

bool PolicyIPCClient::SendMessage(const std::string& request_json,
                                  std::string* response_json) {
  if (!connected_ || pipe_name_.empty()) {
    return false;
  }

  HANDLE hPipe = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE,
                             0, NULL, OPEN_EXISTING, 0, NULL);

  if (hPipe == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "[PolicyIPCClient] Failed to open pipe for message";
    return false;
  }

  // Set pipe mode
  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

  // Write request
  DWORD bytesWritten;
  BOOL success = WriteFile(hPipe, request_json.c_str(),
                           static_cast<DWORD>(request_json.size()),
                           &bytesWritten, NULL);

  if (!success) {
    CloseHandle(hPipe);
    return false;
  }

  // Read response
  char buffer[kBufferSize];
  DWORD bytesRead;
  success = ReadFile(hPipe, buffer, kBufferSize - 1, &bytesRead, NULL);

  CloseHandle(hPipe);

  if (!success || bytesRead == 0) {
    return false;
  }

  buffer[bytesRead] = '\0';
  *response_json = std::string(buffer);

  // Track successful contact for fail-closed security
  last_successful_contact_ = base::TimeTicks::Now();
  consecutive_failures_ = 0;

  return true;
}

PolicyDecision PolicyIPCClient::CheckURLSync(const std::string& url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  PolicyDecision decision;
  decision.allow = true;  // Default allow

  // ========== FAIL-CLOSED SECURITY CHECK ==========
  // If in lockdown mode (server offline + fail-closed enabled), BLOCK ALL
  if (IsInLockdownMode()) {
    decision.allow = false;
    decision.reason = "LOCKDOWN: Policy server unavailable (fail-closed security)";
    LOG(ERROR) << "[PolicyIPCClient] LOCKDOWN MODE: Blocking URL: " << url
               << " (server offline, fail-closed enabled)";
    return decision;
  }

  if (!connected_) {
    // Not connected but fail-open mode (or within grace period)
    if (!fail_closed_mode_) {
      LOG(WARNING) << "[PolicyIPCClient] Server not connected but fail-open mode "
                   << "- allowing URL: " << url;
    }
    return decision;
  }

  base::Value::Dict request;
  request.Set("action", "check_url");
  request.Set("url", url);
  if (!profile_name_.empty()) {
    request.Set("profile", profile_name_);
  }

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  if (!SendMessage(request_json, &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to check URL via IPC: " << url;

    // Track failure for fail-closed logic
    consecutive_failures_++;

    // If fail-closed mode and exceeded max failures, BLOCK
    if (fail_closed_mode_ && consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
      decision.allow = false;
      decision.reason = "Server connection failed (fail-closed security)";
      LOG(ERROR) << "[PolicyIPCClient] BLOCKING due to connection failures: " << url;
      return decision;
    }

    return decision;
  }

  auto parsed = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return decision;
  }

  const auto& dict = parsed->GetDict();
  if (auto allow = dict.FindBool("allow")) {
    decision.allow = *allow;
  }
  if (const auto* reason = dict.FindString("reason")) {
    decision.reason = *reason;
  }

  return decision;
}

PolicyDecision PolicyIPCClient::CheckURLSync(const std::string& url,
                                             const std::string& profile_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  PolicyDecision decision;
  decision.allow = true;  // Default allow

  // ========== FAIL-CLOSED SECURITY CHECK ==========
  // If in lockdown mode (server offline + fail-closed enabled), BLOCK ALL
  if (IsInLockdownMode()) {
    decision.allow = false;
    decision.reason = "LOCKDOWN: Policy server unavailable (fail-closed security)";
    LOG(ERROR) << "[PolicyIPCClient] LOCKDOWN MODE: Blocking URL: " << url
               << " | Profile: " << profile_name
               << " (server offline, fail-closed enabled)";
    return decision;
  }

  if (!connected_) {
    // Not connected but fail-open mode (or within grace period)
    if (!fail_closed_mode_) {
      LOG(WARNING) << "[PolicyIPCClient] Server not connected but fail-open mode "
                   << "- allowing URL: " << url << " | Profile: " << profile_name;
    }
    return decision;
  }

  base::Value::Dict request;
  request.Set("action", "check_url");
  request.Set("url", url);
  // Use explicit profile name parameter
  if (!profile_name.empty()) {
    request.Set("profile", profile_name);
  }

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  if (!SendMessage(request_json, &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to check URL via IPC: " << url
                 << " | Profile: " << profile_name;

    // Track failure for fail-closed logic
    consecutive_failures_++;

    // If fail-closed mode and exceeded max failures, BLOCK
    if (fail_closed_mode_ && consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
      decision.allow = false;
      decision.reason = "Server connection failed (fail-closed security)";
      LOG(ERROR) << "[PolicyIPCClient] BLOCKING due to connection failures: " << url
                 << " | Profile: " << profile_name;
      return decision;
    }

    return decision;
  }

  auto parsed = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return decision;
  }

  const auto& dict = parsed->GetDict();
  if (auto allow = dict.FindBool("allow")) {
    decision.allow = *allow;
  }
  if (const auto* reason = dict.FindString("reason")) {
    decision.reason = *reason;
  }

  return decision;
}

void PolicyIPCClient::CheckURLAsync(const std::string& url,
                                    PolicyCheckCallback callback) {
  // For now, run synchronously on UI thread
  // TODO: Implement proper async on background thread
  PolicyDecision decision = CheckURLSync(url);
  std::move(callback).Run(decision);
}

base::Value::Dict PolicyIPCClient::GetPolicyDictSync(const std::string& role) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::Value::Dict result;

  if (!connected_) {
    return result;
  }

  base::Value::Dict request;
  request.Set("action", "get_policy");
  request.Set("role", role);
  if (!profile_name_.empty()) {
    request.Set("profile", profile_name_);
  }

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  if (!SendMessage(request_json, &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to get policy for role: " << role;
    return result;
  }

  auto parsed = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (parsed && parsed->is_dict()) {
    return std::move(parsed->GetDict());
  }

  return result;
}

void PolicyIPCClient::RegisterPolicyUpdateListener(
    PolicyUpdateCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  policy_listeners_.push_back(std::move(callback));
}

void PolicyIPCClient::StartPolicyWatcher(int poll_interval_ms) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (watching_) {
    return;
  }

  watching_ = true;
  poll_interval_ms_ = poll_interval_ms;

  LOG(INFO) << "[PolicyIPCClient] Starting policy watcher (interval: "
            << poll_interval_ms << "ms)";

  ScheduleNextPolicyCheck(poll_interval_ms);
}

void PolicyIPCClient::StopPolicyWatcher() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  watching_ = false;
  weak_factory_.InvalidateWeakPtrs();
}

void PolicyIPCClient::ScheduleNextPolicyCheck(int delay_ms) {
  if (!watching_) {
    return;
  }

  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&PolicyIPCClient::WatchPolicyChanges,
                     weak_factory_.GetWeakPtr()),
      base::Milliseconds(delay_ms));
}

void PolicyIPCClient::WatchPolicyChanges() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!watching_ || !connected_) {
    return;
  }

  // Query current policy version
  base::Value::Dict request;
  request.Set("action", "get_policy_version");
  if (!profile_name_.empty()) {
    request.Set("profile", profile_name_);
  }

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  if (!SendMessage(request_json, &response_json)) {
    // Server might be down, continue watching
    ScheduleNextPolicyCheck(poll_interval_ms_);
    return;
  }

  auto response = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!response || !response->is_dict()) {
    ScheduleNextPolicyCheck(poll_interval_ms_);
    return;
  }

  const auto& dict = response->GetDict();
  int current_version = dict.FindInt("version").value_or(0);

  // Check if version changed
  if (current_version != last_policy_version_ && last_policy_version_ != 0) {
    LOG(INFO) << "[PolicyIPCClient] Policy version changed: "
              << last_policy_version_ << " -> " << current_version;

    // Get updated policy
    auto policy_dict = GetPolicyDictSync("user_role");

    // Notify all listeners
    for (auto& listener : policy_listeners_) {
      listener.Run(policy_dict);
    }
  }

  last_policy_version_ = current_version;

  // Schedule next check
  ScheduleNextPolicyCheck(poll_interval_ms_);
}

bool PolicyIPCClient::IsConnected() const {
  return connected_;
}

void PolicyIPCClient::SetProfileName(const std::string& profile_name) {
  profile_name_ = profile_name;
  LOG(INFO) << "[PolicyIPCClient] Profile name set to: " << profile_name;
}

// ========== ANTI-BYPASS / FAIL-CLOSED SECURITY IMPLEMENTATION ==========

void PolicyIPCClient::SetFailClosedMode(bool enabled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  fail_closed_mode_ = enabled;
  LOG(INFO) << "[PolicyIPCClient] Fail-closed mode: "
            << (enabled ? "ENABLED (secure)" : "DISABLED (unsafe)");

  if (enabled) {
    LOG(WARNING) << "[PolicyIPCClient] SECURITY: When server is unavailable, "
                 << "ALL URLs will be BLOCKED";
  } else {
    LOG(WARNING) << "[PolicyIPCClient] SECURITY WARNING: Fail-open mode allows "
                 << "bypass by killing server!";
  }
}

bool PolicyIPCClient::IsFailClosedMode() const {
  return fail_closed_mode_;
}

base::TimeDelta PolicyIPCClient::GetTimeSinceLastContact() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (last_successful_contact_.is_null()) {
    // Never connected
    return base::TimeDelta::Max();
  }

  return base::TimeTicks::Now() - last_successful_contact_;
}

bool PolicyIPCClient::IsServerAlive() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Simple heartbeat: try to get policy version
  base::Value::Dict request;
  request.Set("action", "get_policy_version");
  if (!profile_name_.empty()) {
    request.Set("profile", profile_name_);
  }

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  bool success = SendMessage(request_json, &response_json);

  if (success) {
    // Server responded - update last contact time
    last_successful_contact_ = base::TimeTicks::Now();
    consecutive_failures_ = 0;

    LOG(INFO) << "[PolicyIPCClient] Heartbeat: Server is ALIVE";
    return true;
  } else {
    // Server didn't respond
    consecutive_failures_++;

    LOG(WARNING) << "[PolicyIPCClient] Heartbeat: Server NOT responding "
                 << "(failures: " << consecutive_failures_ << ")";
    return false;
  }
}

void PolicyIPCClient::SetGracePeriod(base::TimeDelta grace_period) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  grace_period_ = grace_period;
  LOG(INFO) << "[PolicyIPCClient] Grace period set to: "
            << grace_period.InSeconds() << " seconds";
}

bool PolicyIPCClient::IsInLockdownMode() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Lockdown mode only applies if fail-closed is enabled
  if (!fail_closed_mode_) {
    return false;
  }

  // Check if we've exceeded max consecutive failures
  if (consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
    LOG(WARNING) << "[PolicyIPCClient] LOCKDOWN: Too many consecutive failures ("
                 << consecutive_failures_ << ")";
    return true;
  }

  // Check if grace period has expired
  base::TimeDelta time_since_contact = GetTimeSinceLastContact();
  if (time_since_contact > grace_period_) {
    LOG(WARNING) << "[PolicyIPCClient] LOCKDOWN: Grace period expired ("
                 << time_since_contact.InSeconds() << "s > "
                 << grace_period_.InSeconds() << "s)";
    return true;
  }

  return false;
}

}  // namespace policy_manager
