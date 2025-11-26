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

  // Test connection
  HANDLE hPipe = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             NULL, OPEN_EXISTING, 0, NULL);

  if (hPipe == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    LOG(WARNING) << "[PolicyIPCClient] Failed to connect to pipe: "
                 << pipe_name << " (error: " << error << ")";
    return false;
  }

  CloseHandle(hPipe);
  connected_ = true;
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
  return true;
}

PolicyDecision PolicyIPCClient::CheckURLSync(const std::string& url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  PolicyDecision decision;
  decision.allow = true;

  if (!connected_) {
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

  if (!connected_) {
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
    LOG(WARNING) << "[PolicyIPCClient] Failed to check URL via IPC: " << url;
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

}  // namespace policy_manager
