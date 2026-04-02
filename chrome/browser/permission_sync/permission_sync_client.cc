// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permission_sync/permission_sync_client.h"

#include <utility>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "chrome/browser/permission_sync/permission_cache_manager.h"
#include "content/public/browser/network_service_instance.h"
#include "net/base/isolation_info.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/storage_access_api/status.h"
#include "net/cookies/site_for_cookies.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom.h"
#include "content/public/browser/content_browser_client.h"
#include "services/network/public/mojom/client_security_state.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace permission_sync {

namespace {

// Build a JSON envelope message per FRS Section 3.2.3.
std::string BuildJsonMessage(const std::string& type,
                              base::Value::Dict payload) {
  base::Value::Dict envelope;
  envelope.Set("type", type);
  envelope.Set("payload", std::move(payload));
  envelope.Set("timestamp", base::TimeFormatAsIso8601(base::Time::Now()));
  envelope.Set("message_id",
               base::UnguessableToken::Create().ToString());

  std::string json;
  base::JSONWriter::Write(base::Value(std::move(envelope)), &json);
  return json;
}

// Traffic annotation for the permission sync WebSocket connection.
constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("permission_sync_websocket", R"(
    semantics {
      sender: "Permission Sync Client"
      description:
        "WebSocket connection to the local GravityBrowser application "
        "for real-time permission synchronization. Runs on localhost only."
      trigger: "Browser profile startup."
      data: "Permission rules (blocklist/whitelist) in JSON format."
      destination: LOCAL
    }
    policy {
      cookies_allowed: NO
      setting: "This feature is always active for managed profiles."
    })");

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

PermissionSyncClient::PermissionSyncClient(
    PermissionCacheManager* cache_manager,
    const std::string& profile_id,
    const std::string& ws_url)
    : cache_manager_(cache_manager),
      profile_id_(profile_id),
      ws_url_(ws_url),
      connection_secret_(base::UnguessableToken::Create().ToString()),
      readable_watcher_(FROM_HERE,
                        mojo::SimpleWatcher::ArmingPolicy::MANUAL) {
  LOG(INFO) << "[PermissionSyncClient] Created for profile: " << profile_id_
            << " (connection_secret generated)";
}

PermissionSyncClient::~PermissionSyncClient() {
  Disconnect();
}

// ============================================================================
// Public API
// ============================================================================

void PermissionSyncClient::SetNetworkContextResolver(
    NetworkContextResolver resolver) {
  nc_resolver_ = std::move(resolver);
}

void PermissionSyncClient::SetStandaloneNetworkContext(
    mojo::Remote<network::mojom::NetworkContext> standalone_nc) {
  standalone_nc_ = std::move(standalone_nc);

  // Detect Network Service restart → standalone NC pipe death.
  standalone_nc_.set_disconnect_handler(
      base::BindOnce(
          &PermissionSyncClient::OnStandaloneNetworkContextDisconnected,
          weak_factory_.GetWeakPtr()));

  // Auto-set resolver to return the standalone NC.
  nc_resolver_ = base::BindRepeating(
      [](PermissionSyncClient* self) -> network::mojom::NetworkContext* {
        if (self->standalone_nc_.is_bound() &&
            self->standalone_nc_.is_connected()) {
          return self->standalone_nc_.get();
        }
        return nullptr;
      },
      base::Unretained(this));
}

void PermissionSyncClient::Connect() {
  if (!nc_resolver_) {
    LOG(ERROR) << "[PermissionSyncClient] No NetworkContextResolver set";
    return;
  }

  network::mojom::NetworkContext* nc = nc_resolver_.Run();
  if (!nc) {
    LOG(ERROR) << "[PermissionSyncClient] Resolver returned null NC. "
               << "Profile may be shutting down.";
    return;
  }

  LOG(INFO) << "[PermissionSyncClient] Resolved fresh NetworkContext "
            << "for connection attempt";
  Connect(nc);
}

void PermissionSyncClient::Connect(
    network::mojom::NetworkContext* network_context) {
  if (!network_context) {
    LOG(ERROR) << "[PermissionSyncClient] NetworkContext is null";
    return;
  }

  network_context_ = network_context;
  ResetConnection();

  LOG(INFO) << "[PermissionSyncClient] Connecting to " << ws_url_
            << " (background sync)";

  // If already SYNCHRONIZED from startup rules, don't regress to
  // CONNECTING. WebSocket is supplementary — for runtime updates.
  ConnectionState current = cache_manager_->GetConnectionState();
  if (current != ConnectionState::SYNCHRONIZED) {
    cache_manager_->SetConnectionState(ConnectionState::CONNECTING);
  }

  // Fix #11: Start connection timeout timer (FRS 3.5.1: 5 seconds).
  connection_timeout_timer_.Start(
      FROM_HERE, kConnectionTimeout,
      base::BindOnce(&PermissionSyncClient::OnConnectionTimeout,
                     weak_factory_.GetWeakPtr()));

  // FRS 3.1.1: Chromium initiates WebSocket connection.
  GURL ws_gurl(ws_url_);

  std::vector<std::string> requested_protocols;
  net::SiteForCookies site_for_cookies;
  auto storage_access = net::StorageAccessApiStatus::kNone;

  url::Origin origin = url::Origin::Create(ws_gurl);
  net::IsolationInfo isolation_info =
      net::IsolationInfo::CreateForInternalRequest(origin);

  std::vector<network::mojom::HttpHeaderPtr> additional_headers;
  auto client_security_state = network::mojom::ClientSecurityState::New();

  network_context_->CreateWebSocket(
      ws_gurl,
      requested_protocols,
      site_for_cookies,
      storage_access,
      isolation_info,
      std::move(additional_headers),
      /*process_id=*/0,  // 0 = browser process
      origin,
      std::move(client_security_state),
      /*options=*/0,
      net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation),
      handshake_receiver_.BindNewPipeAndPassRemote(),
      /*url_loader_network_observer=*/mojo::NullRemote(),
      /*auth_handler=*/mojo::NullRemote(),
      /*header_client=*/mojo::NullRemote(),
      /*throttling_profile_id=*/std::nullopt);
}

void PermissionSyncClient::Disconnect() {
  LOG(INFO) << "[PermissionSyncClient] Disconnecting";

  StopHeartbeatTimer();
  reconnect_timer_.Stop();
  timeout_timer_.Stop();
  connection_timeout_timer_.Stop();
  stale_sync_timer_.Stop();

  if (websocket_.is_bound()) {
    websocket_->StartClosingHandshake(1000u, "Client disconnect");
  }

  ResetConnection();

  // Fix #12: Null out network_context_ to prevent dangling pointer
  // usage from any pending timer callbacks.
  network_context_ = nullptr;

  // Fix #1: Invalidate weak ptrs only in intentional shutdown,
  // NOT in ResetConnection (which is called during reconnect flow).
  weak_factory_.InvalidateWeakPtrs();

  cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
}

bool PermissionSyncClient::IsSynchronized() const {
  return cache_manager_->GetConnectionState() == ConnectionState::SYNCHRONIZED;
}

// ============================================================================
// WebSocketHandshakeClient Implementation (FRS Section 3.1.1)
// ============================================================================

void PermissionSyncClient::OnOpeningHandshakeStarted(
    network::mojom::WebSocketHandshakeRequestPtr request) {
  LOG(INFO) << "[PermissionSyncClient] Handshake started with "
            << request->url;
}

void PermissionSyncClient::OnFailure(const std::string& message,
                                      int32_t net_error,
                                      int32_t response_code) {
  LOG(ERROR) << "[PermissionSyncClient] Connection failed: " << message
             << " (net_error=" << net_error
             << ", response_code=" << response_code << ")";

  connection_timeout_timer_.Stop();
  ResetConnection();

  // If we have startup rules, stay SYNCHRONIZED (not DISCONNECTED).
  // Browser continues working. WebSocket failure is non-critical.
  if (!cache_manager_->HasStartupRules()) {
    cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
  }

  // Always attempt reconnect for runtime updates.
  ScheduleReconnect();
}

void PermissionSyncClient::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::WebSocket> socket,
    mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
    network::mojom::WebSocketHandshakeResponsePtr response,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  LOG(INFO) << "[PermissionSyncClient] WebSocket connection established";

  // Cancel connection timeout — we connected successfully.
  connection_timeout_timer_.Stop();

  // Bind the WebSocket remote and client receiver.
  websocket_.Bind(std::move(socket));
  client_receiver_.Bind(std::move(client_receiver));

  // Detect silent Mojo pipe death (Network Service closes WebSocket
  // without delivering OnDropChannel to the client). Without these
  // handlers, the client would wait 45s for heartbeat timeout.
  websocket_.set_disconnect_handler(
      base::BindOnce(&PermissionSyncClient::OnMojoPipeDisconnected,
                     weak_factory_.GetWeakPtr(), "websocket_"));
  client_receiver_.set_disconnect_handler(
      base::BindOnce(&PermissionSyncClient::OnMojoPipeDisconnected,
                     weak_factory_.GetWeakPtr(), "client_receiver_"));

  readable_ = std::move(readable);
  writable_ = std::move(writable);

  readable_watcher_.Watch(
      readable_.get(),
      MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
      base::BindRepeating(&PermissionSyncClient::ReadFromDataPipe,
                          weak_factory_.GetWeakPtr()));

  websocket_->StartReceiving();

  ResetReconnectState();

  // Fix #4: Do NOT set state to CONNECTING again — it was already set
  // in Connect(). State transitions CONNECTING → CONNECTING would be
  // rejected by IsValidTransition(). Just send the CONNECT message.
  SendConnectMessage();
}

// ============================================================================
// WebSocketClient Implementation
// ============================================================================

void PermissionSyncClient::OnDataFrame(
    bool fin,
    network::mojom::WebSocketMessageType type,
    uint64_t data_length) {
  // Reset timeout timer — we received data (FRS 3.1.2).
  timeout_timer_.Stop();
  timeout_timer_.Start(FROM_HERE, kTimeoutDuration,
                       base::BindOnce(&PermissionSyncClient::OnHeartbeatTimeout,
                                      weak_factory_.GetWeakPtr()));

  // Track this frame's bytes for proper message dispatch.
  pending_data_length_ += data_length;

  if (fin) {
    // This is the final frame of a WebSocket message.
    // Record the total message size for dispatch.
    pending_message_sizes_.push(pending_data_length_);
    pending_data_length_ = 0;
  }

  readable_watcher_.ArmOrNotify();
}

void PermissionSyncClient::OnDropChannel(bool was_clean,
                                          uint16_t code,
                                          const std::string& reason) {
  LOG(WARNING) << "[PermissionSyncClient] Channel dropped: "
               << (was_clean ? "clean" : "unclean")
               << ", code=" << code << ", reason=" << reason;

  ResetConnection();

  // 1001 = NC reset (Going Away). Expected during profile init.
  // If we have startup rules, browser is unaffected.
  if (cache_manager_->HasStartupRules()) {
    if (code == 1001) {
      LOG(INFO) << "[PermissionSyncClient] 1001 received but startup "
                << "rules active. Browser unaffected. Will retry.";
    }
    // Don't change state — stay SYNCHRONIZED.
  } else {
    cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
  }

  ScheduleReconnect();
}

void PermissionSyncClient::OnClosingHandshake() {
  LOG(INFO) << "[PermissionSyncClient] Server initiated closing handshake";
}

// ============================================================================
// Protocol Message Handlers (FRS Section 3.3)
// ============================================================================

void PermissionSyncClient::OnMessageReceived(const std::string& message) {
  auto parsed = base::JSONReader::Read(message, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[PermissionSyncClient] Invalid JSON message received";
    return;
  }

  const base::Value::Dict& envelope = parsed->GetDict();
  const std::string* type = envelope.FindString("type");
  if (!type) {
    LOG(ERROR) << "[PermissionSyncClient] Message missing 'type' field";
    return;
  }

  const base::Value::Dict* payload = envelope.FindDict("payload");
  base::Value::Dict empty_dict;

  // Fix #10: Use DVLOG for high-frequency messages (HEARTBEAT_ACK).
  if (*type == "HEARTBEAT_ACK") {
    DVLOG(1) << "[PermissionSyncClient] Received message type: " << *type;
  } else {
    LOG(INFO) << "[PermissionSyncClient] Received message type: " << *type;
  }

  // Route to appropriate handler.
  if (*type == "CONNECTED") {
    HandleConnectedMessage(payload ? *payload : empty_dict);
  } else if (*type == "PERMISSION_SYNC") {
    HandlePermissionSyncMessage(payload ? *payload : empty_dict);
  } else if (*type == "PERMISSION_UPDATE") {
    HandlePermissionUpdateMessage(payload ? *payload : empty_dict);
  } else if (*type == "HEARTBEAT_ACK") {
    HandleHeartbeatAckMessage(payload ? *payload : empty_dict);
  } else if (*type == "ERROR") {
    HandleErrorMessage(payload ? *payload : empty_dict);
  } else {
    LOG(WARNING) << "[PermissionSyncClient] Unknown message type: " << *type;
  }
}

void PermissionSyncClient::HandleConnectedMessage(
    const base::Value::Dict& payload) {
  LOG(INFO) << "[PermissionSyncClient] Handshake accepted by server";

  // If already SYNCHRONIZED from startup, transition SYNCHRONIZED → SYNCING
  // for protocol correctness: server will send PERMISSION_SYNC next.
  ConnectionState current = cache_manager_->GetConnectionState();
  if (current == ConnectionState::SYNCHRONIZED) {
    // Bypass normal validation: SYNCHRONIZED → SYNCING is valid.
    cache_manager_->SetConnectionState(ConnectionState::SYNCING);
  } else {
    cache_manager_->SetConnectionState(ConnectionState::SYNCING);
  }

  // Start stale sync timer: if PERMISSION_SYNC doesn't arrive within
  // kStaleSyncTimeout, the WebSocket likely died silently → reconnect.
  stale_sync_timer_.Start(
      FROM_HERE, kStaleSyncTimeout,
      base::BindOnce(&PermissionSyncClient::OnStaleSyncTimeout,
                     weak_factory_.GetWeakPtr()));
}

void PermissionSyncClient::HandlePermissionSyncMessage(
    const base::Value::Dict& payload) {
  // Fix #3: Pass Dict& directly — no serialize/deserialize roundtrip.
  if (!cache_manager_->LoadFullPermissionSet(payload)) {
    LOG(ERROR) << "[PermissionSyncClient] Failed to load permission set";
    return;
  }

  // Cancel STALE sync timer if active (we got the sync we were waiting for).
  stale_sync_timer_.Stop();

  int version = cache_manager_->GetPermissionVersion();
  LOG(INFO) << "[PermissionSyncClient] Full sync received. Version: "
            << version << ", Rules: " << cache_manager_->GetRuleCount();

  // Ensure state is SYNCHRONIZED (might already be from startup).
  ConnectionState current = cache_manager_->GetConnectionState();
  if (current != ConnectionState::SYNCHRONIZED) {
    cache_manager_->SetConnectionState(ConnectionState::SYNCHRONIZED);
  }

  SendSyncAck(version);
  StartHeartbeatTimer();
}

void PermissionSyncClient::HandlePermissionUpdateMessage(
    const base::Value::Dict& payload) {
  // Fix #3: Pass Dict& directly — no serialize/deserialize roundtrip.
  if (!cache_manager_->ApplyDeltaUpdate(payload)) {
    LOG(WARNING) << "[PermissionSyncClient] Delta update failed "
                 << "(version gap). Requesting full sync.";
    cache_manager_->SetConnectionState(ConnectionState::SYNCING);
    // Send explicit request for full sync (fix #6).
    SendTextMessage(BuildJsonMessage("REQUEST_SYNC", base::Value::Dict()));
    return;
  }

  int version = cache_manager_->GetPermissionVersion();
  LOG(INFO) << "[PermissionSyncClient] Delta update applied. Version: "
            << version;
  SendUpdateAck(version);
}

void PermissionSyncClient::HandleHeartbeatAckMessage(
    const base::Value::Dict& payload) {
  const std::string* status = payload.FindString("status");
  if (!status) return;

  if (*status == "STALE") {
    LOG(WARNING) << "[PermissionSyncClient] Heartbeat: version STALE";
    cache_manager_->SetConnectionState(ConnectionState::SYNCING);

    // Fix #6: Send explicit sync request + start timeout.
    // If server doesn't send PERMISSION_SYNC within 10 seconds,
    // we reconnect to force a clean sync.
    SendTextMessage(BuildJsonMessage("REQUEST_SYNC", base::Value::Dict()));
    stale_sync_timer_.Start(
        FROM_HERE, kStaleSyncTimeout,
        base::BindOnce(&PermissionSyncClient::OnStaleSyncTimeout,
                       weak_factory_.GetWeakPtr()));
  } else {
    DVLOG(1) << "[PermissionSyncClient] Heartbeat: OK";
  }
}

void PermissionSyncClient::HandleErrorMessage(
    const base::Value::Dict& payload) {
  const std::string* code = payload.FindString("code");
  const std::string* message = payload.FindString("message");
  LOG(ERROR) << "[PermissionSyncClient] Server error: "
             << (code ? *code : "unknown")
             << " — " << (message ? *message : "no message");

  if (code && *code == "ERR_SESSION_REPLAY") {
    // Token replay detected — terminate browser immediately.
    // Attacker copied a valid token but doesn't have the in-memory
    // connection_secret from the original browser.
    LOG(ERROR) << "[PermissionSyncClient] SESSION REPLAY → terminating.";
    base::Process::TerminateCurrentProcessImmediately(1);
  }

  if (code && (*code == "ERR_INVALID_PROFILE" ||
               *code == "ERR_PROTOCOL_MISMATCH")) {
    LOG(ERROR) << "[PermissionSyncClient] Fatal error. Not retrying.";
    Disconnect();
  }
}

// ============================================================================
// Send Protocol Messages
// ============================================================================

void PermissionSyncClient::SendConnectMessage() {
  base::Value::Dict payload;
  payload.Set("profile_id", profile_id_);
  payload.Set("protocol_version", 1);

  // Include session_id for anti-replay validation.
  // Extracted from --gra-token and stored as --gra-session-id during startup.
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch("gra-session-id")) {
    std::string session_id =
        command_line->GetSwitchValueASCII("gra-session-id");
    if (!session_id.empty()) {
      payload.Set("session_id", session_id);
    }
  }

  // Connection secret: random value from THIS browser instance's memory.
  // Server binds on first CONNECT, verifies on reconnect.
  // Attacker has the token but NOT this secret.
  payload.Set("connection_secret", connection_secret_);

  SendTextMessage(BuildJsonMessage("CONNECT", std::move(payload)));
  LOG(INFO) << "[PermissionSyncClient] Sent CONNECT for profile: "
            << profile_id_;
}

void PermissionSyncClient::SendSyncAck(int version) {
  base::Value::Dict payload;
  payload.Set("version", version);
  payload.Set("status", "OK");
  SendTextMessage(BuildJsonMessage("SYNC_ACK", std::move(payload)));
}

void PermissionSyncClient::SendUpdateAck(int version) {
  base::Value::Dict payload;
  payload.Set("version", version);
  payload.Set("status", "OK");
  SendTextMessage(BuildJsonMessage("UPDATE_ACK", std::move(payload)));
}

void PermissionSyncClient::SendHeartbeat() {
  base::Value::Dict payload;
  payload.Set("permission_version", cache_manager_->GetPermissionVersion());
  SendTextMessage(BuildJsonMessage("HEARTBEAT", std::move(payload)));
  DVLOG(1) << "[PermissionSyncClient] Heartbeat sent. Version: "
           << cache_manager_->GetPermissionVersion();
}

void PermissionSyncClient::SendTextMessage(const std::string& json) {
  if (!websocket_.is_bound() || !writable_.is_valid()) {
    LOG(WARNING) << "[PermissionSyncClient] Cannot send: not connected";
    return;
  }

  // Fix #8: Correct order per Mojo WebSocket protocol.
  // SendMessage tells the network service how many bytes to expect,
  // then we write the data into the pipe.
  websocket_->SendMessage(network::mojom::WebSocketMessageType::TEXT,
                          json.size());

  // Note (#7): WriteAllData is a blocking call. Acceptable here because
  // all client-to-server messages are small (< 1KB). For large payloads
  // in the future, consider BeginWriteData + watcher pattern.
  WriteToDataPipe(json);
}

// ============================================================================
// Data Pipe I/O
// ============================================================================

void PermissionSyncClient::ReadFromDataPipe(
    MojoResult result,
    const mojo::HandleSignalsState& state) {
  if (result != MOJO_RESULT_OK) {
    if (result == MOJO_RESULT_FAILED_PRECONDITION) {
      // Data pipe peer closed — WebSocket died at transport level.
      // Trigger immediate reconnection instead of waiting for stale sync timer.
      LOG(WARNING) << "[PermissionSyncClient] Data pipe PEER_CLOSED detected. "
                   << "WebSocket died silently. Triggering immediate reconnect.";
      StopHeartbeatTimer();
      connection_timeout_timer_.Stop();
      stale_sync_timer_.Stop();
      ResetConnection();
      if (!cache_manager_->HasStartupRules()) {
        cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
      }
      ScheduleReconnect();
    } else {
      LOG(ERROR) << "[PermissionSyncClient] Read pipe error: " << result;
    }
    return;
  }

  // Read all available data from the pipe into incoming_buffer_.
  while (true) {
    base::span<const uint8_t> buffer;
    MojoResult read_result = readable_->BeginReadData(
        MOJO_BEGIN_READ_DATA_FLAG_NONE, buffer);

    if (read_result == MOJO_RESULT_SHOULD_WAIT) {
      readable_watcher_.ArmOrNotify();
      break;
    }

    if (read_result != MOJO_RESULT_OK) {
      break;
    }

    incoming_buffer_.append(
        reinterpret_cast<const char*>(buffer.data()), buffer.size());
    readable_->EndReadData(buffer.size());
  }

  // Dispatch complete messages using the queue.
  // Each entry in pending_message_sizes_ is the total byte count
  // for one complete WebSocket message.
  while (!pending_message_sizes_.empty()) {
    uint64_t msg_size = pending_message_sizes_.front();

    if (incoming_buffer_.size() < msg_size) {
      // Not enough data yet for this message. Wait for more.
      break;
    }

    // Extract exactly msg_size bytes as one complete message.
    std::string message = incoming_buffer_.substr(0, msg_size);
    incoming_buffer_.erase(0, msg_size);
    pending_message_sizes_.pop();

    OnMessageReceived(message);
  }
}

void PermissionSyncClient::WriteToDataPipe(const std::string& data) {
  if (!writable_.is_valid()) return;

  auto data_span = base::as_byte_span(data);
  MojoResult result = writable_->WriteAllData(data_span);
  if (result != MOJO_RESULT_OK) {
    LOG(ERROR) << "[PermissionSyncClient] Write pipe error: " << result;
  }
}

// ============================================================================
// Heartbeat (FRS Section 3.1.2)
// ============================================================================

void PermissionSyncClient::StartHeartbeatTimer() {
  heartbeat_timer_.Start(
      FROM_HERE, kHeartbeatInterval,
      base::BindRepeating(&PermissionSyncClient::SendHeartbeat,
                          weak_factory_.GetWeakPtr()));

  timeout_timer_.Start(FROM_HERE, kTimeoutDuration,
                       base::BindOnce(&PermissionSyncClient::OnHeartbeatTimeout,
                                      weak_factory_.GetWeakPtr()));

  LOG(INFO) << "[PermissionSyncClient] Heartbeat started ("
            << kHeartbeatInterval.InSeconds() << "s interval, "
            << kTimeoutDuration.InSeconds() << "s timeout)";
}

void PermissionSyncClient::StopHeartbeatTimer() {
  heartbeat_timer_.Stop();
  timeout_timer_.Stop();
}

void PermissionSyncClient::OnHeartbeatTimeout() {
  LOG(WARNING) << "[PermissionSyncClient] Heartbeat timeout ("
               << kTimeoutDuration.InSeconds() << "s). Reconnecting...";

  ResetConnection();
  cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
  ScheduleReconnect();
}

// ============================================================================
// Reconnection (FRS Section 3.1.3)
// ============================================================================

void PermissionSyncClient::ScheduleReconnect() {
  // Use resolver if available; fall back to stored pointer.
  if (!nc_resolver_ && !network_context_) {
    LOG(ERROR) << "[PermissionSyncClient] Cannot reconnect: "
               << "no resolver and no NetworkContext";
    return;
  }

  // Fix #1: Always use timer for reconnection (no synchronous dispatch).
  // Eliminates WeakPtr crash when ResetConnection invalidates ptrs
  // in same call stack.
  size_t index = std::min(static_cast<size_t>(reconnect_attempt_),
                          kMaxReconnectIndex);
  int delay_ms = kReconnectDelaysMs[index];

  LOG(INFO) << "[PermissionSyncClient] Reconnect attempt "
            << (reconnect_attempt_ + 1) << " in " << delay_ms << "ms";

  reconnect_attempt_++;

  reconnect_timer_.Start(
      FROM_HERE, base::Milliseconds(delay_ms),
      base::BindOnce(&PermissionSyncClient::OnReconnectTimer,
                      weak_factory_.GetWeakPtr()));
}

void PermissionSyncClient::OnReconnectTimer() {
  LOG(INFO) << "[PermissionSyncClient] Attempting reconnection...";

  // Check if standalone NC is still alive. If not, recreate it.
  if (standalone_nc_.is_bound() && !standalone_nc_.is_connected()) {
    LOG(WARNING) << "[PermissionSyncClient] Standalone NC pipe disconnected. "
                 << "Recreating before reconnect.";
    RecreateStandaloneNetworkContext();
  } else if (!standalone_nc_.is_bound()) {
    LOG(WARNING) << "[PermissionSyncClient] Standalone NC not bound. "
                 << "Creating fresh NC for reconnect.";
    RecreateStandaloneNetworkContext();
  }

  // Use resolver-based Connect() to get a FRESH NetworkContext.
  if (nc_resolver_) {
    Connect();  // Calls resolver internally
  } else if (network_context_) {
    Connect(network_context_);  // Fallback to stored pointer
  } else {
    LOG(ERROR) << "[PermissionSyncClient] No NC available for reconnect.";
  }
}

void PermissionSyncClient::ResetReconnectState() {
  reconnect_attempt_ = 0;
  reconnect_timer_.Stop();
}

// ============================================================================
// Timeout Handlers
// ============================================================================

void PermissionSyncClient::OnConnectionTimeout() {
  // Fix #11: FRS 3.5.1 — connection must establish within 5 seconds.
  LOG(WARNING) << "[PermissionSyncClient] Connection timeout ("
               << kConnectionTimeout.InSeconds() << "s). Treating as failure.";

  ResetConnection();
  cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
  ScheduleReconnect();
}

void PermissionSyncClient::OnStaleSyncTimeout() {
  // Fix #6: If STALE → SYNCING but no PERMISSION_SYNC arrives in 10s,
  // reconnect to force a clean re-sync.
  LOG(WARNING) << "[PermissionSyncClient] STALE sync timeout ("
               << kStaleSyncTimeout.InSeconds()
               << "s). Reconnecting for clean sync.";

  ResetConnection();
  cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
  ScheduleReconnect();
}

// ============================================================================
// Mojo Pipe Disconnect Detection
// ============================================================================

void PermissionSyncClient::OnMojoPipeDisconnected(
    const std::string& pipe_name) {
  LOG(WARNING) << "[PermissionSyncClient] Mojo pipe '" << pipe_name
               << "' disconnected silently (Network Service closed WebSocket "
               << "without OnDropChannel). Triggering immediate reconnect.";

  // Treat this like OnDropChannel(false, 1001, "pipe_death").
  StopHeartbeatTimer();
  connection_timeout_timer_.Stop();
  stale_sync_timer_.Stop();
  ResetConnection();

  if (!cache_manager_->HasStartupRules()) {
    cache_manager_->SetConnectionState(ConnectionState::DISCONNECTED);
  }

  ScheduleReconnect();
}

// ============================================================================
// Standalone NetworkContext Lifecycle
// ============================================================================

void PermissionSyncClient::OnStandaloneNetworkContextDisconnected() {
  LOG(WARNING) << "[PermissionSyncClient] Standalone NC Mojo pipe disconnected "
               << "(Network Service restarted). Will recreate on next "
               << "reconnect attempt.";
  // Don't recreate immediately — the reconnect timer will handle it.
  // Just reset the dead remote so is_bound() returns false.
  standalone_nc_.reset();
}

void PermissionSyncClient::RecreateStandaloneNetworkContext() {
  LOG(INFO) << "[PermissionSyncClient] Creating fresh standalone "
            << "NetworkContext for reconnect.";

  // Reset old NC if still bound (but disconnected).
  standalone_nc_.reset();

  // Create new standalone NC (same logic as InitializeSyncClient in factory).
  auto params = network::mojom::NetworkContextParams::New();
  params->cert_verifier_params = content::GetCertVerifierParams(
      cert_verifier::mojom::CertVerifierCreationParams::New());
  // DIRECT proxy: bypass all proxy for localhost WebSocket.
  params->initial_proxy_config =
      net::ProxyConfigWithAnnotation::CreateDirect();
  content::CreateNetworkContextInNetworkService(
      standalone_nc_.BindNewPipeAndPassReceiver(), std::move(params));

  // Re-register disconnect handler.
  standalone_nc_.set_disconnect_handler(
      base::BindOnce(
          &PermissionSyncClient::OnStandaloneNetworkContextDisconnected,
          weak_factory_.GetWeakPtr()));

  // Update resolver to point to the new NC.
  nc_resolver_ = base::BindRepeating(
      [](PermissionSyncClient* self) -> network::mojom::NetworkContext* {
        if (self->standalone_nc_.is_bound() &&
            self->standalone_nc_.is_connected()) {
          return self->standalone_nc_.get();
        }
        return nullptr;
      },
      base::Unretained(this));

  LOG(INFO) << "[PermissionSyncClient] Fresh standalone NC created. Bound="
            << standalone_nc_.is_bound()
            << " Connected=" << standalone_nc_.is_connected();
}

// ============================================================================
// Internal Helpers
// ============================================================================

void PermissionSyncClient::ResetConnection() {
  // Fix #1: Do NOT call InvalidateWeakPtrs() here.
  // ResetConnection is called during reconnect flow (OnFailure,
  // OnDropChannel, OnHeartbeatTimeout → ScheduleReconnect).
  // Invalidating weak ptrs would kill the reconnect timer's callback
  // and any other pending callbacks. Only Disconnect() and the
  // destructor should invalidate weak ptrs.

  StopHeartbeatTimer();
  connection_timeout_timer_.Stop();
  stale_sync_timer_.Stop();
  readable_watcher_.Cancel();
  incoming_buffer_.clear();
  // Clear the per-message dispatch queue.
  while (!pending_message_sizes_.empty()) {
    pending_message_sizes_.pop();
  }
  pending_data_length_ = 0;

  websocket_.reset();
  readable_.reset();
  writable_.reset();

  if (handshake_receiver_.is_bound())
    handshake_receiver_.reset();
  if (client_receiver_.is_bound())
    client_receiver_.reset();
}

}  // namespace permission_sync
