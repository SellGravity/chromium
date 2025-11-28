// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// HTTP-based Policy IPC Client
// This is a simplified, more stable version using HTTP REST API instead of Named Pipes

#include "chrome/browser/policy_manager/policy_ipc_client.h"

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/json_common.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"

namespace policy_manager {

namespace {

// Singleton instance
PolicyIPCClient* g_instance = nullptr;

}  // namespace

PolicyIPCClient::PolicyIPCClient() {
  g_instance = this;
}

PolicyIPCClient::~PolicyIPCClient() {
  g_instance = nullptr;
}

PolicyIPCClient* PolicyIPCClient::GetInstance() {
  return g_instance;
}

bool PolicyIPCClient::Initialize(const std::string& server_url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  server_url_ = server_url;
  connected_ = true;

  // Test connection with health check
  std::string response_json;
  if (!SendHttpPost("/api/health", "{}", &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to connect to HTTP server: " << server_url;
    connected_ = false;
    return false;
  }

  // Parse health check response
  auto parsed = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (parsed && parsed->is_dict()) {
    const auto* status = parsed->GetDict().FindString("status");
    if (status && *status == "healthy") {
      last_successful_contact_ = base::TimeTicks::Now();
      consecutive_failures_ = 0;
      LOG(INFO) << "[PolicyIPCClient] Connected to HTTP policy server: " << server_url;
      return true;
    }
  }

  LOG(WARNING) << "[PolicyIPCClient] HTTP server health check failed";
  connected_ = false;
  return false;
}

bool PolicyIPCClient::SendHttpPost(const std::string& endpoint,
                                   const std::string& request_json,
                                   std::string* response_json) {
  if (server_url_.empty()) {
    return false;
  }

  // Parse server URL (e.g., "http://localhost:8765")
  std::wstring wide_url = base::UTF8ToWide(server_url_ + endpoint);

  URL_COMPONENTS urlComp;
  ZeroMemory(&urlComp, sizeof(urlComp));
  urlComp.dwStructSize = sizeof(urlComp);

  // Set required component lengths to non-zero
  urlComp.dwSchemeLength    = (DWORD)-1;
  urlComp.dwHostNameLength  = (DWORD)-1;
  urlComp.dwUrlPathLength   = (DWORD)-1;
  urlComp.dwExtraInfoLength = (DWORD)-1;

  if (!WinHttpCrackUrl(wide_url.c_str(), (DWORD)wide_url.length(), 0, &urlComp)) {
    LOG(ERROR) << "[PolicyIPCClient] Failed to parse URL: " << GetLastError();
    return false;
  }

  // Extract components
  std::wstring hostname(urlComp.lpszHostName, urlComp.dwHostNameLength);
  std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
  INTERNET_PORT port = urlComp.nPort;

  // Initialize WinHTTP session
  HINTERNET hSession = WinHttpOpen(
      L"Chromium Policy Client/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0);

  if (!hSession) {
    LOG(ERROR) << "[PolicyIPCClient] WinHttpOpen failed: " << GetLastError();
    return false;
  }

  // Set timeout to 5 seconds
  DWORD timeout = 5000;
  WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
  WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

  // Connect to server
  HINTERNET hConnect = WinHttpConnect(
      hSession,
      hostname.c_str(),
      port,
      0);

  if (!hConnect) {
    LOG(ERROR) << "[PolicyIPCClient] WinHttpConnect failed: " << GetLastError();
    WinHttpCloseHandle(hSession);
    return false;
  }

  // Open HTTP request
  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect,
      L"POST",
      path.c_str(),
      NULL,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);

  if (!hRequest) {
    LOG(ERROR) << "[PolicyIPCClient] WinHttpOpenRequest failed: " << GetLastError();
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  // Set Content-Type header
  std::wstring headers = L"Content-Type: application/json\r\n";
  WinHttpAddRequestHeaders(
      hRequest,
      headers.c_str(),
      (DWORD)headers.length(),
      WINHTTP_ADDREQ_FLAG_ADD);

  // Send request with JSON payload
  BOOL bResults = WinHttpSendRequest(
      hRequest,
      WINHTTP_NO_ADDITIONAL_HEADERS,
      0,
      (LPVOID)request_json.c_str(),
      (DWORD)request_json.length(),
      (DWORD)request_json.length(),
      0);

  if (!bResults) {
    LOG(ERROR) << "[PolicyIPCClient] WinHttpSendRequest failed: " << GetLastError();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  // Receive response
  bResults = WinHttpReceiveResponse(hRequest, NULL);

  if (!bResults) {
    LOG(ERROR) << "[PolicyIPCClient] WinHttpReceiveResponse failed: " << GetLastError();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  // Check HTTP status code
  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  WinHttpQueryHeaders(
      hRequest,
      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX,
      &statusCode,
      &statusCodeSize,
      WINHTTP_NO_HEADER_INDEX);

  if (statusCode != 200) {
    LOG(ERROR) << "[PolicyIPCClient] HTTP error: " << statusCode;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  // Read response data
  std::string response_data;
  DWORD dwSize = 0;
  DWORD dwDownloaded = 0;
  LPSTR pszOutBuffer;

  do {
    // Check for available data
    dwSize = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
      LOG(ERROR) << "[PolicyIPCClient] WinHttpQueryDataAvailable failed: " << GetLastError();
      break;
    }

    // Allocate space for the buffer
    pszOutBuffer = new char[dwSize + 1];
    if (!pszOutBuffer) {
      LOG(ERROR) << "[PolicyIPCClient] Out of memory";
      dwSize = 0;
      break;
    } else {
      // Read the data
      ZeroMemory(pszOutBuffer, dwSize + 1);

      if (!WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
        LOG(ERROR) << "[PolicyIPCClient] WinHttpReadData failed: " << GetLastError();
        delete[] pszOutBuffer;
        break;
      } else {
        response_data.append(pszOutBuffer, dwDownloaded);
        delete[] pszOutBuffer;
      }
    }
  } while (dwSize > 0);

  // Close handles
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  if (response_data.empty()) {
    LOG(ERROR) << "[PolicyIPCClient] Empty HTTP response";
    return false;
  }

  *response_json = response_data;

  // Track successful contact
  last_successful_contact_ = base::TimeTicks::Now();
  consecutive_failures_ = 0;

  return true;
}

PolicyDecision PolicyIPCClient::CheckURLSync(const std::string& url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  PolicyDecision decision;
  decision.allow = true;  // Default allow

  // ========== FAIL-CLOSED SECURITY CHECK ==========
  if (IsInLockdownMode()) {
    // ========== AUTO-RECONNECT IN LOCKDOWN MODE ==========
    if (auto_reconnect_enabled_) {
      LOG(WARNING) << "[PolicyIPCClient] LOCKDOWN MODE: Attempting auto-reconnect...";
      if (TryReconnect()) {
        LOG(INFO) << "[PolicyIPCClient] ✓ Reconnected from LOCKDOWN! Continuing...";
        // Don't return, continue to execute the request below
      } else {
        LOG(ERROR) << "[PolicyIPCClient] ✗ Reconnect failed, staying in LOCKDOWN";
        decision.allow = false;
        decision.reason = "LOCKDOWN: Policy server unavailable (fail-closed security)";
        LOG(ERROR) << "[PolicyIPCClient] LOCKDOWN MODE: Blocking URL: " << url;
        return decision;
      }
    } else {
      decision.allow = false;
      decision.reason = "LOCKDOWN: Policy server unavailable (fail-closed security)";
      LOG(ERROR) << "[PolicyIPCClient] LOCKDOWN MODE: Blocking URL: " << url;
      return decision;
    }
  }

  if (!connected_) {
    // ========== AUTO-RECONNECT ==========
    if (auto_reconnect_enabled_) {
      LOG(WARNING) << "[PolicyIPCClient] Disconnected, attempting auto-reconnect...";
      if (TryReconnect()) {
        LOG(INFO) << "[PolicyIPCClient] ✓ Reconnected! Continuing with request...";
        // Don't return, continue to execute the request below
      } else {
        LOG(WARNING) << "[PolicyIPCClient] ✗ Reconnect failed";
        if (!fail_closed_mode_) {
          LOG(WARNING) << "[PolicyIPCClient] Server not connected but fail-open mode";
        }
        return decision;
      }
    } else {
      // Auto-reconnect disabled, return immediately
      if (!fail_closed_mode_) {
        LOG(WARNING) << "[PolicyIPCClient] Server not connected but fail-open mode";
      }
      return decision;
    }
  }

  // Build request JSON
  base::Value::Dict request;
  request.Set("url", url);
  if (!profile_name_.empty()) {
    request.Set("profile", profile_name_);
  }

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  // Send HTTP POST to /api/check_url
  std::string response_json;
  if (!SendHttpPost("/api/check_url", request_json, &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to check URL via HTTP: " << url;

    consecutive_failures_++;

    // ========== AUTO-RECONNECT ON FAILURE ==========
    if (auto_reconnect_enabled_ && consecutive_failures_ >= 2) {
      LOG(WARNING) << "[PolicyIPCClient] Multiple failures detected, attempting auto-reconnect...";
      if (TryReconnect()) {
        // Retry the request after successful reconnect
        if (SendHttpPost("/api/check_url", request_json, &response_json)) {
          LOG(INFO) << "[PolicyIPCClient] ✓ Request succeeded after reconnect";
          goto parse_response;  // Jump to response parsing
        }
      }
    }

    if (fail_closed_mode_ && consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
      decision.allow = false;
      decision.reason = "Server connection failed (fail-closed security)";
      LOG(ERROR) << "[PolicyIPCClient] BLOCKING due to connection failures: " << url;
      return decision;
    }

    return decision;
  }

parse_response:

  // Parse response
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
  decision.allow = true;

  if (IsInLockdownMode()) {
    // ========== AUTO-RECONNECT IN LOCKDOWN MODE ==========
    if (auto_reconnect_enabled_) {
      LOG(WARNING) << "[PolicyIPCClient] LOCKDOWN MODE: Attempting auto-reconnect...";
      if (TryReconnect()) {
        LOG(INFO) << "[PolicyIPCClient] ✓ Reconnected from LOCKDOWN! Continuing...";
        // Don't return, continue to execute the request below
      } else {
        LOG(ERROR) << "[PolicyIPCClient] ✗ Reconnect failed, staying in LOCKDOWN";
        decision.allow = false;
        decision.reason = "LOCKDOWN: Policy server unavailable (fail-closed security)";
        LOG(ERROR) << "[PolicyIPCClient] LOCKDOWN MODE: Blocking URL: " << url
                   << " | Profile: " << profile_name;
        return decision;
      }
    } else {
      decision.allow = false;
      decision.reason = "LOCKDOWN: Policy server unavailable (fail-closed security)";
      LOG(ERROR) << "[PolicyIPCClient] LOCKDOWN MODE: Blocking URL: " << url
                 << " | Profile: " << profile_name;
      return decision;
    }
  }

  if (!connected_) {
    // ========== AUTO-RECONNECT ==========
    if (auto_reconnect_enabled_) {
      LOG(WARNING) << "[PolicyIPCClient] Disconnected, attempting auto-reconnect...";
      if (TryReconnect()) {
        LOG(INFO) << "[PolicyIPCClient] ✓ Reconnected! Continuing with request...";
        // Don't return, continue to execute the request below
      } else {
        LOG(WARNING) << "[PolicyIPCClient] ✗ Reconnect failed";
        return decision;
      }
    } else {
      return decision;
    }
  }

  base::Value::Dict request;
  request.Set("url", url);
  request.Set("profile", profile_name);

  std::string request_json;
  base::JSONWriter::Write(request, &request_json);

  std::string response_json;
  if (!SendHttpPost("/api/check_url", request_json, &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to check URL via HTTP";

    consecutive_failures_++;

    // ========== AUTO-RECONNECT ON FAILURE ==========
    if (auto_reconnect_enabled_ && consecutive_failures_ >= 2) {
      LOG(WARNING) << "[PolicyIPCClient] Multiple failures detected, attempting auto-reconnect...";
      if (TryReconnect()) {
        // Retry the request after successful reconnect
        if (SendHttpPost("/api/check_url", request_json, &response_json)) {
          LOG(INFO) << "[PolicyIPCClient] ✓ Request succeeded after reconnect";
          goto parse_response_profile;  // Jump to response parsing
        }
      }
    }

    if (fail_closed_mode_ && consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
      decision.allow = false;
      decision.reason = "Server connection failed (fail-closed security)";
      return decision;
    }

    return decision;
  }

parse_response_profile:

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

base::Value::Dict PolicyIPCClient::GetPolicyDictSync(const std::string& role) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::Value::Dict result;

  if (!connected_) {
    return result;
  }

  // Build query string for GET request: /api/policy?profile=XXX&role=XXX
  std::string endpoint = "/api/policy?";
  if (!profile_name_.empty()) {
    endpoint += "profile=" + profile_name_ + "&";
  }
  endpoint += "role=" + role;

  // Send HTTP GET (using POST with empty body for simplicity)
  std::string response_json;
  if (!SendHttpPost(endpoint, "{}", &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] Failed to get policy for role: " << role;
    return result;
  }

  auto parsed = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (parsed && parsed->is_dict()) {
    return std::move(parsed->GetDict());
  }

  return result;
}

bool PolicyIPCClient::IsConnected() const {
  return connected_;
}

void PolicyIPCClient::SetProfileName(const std::string& profile_name) {
  profile_name_ = profile_name;
}

// ========== FAIL-CLOSED SECURITY ==========

void PolicyIPCClient::SetFailClosedMode(bool enabled) {
  fail_closed_mode_ = enabled;
}

bool PolicyIPCClient::IsFailClosedMode() const {
  return fail_closed_mode_;
}

base::TimeDelta PolicyIPCClient::GetTimeSinceLastContact() const {
  if (last_successful_contact_.is_null()) {
    return base::TimeDelta::Max();
  }
  return base::TimeTicks::Now() - last_successful_contact_;
}

bool PolicyIPCClient::IsServerAlive() {
  std::string response_json;
  if (SendHttpPost("/api/health", "{}", &response_json)) {
    return true;
  }
  return false;
}

void PolicyIPCClient::SetGracePeriod(base::TimeDelta grace_period) {
  grace_period_ = grace_period;
}

bool PolicyIPCClient::IsInLockdownMode() const {
  if (!fail_closed_mode_) {
    return false;
  }

  if (consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
    return true;
  }

  base::TimeDelta time_since_contact = GetTimeSinceLastContact();
  if (time_since_contact > grace_period_) {
    return true;
  }

  return false;
}

// ========== AUTO-RECONNECT IMPLEMENTATION ==========

bool PolicyIPCClient::TryReconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Check if we should throttle reconnect attempts
  base::TimeDelta time_since_last_attempt =
      base::TimeTicks::Now() - last_reconnect_attempt_;

  if (!last_reconnect_attempt_.is_null() &&
      time_since_last_attempt < reconnect_delay_) {
    LOG(WARNING) << "[PolicyIPCClient] Throttling reconnect attempt (last: "
                 << time_since_last_attempt.InSeconds() << "s ago)";
    return false;
  }

  last_reconnect_attempt_ = base::TimeTicks::Now();

  LOG(INFO) << "[PolicyIPCClient] → Attempting to reconnect to: " << server_url_;

  // Try to reconnect by sending health check
  std::string response_json;
  if (!SendHttpPost("/api/health", "{}", &response_json)) {
    LOG(WARNING) << "[PolicyIPCClient] ✗ Reconnect failed - server not responding";
    connected_ = false;
    return false;
  }

  // Parse health check response
  auto parsed = base::JSONReader::Read(response_json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (parsed && parsed->is_dict()) {
    const auto* status = parsed->GetDict().FindString("status");
    if (status && *status == "healthy") {
      connected_ = true;
      last_successful_contact_ = base::TimeTicks::Now();
      consecutive_failures_ = 0;
      LOG(INFO) << "[PolicyIPCClient] ✓ Reconnected successfully to: " << server_url_;
      return true;
    }
  }

  LOG(WARNING) << "[PolicyIPCClient] ✗ Reconnect failed - invalid health check response";
  connected_ = false;
  return false;
}

void PolicyIPCClient::SetAutoReconnect(bool enabled) {
  auto_reconnect_enabled_ = enabled;
  LOG(INFO) << "[PolicyIPCClient] Auto-reconnect "
            << (enabled ? "ENABLED" : "DISABLED");
}

// Stubs for unused policy watcher methods
void PolicyIPCClient::RegisterPolicyUpdateListener(PolicyUpdateCallback callback) {}
void PolicyIPCClient::StartPolicyWatcher(int poll_interval_ms) {}
void PolicyIPCClient::StopPolicyWatcher() {}
void PolicyIPCClient::WatchPolicyChanges() {}
void PolicyIPCClient::ScheduleNextPolicyCheck(int delay_ms) {}

}  // namespace policy_manager
