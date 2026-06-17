// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_P2P_SOCKS5_UDP_TUNNEL_H_
#define SERVICES_NETWORK_P2P_SOCKS5_UDP_TUNNEL_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/component_export.h"
#include "base/containers/span.h"
#include "base/containers/span_reader.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "net/base/completion_once_callback.h"
#include "net/base/ip_endpoint.h"
#include "net/log/net_log_with_source.h"
#include "net/socket/datagram_server_socket.h"
#include "net/socket/stream_socket.h"
#include "net/socket/udp_server_socket.h"

namespace net {
class IOBuffer;
class IOBufferWithSize;
class NetLog;
class StreamSocket;
class ClientSocketFactory;
}  // namespace net

namespace network {

// Socks5UdpTunnel implements net::DatagramServerSocket by tunneling UDP packets
// through a SOCKS5 proxy using the UDP ASSOCIATE command (RFC 1928 §7).
//
// Lifecycle:
//   Listen() → TCP connect to proxy → SOCKS5 greeting → UDP ASSOCIATE request
//             → receive BND.ADDR:BND.PORT relay endpoint
//             → bind local UDP socket to receive relay traffic
//
// SendTo(): wraps each outgoing datagram in SOCKS5 UDP header, sends to relay.
// RecvFrom(): receives from relay, strips SOCKS5 UDP header, returns payload
//             and real source address to caller.
//
// If tunnel setup fails (TCP connect error, SOCKS5 reject, etc.), the socket
// enters kError state and all subsequent operations return net::ERR_FAILED.
// There is NO silent fallback to TCP.
class COMPONENT_EXPORT(NETWORK_SERVICE) Socks5UdpTunnel
    : public net::DatagramServerSocket {
 public:
  // |proxy_endpoint|: address of the SOCKS5 proxy (host:port).
  // |net_log|: may be null.
  // |username|, |password|: optional credentials for SOCKS5
  //   username/password sub-negotiation (RFC 1929). Pass empty strings for
  //   unauthenticated proxies.
  Socks5UdpTunnel(const net::IPEndPoint& proxy_endpoint,
                  net::NetLog* net_log,
                  std::string username = {},
                  std::string password = {});

  Socks5UdpTunnel(const Socks5UdpTunnel&) = delete;
  Socks5UdpTunnel& operator=(const Socks5UdpTunnel&) = delete;

  ~Socks5UdpTunnel() override;

  // ── net::DatagramServerSocket overrides ───────────────────────────────────

  // Initiates the SOCKS5 UDP ASSOCIATE handshake.
  // |address|: the local address/port hint (address.address() is used to
  //            bind the local UDP socket; port is ignored, an ephemeral port
  //            is chosen automatically).
  // Returns net::ERR_IO_PENDING while handshake is in progress. Callers must
  // not call RecvFrom/SendTo until the callback passed to Listen() returns OK
  // (i.e., Listen() itself completes synchronously with OK).
  // NOTE: Because DatagramServerSocket::Listen() has no callback parameter,
  // we start the async handshake internally and report completion via the
  // listen_done_callback set with SetListenDoneCallback() before Listen() is
  // called. When Listen() returns net::ERR_IO_PENDING, the caller should wait
  // for the callback before issuing I/O.
  int Listen(const net::IPEndPoint& address) override;

  int RecvFrom(net::IOBuffer* buf,
               int buf_len,
               net::IPEndPoint* address,
               net::CompletionOnceCallback callback) override;

  int SendTo(net::IOBuffer* buf,
             int buf_len,
             const net::IPEndPoint& address,
             net::CompletionOnceCallback callback) override;

  int SetReceiveBufferSize(int32_t size) override;
  int SetSendBufferSize(int32_t size) override;

  void AllowAddressReuse() override {}
  void AllowBroadcast() override {}
  void AllowAddressSharingForMulticast() override {}
  int JoinGroup(const net::IPAddress& group_address) const override;
  int LeaveGroup(const net::IPAddress& group_address) const override;
  int SetMulticastInterface(uint32_t interface_index) override;
  int SetMulticastTimeToLive(int time_to_live) override;
  int SetMulticastLoopbackMode(bool loopback) override;
  int SetDiffServCodePoint(net::DiffServCodePoint dscp) override;
  void DetachFromThread() override;

  // ── net::DatagramSocket (base of DatagramServerSocket) overrides ──────────
  void Close() override;
  int GetPeerAddress(net::IPEndPoint* address) const override;
  int GetLocalAddress(net::IPEndPoint* address) const override;
  void UseNonBlockingIO() override;
  int SetDoNotFragment() override;
  int SetRecvTos() override;
  int SetTos(net::DiffServCodePoint dscp, net::EcnCodePoint ecn) override;
  void SetMsgConfirm(bool confirm) override {}
  const net::NetLogWithSource& NetLog() const override;
  net::DscpAndEcn GetLastTos() const override;

  // Called by the owner (P2PSocketUdp via Init()) to be notified when the
  // async SOCKS5 handshake completes. |callback| is invoked with net::OK on
  // success or a net error code on failure.
  void SetListenDoneCallback(net::CompletionOnceCallback callback);

 private:
  // ── State machine ─────────────────────────────────────────────────────────
  enum class State {
    kInit,
    kConnecting,       // TCP connect to proxy in flight
    kSendingGreeting,  // Writing SOCKS5 greeting (VER NMETHODS METHODS)
    kReadingChoice,    // Reading proxy method selection
    kSendingAuth,      // Writing RFC 1929 username/password sub-auth
    kReadingAuthReply, // Reading RFC 1929 auth reply
    kSendingAssoc,     // Writing UDP ASSOCIATE request
    kReadingReply,     // Reading UDP ASSOCIATE reply
    kReady,            // Tunnel established, relay_endpoint_ valid
    kError,
  };

  // ── Handshake steps ───────────────────────────────────────────────────────
  void DoConnect();
  void OnConnectDone(int result);
  void SendGreeting();
  void OnGreetingSent(int result);
  void ReadServerChoice();
  void DoReadServerChoice();
  void OnServerChoiceRead(int result);
  // RFC 1929 sub-negotiation (only used when proxy selects method 0x02)
  void SendAuth();
  void OnAuthSent(int result);
  void ReadAuthReply();
  void OnAuthReplyRead(int result);
  void SendAssociateRequest();
  void OnAssociateRequestSent(int result);
  void ReadAssociateReply();
  void DoReadAssociateReply();
  void OnAssociateReplyRead(int result);
  void ProcessAssociateReply();
  bool ParseAssociateReply(const uint8_t* data, int len,
                           net::IPEndPoint* relay_out);
  void BindLocalUdp(const net::IPEndPoint& local_hint);
  void OnSetupComplete(int net_error);

  // ── UDP I/O helpers ───────────────────────────────────────────────────────

  // Wraps outgoing payload with SOCKS5 UDP header targeting |dest|.
  scoped_refptr<net::IOBuffer> WrapUdpPacket(net::IOBuffer* buf,
                                              int buf_len,
                                              const net::IPEndPoint& dest,
                                              int* out_wrapped_len);

  // Unwraps incoming relay datagram. On success fills |source_out| and
  // |payload_span_out| (a subspan of |data|).
  bool UnwrapUdpPacket(base::span<const uint8_t> data,
                       net::IPEndPoint* source_out,
                       base::span<const uint8_t>* payload_span_out);

  // Parses BND.ADDR + BND.PORT from a SpanReader positioned right after
  // VER/REP/RSV/ATYP in the ASSOCIATE reply.
  bool ParseAssociateReply(base::SpanReader<const uint8_t>& reader,
                            uint8_t atyp,
                            net::IPEndPoint* relay_out);

  void DoRecvFrom();
  void OnRecvFrom(int result);

  // Pending RecvFrom state
  scoped_refptr<net::IOBuffer> pending_recv_buf_;
  int pending_recv_buf_len_ = 0;
  raw_ptr<net::IPEndPoint> pending_recv_address_ = nullptr;
  net::CompletionOnceCallback pending_recv_callback_;

  // Internal read buffer for receiving from relay (includes SOCKS5 header)
  scoped_refptr<net::IOBufferWithSize> udp_recv_buf_;
  net::IPEndPoint udp_recv_addr_;

  // ── Members ───────────────────────────────────────────────────────────────
  State state_ = State::kInit;

  net::IPEndPoint proxy_endpoint_;
  net::IPEndPoint local_udp_address_;  // filled after BindLocalUdp()
  net::IPEndPoint relay_endpoint_;     // BND.ADDR:BND.PORT from proxy reply
  bool fake_success_ = false;

  // Optional proxy credentials for RFC 1929 username/password auth.
  std::string username_;
  std::string password_;

  // TCP control socket (kept alive for the lifetime of the tunnel per RFC 1928)
  std::unique_ptr<net::StreamSocket> control_socket_;

  // UDP socket used for sending/receiving relay traffic
  std::unique_ptr<net::UDPServerSocket> udp_socket_;

  // Handshake I/O buffers
  scoped_refptr<net::IOBufferWithSize> handshake_write_buf_;
  scoped_refptr<net::IOBufferWithSize> handshake_read_buf_;
  int handshake_read_offset_ = 0;

  net::NetLogWithSource net_log_;

  // Callback invoked when the async handshake completes
  net::CompletionOnceCallback listen_done_callback_;

  base::WeakPtrFactory<Socks5UdpTunnel> weak_factory_{this};
};

}  // namespace network

#endif  // SERVICES_NETWORK_P2P_SOCKS5_UDP_TUNNEL_H_
