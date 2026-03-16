// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_CLIENT_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_CLIENT_H_

#include <queue>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"

namespace permission_sync {

class PermissionCacheManager;

// ============================================================================
// PermissionSyncClient (FRS Section 3.1 & 3.3)
// ============================================================================
// Manages the WebSocket connection to GravityBrowser server for real-time
// permission synchronization.
//
// Lifecycle:
//   1. Created when a browser profile is launched.
//   2. Connects to ws://127.0.0.1:<port> within 1 second.
//   3. Performs handshake (CONNECT → CONNECTED → PERMISSION_SYNC → SYNC_ACK).
//   4. Maintains heartbeat every 30 seconds.
//   5. Reconnects with exponential backoff on disconnection.
//   6. Destroyed when the profile/browser is shut down.
//
// Thread safety:
//   All methods run on the UI thread. Cache updates go through
//   PermissionCacheManager which is internally thread-safe.
//
class PermissionSyncClient
    : public network::mojom::WebSocketHandshakeClient,
      public network::mojom::WebSocketClient {
 public:
  // |cache_manager| must outlive this object.
  // |profile_id| identifies which profile's permissions to sync.
  // |ws_url| is the WebSocket endpoint (default: ws://127.0.0.1:9800).
  PermissionSyncClient(PermissionCacheManager* cache_manager,
                       const std::string& profile_id,
                       const std::string& ws_url = "ws://127.0.0.1:9800");
  ~PermissionSyncClient() override;

  PermissionSyncClient(const PermissionSyncClient&) = delete;
  PermissionSyncClient& operator=(const PermissionSyncClient&) = delete;

  // Initiates the WebSocket connection. Safe to call multiple times.
  void Connect(network::mojom::NetworkContext* network_context);

  // Gracefully disconnects and stops all timers.
  void Disconnect();

  // Returns whether the client is currently connected & synchronized.
  bool IsSynchronized() const;

  // network::mojom::WebSocketHandshakeClient:
  void OnOpeningHandshakeStarted(
      network::mojom::WebSocketHandshakeRequestPtr request) override;
  void OnFailure(const std::string& message,
                 int32_t net_error,
                 int32_t response_code) override;
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebSocket> socket,
      mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
      network::mojom::WebSocketHandshakeResponsePtr response,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override;

  // network::mojom::WebSocketClient:
  void OnDataFrame(bool fin,
                   network::mojom::WebSocketMessageType type,
                   uint64_t data_length) override;
  void OnDropChannel(bool was_clean,
                     uint16_t code,
                     const std::string& reason) override;
  void OnClosingHandshake() override;

 private:
  // --- Protocol Message Handlers (FRS Section 3.3) ---
  void OnMessageReceived(const std::string& message);
  void HandleConnectedMessage(const base::Value::Dict& payload);
  void HandlePermissionSyncMessage(const base::Value::Dict& payload);
  void HandlePermissionUpdateMessage(const base::Value::Dict& payload);
  void HandleHeartbeatAckMessage(const base::Value::Dict& payload);
  void HandleErrorMessage(const base::Value::Dict& payload);

  // --- Send Protocol Messages ---
  void SendConnectMessage();
  void SendSyncAck(int version);
  void SendUpdateAck(int version);
  void SendHeartbeat();
  void SendTextMessage(const std::string& json);

  // --- Data Pipe Handling ---
  void ReadFromDataPipe(MojoResult result,
                        const mojo::HandleSignalsState& state);
  void WriteToDataPipe(const std::string& data);

  // --- Heartbeat (FRS Section 3.1.2) ---
  void StartHeartbeatTimer();
  void StopHeartbeatTimer();
  void OnHeartbeatTimeout();

  // --- Reconnection (FRS Section 3.1.3) ---
  void ScheduleReconnect();
  void OnReconnectTimer();
  void ResetReconnectState();

  // --- Cleanup ---
  // Resets Mojo bindings, pipes, and buffers. Does NOT invalidate weak ptrs.
  void ResetConnection();

  // --- Connection Timeout (FRS 3.5.1) ---
  void OnConnectionTimeout();

  // --- STALE sync timeout ---
  void OnStaleSyncTimeout();

  // Configuration
  const raw_ptr<PermissionCacheManager> cache_manager_;
  std::string profile_id_;
  std::string ws_url_;

  // Mojo WebSocket bindings
  mojo::Receiver<network::mojom::WebSocketHandshakeClient>
      handshake_receiver_{this};
  mojo::Receiver<network::mojom::WebSocketClient> client_receiver_{this};
  mojo::Remote<network::mojom::WebSocket> websocket_;

  // Data pipes for reading/writing WebSocket frames
  mojo::ScopedDataPipeConsumerHandle readable_;
  mojo::ScopedDataPipeProducerHandle writable_;
  mojo::SimpleWatcher readable_watcher_;

  // Per-message dispatch:
  // OnDataFrame may fire for multiple WebSocket messages before
  // ReadFromDataPipe runs. Each complete message (fin=true) records its
  // total byte count in pending_message_sizes_. ReadFromDataPipe then
  // dispatches exactly that many bytes per message.
  std::string incoming_buffer_;
  std::queue<uint64_t> pending_message_sizes_;
  uint64_t pending_data_length_ = 0;  // Bytes in current multi-frame message

  // Fix #12: NetworkContext stored for reconnection.
  // Set to nullptr in Disconnect() to prevent dangling pointer usage.
  raw_ptr<network::mojom::NetworkContext> network_context_ = nullptr;

  // Heartbeat timer (FRS 3.1.2: every 30 seconds)
  base::RepeatingTimer heartbeat_timer_;
  // Timeout timer (FRS 3.1.2: 45 seconds no-message timeout)
  base::OneShotTimer timeout_timer_;

  // Reconnection state (FRS 3.1.3: exponential backoff)
  base::OneShotTimer reconnect_timer_;
  int reconnect_attempt_ = 0;
  // Backoff delays: 500ms → 1s → 2s → 5s (max)
  // Fix #1: No 0ms delay — always async via timer.
  static constexpr int kReconnectDelaysMs[] = {500, 1000, 2000, 5000};
  static constexpr size_t kMaxReconnectIndex = 3;

  // Fix #11: Connection timeout (FRS 3.5.1: 5 seconds).
  base::OneShotTimer connection_timeout_timer_;
  static constexpr base::TimeDelta kConnectionTimeout = base::Seconds(5);

  // Fix #6: STALE sync timeout.
  base::OneShotTimer stale_sync_timer_;
  static constexpr base::TimeDelta kStaleSyncTimeout = base::Seconds(10);

  // Timings (FRS constants)
  static constexpr base::TimeDelta kHeartbeatInterval = base::Seconds(30);
  static constexpr base::TimeDelta kTimeoutDuration = base::Seconds(45);

  base::WeakPtrFactory<PermissionSyncClient> weak_factory_{this};
};

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_CLIENT_H_
