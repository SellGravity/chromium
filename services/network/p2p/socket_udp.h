// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_P2P_SOCKET_UDP_H_
#define SERVICES_NETWORK_P2P_SOCKET_UDP_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <set>
#include <vector>

#include "base/component_export.h"
#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "net/base/backoff_entry.h"
#include "net/base/ip_endpoint.h"
#include "net/socket/diff_serv_code_point.h"
#include "net/socket/udp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/p2p/socket.h"
#include "services/network/public/cpp/p2p_socket_type.h"
#include "services/network/throttling/scoped_throttling_token.h"
#include "third_party/webrtc/rtc_base/async_packet_socket.h"

namespace net {
class NetLog;
}  // namespace net

namespace network {

class P2PMessageThrottler;
class ThrottlingP2PNetworkInterceptor;

struct P2PPendingPacket {
  P2PPendingPacket(const net::IPEndPoint& to,
                   base::span<const uint8_t> content,
                   const webrtc::AsyncSocketPacketOptions& options,
                   uint64_t id);
  P2PPendingPacket(const P2PPendingPacket& other);
  ~P2PPendingPacket();
  net::IPEndPoint to;
  scoped_refptr<net::IOBuffer> data;
  size_t size;
  webrtc::AsyncSocketPacketOptions packet_options;
  uint64_t id;
};

class COMPONENT_EXPORT(NETWORK_SERVICE) P2PSocketUdp : public P2PSocket {
 public:
  friend class ThrottlingP2PNetworkInterceptor;

  // Limit the maximum number of batching received packets.
  static constexpr size_t kUdpMaxBatchingRecvPackets = 64;
  // Limit the maximum buffering time of batching received packets.
  static constexpr base::TimeDelta kUdpMaxBatchingRecvBuffering =
      base::Milliseconds(1);

  using DatagramServerSocketFactory =
      base::RepeatingCallback<std::unique_ptr<net::DatagramServerSocket>(
          net::NetLog* net_log)>;
  // |is_socks5_tunnel|: true when |socket_factory| is guaranteed to produce a
  // Socks5UdpTunnel (see services/network/p2p/socks5_udp_tunnel.h). Its
  // Listen() completes asynchronously, so DoListen() needs to know to call
  // SetListenDoneCallback() and wait, rather than treating the
  // net::ERR_IO_PENDING it returns as an immediate bind failure. This flag is
  // also what makes the static_cast<Socks5UdpTunnel*> in DoListen() safe
  // without RTTI (Chromium builds with -fno-rtti, so dynamic_cast isn't
  // available) — the caller (P2PSocketManager::CreateSocket) is the only one
  // who knows |socket_factory|'s concrete return type, so it must vouch for
  // it via this flag.
  P2PSocketUdp(Delegate* delegate,
               mojo::PendingRemote<mojom::P2PSocketClient> client,
               mojo::PendingReceiver<mojom::P2PSocket> socket,
               P2PMessageThrottler* throttler,
               const net::NetworkTrafficAnnotationTag& traffic_annotation,
               net::NetLog* net_log,
               const DatagramServerSocketFactory& socket_factory,
               std::optional<base::UnguessableToken> devtools_token,
               bool is_socks5_tunnel = false);
  P2PSocketUdp(Delegate* delegate,
               mojo::PendingRemote<mojom::P2PSocketClient> client,
               mojo::PendingReceiver<mojom::P2PSocket> socket,
               P2PMessageThrottler* throttler,
               const net::NetworkTrafficAnnotationTag& traffic_annotation,
               net::NetLog* net_log,
               std::optional<base::UnguessableToken> devtools_token);

  P2PSocketUdp(const P2PSocketUdp&) = delete;
  P2PSocketUdp& operator=(const P2PSocketUdp&) = delete;

  ~P2PSocketUdp() override;

  // P2PSocket overrides.
  void Init(
      const net::IPEndPoint& local_address,
      uint16_t min_port,
      uint16_t max_port,
      const P2PHostAndIPEndPoint& remote_address,
      const net::NetworkAnonymizationKey& network_anonymization_key) override;

  // mojom::P2PSocket implementation:
  void Send(base::span<const uint8_t> data,
            const P2PPacketInfo& packet_info) override;
  void SendBatch(std::vector<mojom::P2PSendPacketPtr> packet_batch) override;
  void SetOption(P2PSocketOption option, int32_t value) override;

 private:
  friend class P2PSocketUdpTest;

  typedef std::set<net::IPEndPoint> ConnectedPeerSet;

  bool SendPacket(base::span<const uint8_t> data,
                  const P2PPacketInfo& packet_info);
  void DoRead();
  void OnRecv(int result);
  void MaybeDrainReceivedPackets(bool force);

  // Called when Socks5UdpTunnel's async Listen() (real TCP connect + SOCKS5
  // handshake to the proxy) completes. On failure (e.g. the proxy replied
  // REP=0x7 "command not supported" to UDP ASSOCIATE — a real, working
  // SOCKS5 proxy that just doesn't tunnel UDP), falls back to a
  // NoEgressUdpSocket: it binds a real local port so a genuine "host"
  // candidate still appears in the SDP, but unconditionally blocks every
  // send/recv so no real packet (including ICE connectivity-check pings,
  // which are STUN-format and not blocked by disabling STUN gathering) can
  // ever leak the real IP over the network. See socket_udp.cc's
  // NoEgressUdpSocket and OnSocks5ListenDone for details.
  void OnSocks5ListenDone(const net::IPEndPoint& local_address,
                          const P2PHostAndIPEndPoint& remote_address,
                          int result);
  // Binds |socket_| (already created via |factory|) to |local_address|,
  // retrying within [min_port, max_port] on failure. Calls OnError() or
  // FinishInit() depending on outcome. When |is_socks5_tunnel_| is set, skips
  // the port-range retry loop (meaningless for a tunneled socket — the real
  // local port is chosen automatically inside Socks5UdpTunnel::BindLocalUdp)
  // and instead waits for OnSocks5ListenDone via SetListenDoneCallback.
  void DoListen(const DatagramServerSocketFactory& factory,
                const net::IPEndPoint& local_address,
                uint16_t min_port,
                uint16_t max_port,
                const P2PHostAndIPEndPoint& remote_address);
  // Common post-Listen initialization (buffer setup, SocketCreated, DoRead).
  void FinishInit(const P2PHostAndIPEndPoint& remote_address);

  // Following 3 methods return false if the result was an error and the socket
  // was destroyed. The caller should stop using |this| in that case.
  [[nodiscard]] bool HandleReadResult(int result);
  [[nodiscard]] bool HandleSendResult(uint64_t packet_id,
                                      int32_t transport_sequence_number,
                                      int64_t send_time_ms,
                                      int result);
  [[nodiscard]] bool DoSend(const P2PPendingPacket& packet);

  void OnSend(uint64_t packet_id,
              int32_t transport_sequence_number,
              int64_t send_time_ms,
              int result);

  void MaybeUpdateTos(net::DiffServCodePoint dscp, net::EcnCodePoint ecn);
  net::NetLog* net_log() const { return net_log_with_source_.net_log(); }

  // Called at the end of sends to send out SendComplete to the |client_|.
  void ProcessSendCompletions();

  void ReceiveFromInterceptor(mojom::P2PReceivedPacketPtr packet,
                              scoped_refptr<net::IOBuffer> buffer);
  void SendFromInterceptor(const P2PPendingPacket& packet);
  void SendCompletionFromInterceptor(P2PSendPacketMetrics metrics);
  void DisconnectInterceptor();

  std::unique_ptr<net::DatagramServerSocket> socket_;
  scoped_refptr<net::IOBuffer> recv_buffer_;
  net::IPEndPoint recv_address_;

  // Data of `pending_recieved_packets_` are raw pointers to buffers in
  // `pending_receive_buffers_`.
  std::vector<mojom::P2PReceivedPacketPtr> pending_received_packets_;
  std::vector<scoped_refptr<net::IOBuffer>> pending_received_buffers_;

  base::circular_deque<P2PPendingPacket> send_queue_;
  bool send_pending_ = false;
  net::DiffServCodePoint last_dscp_ = net::DSCP_DEFAULT;
  net::EcnCodePoint last_ecn_ = net::ECN_DEFAULT;
  net::BackoffEntry set_tos_backoff_;

  // Set of peer for which we have received STUN binding request or
  // response or relay allocation request or response.
  ConnectedPeerSet connected_peers_;
  raw_ptr<P2PMessageThrottler> throttler_;

  const net::NetworkTrafficAnnotationTag traffic_annotation_;
  net::NetLogWithSource net_log_with_source_;
  const std::unique_ptr<ScopedThrottlingToken> throttling_token_;

  // Callback object that returns a new socket when invoked.
  DatagramServerSocketFactory socket_factory_;

  // See the matching constructor parameter's comment.
  const bool is_socks5_tunnel_ = false;

  raw_ptr<ThrottlingP2PNetworkInterceptor> interceptor_;

  // Container for batching send completions.
  std::vector<::network::P2PSendPacketMetrics> send_completions_;

  base::WeakPtrFactory<P2PSocketUdp> weak_ptr_factory_{this};
};

}  // namespace network

#endif  // SERVICES_NETWORK_P2P_SOCKET_UDP_H_
