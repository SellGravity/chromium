// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/p2p/socks5_udp_tunnel.h"

#include <stddef.h>

#include <algorithm>
#include <array>
#include <utility>

#include "base/containers/span.h"
#include "base/containers/span_reader.h"
#include "base/containers/span_writer.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/numerics/byte_conversions.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/log/net_log.h"
#include "net/socket/tcp_client_socket.h"
#include "net/socket/udp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace network {

namespace {

constexpr int kUdpRelayReadBufferSize = 65536;

// SOCKS5 constants (RFC 1928)
constexpr uint8_t kSocks5Version        = 0x05;
constexpr uint8_t kSocks5MethodNoAuth   = 0x00;
constexpr uint8_t kSocks5MethodNoAccept = 0xFF;
constexpr uint8_t kSocks5CmdUdpAssoc    = 0x03;
constexpr uint8_t kSocks5AtypIpv4       = 0x01;
constexpr uint8_t kSocks5AtypIpv6       = 0x04;
constexpr uint8_t kSocks5RepSuccess     = 0x00;

constexpr int kGreetingLen      = 3;
constexpr int kServerChoiceLen  = 2;
constexpr int kAssocRequestLen  = 10;
constexpr int kMaxAssocReplyLen = 22;
constexpr int kUdpHeaderMinLen  = 10;

net::NetworkTrafficAnnotationTag GetTrafficAnnotation() {
  return net::DefineNetworkTrafficAnnotation("socks5_udp_tunnel", R"(
      semantics {
        sender: "WebRTC SOCKS5 UDP Tunnel"
        description:
          "Establishes a SOCKS5 UDP ASSOCIATE tunnel so that WebRTC ICE/STUN "
          "UDP datagrams are relayed through the configured SOCKS5 proxy "
          "instead of being sent directly. The TCP control connection is kept "
          "open for the lifetime of the WebRTC session."
        trigger:
          "Created when --webrtc-mode=forward_udp is active and a WebRTC "
          "PeerConnection is established."
        data:
          "SOCKS5 handshake bytes and subsequent WebRTC UDP datagrams "
          "(STUN/DTLS/SRTP)."
        destination: OTHER
        destination_other: "The configured SOCKS5 proxy server."
      }
      policy {
        cookies_allowed: NO
        setting:
          "Enabled only when Chrome is launched with "
          "--webrtc-mode=forward_udp."
        policy_exception_justification: "Not a user-visible network request."
      })");
}

// Helper: construct a SpanReader<const uint8_t> from the first |len| bytes of
// an IOBufferWithSize. IOBufferWithSize::span() returns span<uint8_t>; the
// implicit conversion to span<const uint8_t> happens inside SpanReader's
// constructor via the deduction guide.
base::SpanReader<const uint8_t> MakeReader(net::IOBufferWithSize* buf,
                                            int len) {
  base::span<const uint8_t> s = buf->span().first(
      static_cast<size_t>(len));
  return base::SpanReader<const uint8_t>(s);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

Socks5UdpTunnel::Socks5UdpTunnel(const net::IPEndPoint& proxy_endpoint,
                                   net::NetLog* net_log)
    : proxy_endpoint_(proxy_endpoint),
      net_log_(net::NetLogWithSource::Make(net_log,
                                           net::NetLogSourceType::SOCKET)) {}

Socks5UdpTunnel::~Socks5UdpTunnel() {
  Close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

int Socks5UdpTunnel::Listen(const net::IPEndPoint& address) {
  DCHECK_EQ(state_, State::kInit);
  local_udp_address_ = address;
  state_ = State::kConnecting;
  DoConnect();
  return net::ERR_IO_PENDING;
}

void Socks5UdpTunnel::SetListenDoneCallback(
    net::CompletionOnceCallback callback) {
  listen_done_callback_ = std::move(callback);
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 1 — TCP connect
// ─────────────────────────────────────────────────────────────────────────────

void Socks5UdpTunnel::DoConnect() {
  net::AddressList proxy_addr_list(proxy_endpoint_);
  control_socket_ = std::make_unique<net::TCPClientSocket>(
      proxy_addr_list, nullptr, nullptr, net_log_.net_log(),
      net::NetLogSource());
  int rv = control_socket_->Connect(
      base::BindOnce(&Socks5UdpTunnel::OnConnectDone,
                     weak_factory_.GetWeakPtr()));
  if (rv != net::ERR_IO_PENDING)
    OnConnectDone(rv);
}

void Socks5UdpTunnel::OnConnectDone(int result) {
  if (result != net::OK) {
    LOG(ERROR) << "Socks5UdpTunnel: TCP connect failed: " << result;
    OnSetupComplete(result);
    return;
  }
  SendGreeting();
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 2 — Send greeting: VER(1) NMETHODS(1) METHOD(1)
// ─────────────────────────────────────────────────────────────────────────────

void Socks5UdpTunnel::SendGreeting() {
  state_ = State::kSendingGreeting;
  handshake_write_buf_ =
      base::MakeRefCounted<net::IOBufferWithSize>(kGreetingLen);
  auto writer = base::SpanWriter(handshake_write_buf_->span());
  writer.WriteU8BigEndian(kSocks5Version);
  writer.WriteU8BigEndian(uint8_t{0x01});
  writer.WriteU8BigEndian(kSocks5MethodNoAuth);

  int rv = control_socket_->Write(
      handshake_write_buf_.get(), kGreetingLen,
      base::BindOnce(&Socks5UdpTunnel::OnGreetingSent,
                     weak_factory_.GetWeakPtr()),
      GetTrafficAnnotation());
  if (rv != net::ERR_IO_PENDING)
    OnGreetingSent(rv);
}

void Socks5UdpTunnel::OnGreetingSent(int result) {
  if (result < 0) {
    LOG(ERROR) << "Socks5UdpTunnel: greeting write failed: " << result;
    OnSetupComplete(result);
    return;
  }
  ReadServerChoice();
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 3 — Read server method selection: VER(1) METHOD(1)
// ─────────────────────────────────────────────────────────────────────────────

void Socks5UdpTunnel::ReadServerChoice() {
  state_ = State::kReadingChoice;
  handshake_read_buf_ =
      base::MakeRefCounted<net::IOBufferWithSize>(kServerChoiceLen);
  handshake_read_offset_ = 0;
  DoReadServerChoice();
}

void Socks5UdpTunnel::DoReadServerChoice() {
  auto buf = base::MakeRefCounted<net::DrainableIOBuffer>(
      handshake_read_buf_, kServerChoiceLen);
  buf->DidConsume(handshake_read_offset_);
  int rv = control_socket_->Read(
      buf.get(), buf->BytesRemaining(),
      base::BindOnce(&Socks5UdpTunnel::OnServerChoiceRead,
                     weak_factory_.GetWeakPtr()));
  if (rv != net::ERR_IO_PENDING)
    OnServerChoiceRead(rv);
}

void Socks5UdpTunnel::OnServerChoiceRead(int result) {
  if (result <= 0) {
    LOG(ERROR) << "Socks5UdpTunnel: server choice read failed, result="
               << result;
    OnSetupComplete(result == 0 ? net::ERR_CONNECTION_CLOSED : result);
    return;
  }
  handshake_read_offset_ += result;
  if (handshake_read_offset_ < kServerChoiceLen) {
    DoReadServerChoice();
    return;
  }
  auto reader = MakeReader(handshake_read_buf_.get(), kServerChoiceLen);
  uint8_t ver = 0, method = 0;
  reader.ReadU8BigEndian(ver);
  reader.ReadU8BigEndian(method);
  if (ver != kSocks5Version || method == kSocks5MethodNoAccept) {
    LOG(ERROR) << "Socks5UdpTunnel: proxy rejected auth. METHOD=0x"
               << std::hex << static_cast<int>(method);
    OnSetupComplete(net::ERR_FAILED);
    return;
  }
  SendAssociateRequest();
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 4 — Send UDP ASSOCIATE request
//   VER(1) CMD(1) RSV(1) ATYP(1) 0.0.0.0(4) 0(2)
// ─────────────────────────────────────────────────────────────────────────────

void Socks5UdpTunnel::SendAssociateRequest() {
  state_ = State::kSendingAssoc;
  handshake_write_buf_ =
      base::MakeRefCounted<net::IOBufferWithSize>(kAssocRequestLen);
  auto writer = base::SpanWriter(handshake_write_buf_->span());
  writer.WriteU8BigEndian(kSocks5Version);
  writer.WriteU8BigEndian(kSocks5CmdUdpAssoc);
  writer.WriteU8BigEndian(uint8_t{0x00});   // RSV
  writer.WriteU8BigEndian(kSocks5AtypIpv4);
  writer.WriteU8BigEndian(uint8_t{0x00});   // BND.ADDR = 0.0.0.0
  writer.WriteU8BigEndian(uint8_t{0x00});
  writer.WriteU8BigEndian(uint8_t{0x00});
  writer.WriteU8BigEndian(uint8_t{0x00});
  writer.WriteU16BigEndian(uint16_t{0});    // BND.PORT = 0

  int rv = control_socket_->Write(
      handshake_write_buf_.get(), kAssocRequestLen,
      base::BindOnce(&Socks5UdpTunnel::OnAssociateRequestSent,
                     weak_factory_.GetWeakPtr()),
      GetTrafficAnnotation());
  if (rv != net::ERR_IO_PENDING)
    OnAssociateRequestSent(rv);
}

void Socks5UdpTunnel::OnAssociateRequestSent(int result) {
  if (result < 0) {
    LOG(ERROR) << "Socks5UdpTunnel: ASSOCIATE write failed: " << result;
    OnSetupComplete(result);
    return;
  }
  ReadAssociateReply();
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 5 — Read UDP ASSOCIATE reply
// ─────────────────────────────────────────────────────────────────────────────

void Socks5UdpTunnel::ReadAssociateReply() {
  state_ = State::kReadingReply;
  handshake_read_buf_ =
      base::MakeRefCounted<net::IOBufferWithSize>(kMaxAssocReplyLen);
  handshake_read_offset_ = 0;
  DoReadAssociateReply();
}

void Socks5UdpTunnel::DoReadAssociateReply() {
  int target_len = 4;
  if (handshake_read_offset_ >= 4) {
    uint8_t atyp = handshake_read_buf_->span()[3];
    if (atyp == kSocks5AtypIpv4) {
      target_len = 10;
    } else if (atyp == kSocks5AtypIpv6) {
      target_len = 22;
    } else {
      LOG(ERROR) << "Socks5UdpTunnel: unsupported ATYP in reply: "
                 << static_cast<int>(atyp);
      OnSetupComplete(net::ERR_FAILED);
      return;
    }
  }

  if (handshake_read_offset_ >= target_len) {
    ProcessAssociateReply();
    return;
  }

  auto buf = base::MakeRefCounted<net::DrainableIOBuffer>(
      handshake_read_buf_, target_len);
  buf->DidConsume(handshake_read_offset_);
  int rv = control_socket_->Read(
      buf.get(), buf->BytesRemaining(),
      base::BindOnce(&Socks5UdpTunnel::OnAssociateReplyRead,
                     weak_factory_.GetWeakPtr()));
  if (rv != net::ERR_IO_PENDING)
    OnAssociateReplyRead(rv);
}

void Socks5UdpTunnel::OnAssociateReplyRead(int result) {
  if (result <= 0) {
    LOG(ERROR) << "Socks5UdpTunnel: ASSOCIATE reply read failed, result="
               << result;
    OnSetupComplete(result == 0 ? net::ERR_CONNECTION_CLOSED : result);
    return;
  }
  handshake_read_offset_ += result;
  DoReadAssociateReply();
}

void Socks5UdpTunnel::ProcessAssociateReply() {
  auto reader = MakeReader(handshake_read_buf_.get(), handshake_read_offset_);
  uint8_t ver = 0, rep = 0, rsv = 0, atyp = 0;
  reader.ReadU8BigEndian(ver);
  reader.ReadU8BigEndian(rep);
  reader.ReadU8BigEndian(rsv);
  reader.ReadU8BigEndian(atyp);

  if (ver != kSocks5Version || rep != kSocks5RepSuccess) {
    LOG(ERROR) << "Socks5UdpTunnel: ASSOCIATE failed, REP=0x"
               << std::hex << static_cast<int>(rep);
    OnSetupComplete(net::ERR_FAILED);
    return;
  }

  net::IPEndPoint relay;
  if (!ParseAssociateReply(reader, atyp, &relay)) {
    OnSetupComplete(net::ERR_FAILED);
    return;
  }
  relay_endpoint_ = relay;
  VLOG(1) << "Socks5UdpTunnel: relay=" << relay_endpoint_.ToString();
  BindLocalUdp(local_udp_address_);
}

bool Socks5UdpTunnel::ParseAssociateReply(
    base::SpanReader<const uint8_t>& reader,
    uint8_t atyp,
    net::IPEndPoint* relay_out) {
  uint16_t port = 0;
  if (atyp == kSocks5AtypIpv4) {
    // Read<4>() returns std::optional<span<const uint8_t, 4>>
    auto addr_opt = reader.Read<4>();
    if (!addr_opt || !reader.ReadU16BigEndian(port))
      return false;
    const auto& ab = *addr_opt;
    net::IPAddress addr(ab[0u], ab[1u], ab[2u], ab[3u]);
    *relay_out = net::IPEndPoint(addr, port);
    return true;
  } else if (atyp == kSocks5AtypIpv6) {
    auto addr_opt = reader.Read<16>();
    if (!addr_opt || !reader.ReadU16BigEndian(port))
      return false;
    std::array<uint8_t, 16> addr_bytes;
    base::span(addr_bytes).copy_from(*addr_opt);
    net::IPAddress addr(addr_bytes);
    *relay_out = net::IPEndPoint(addr, port);
    return true;
  }
  LOG(ERROR) << "Socks5UdpTunnel: unsupported ATYP: "
             << static_cast<int>(atyp);
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 6 — Bind local UDP socket
// ─────────────────────────────────────────────────────────────────────────────

void Socks5UdpTunnel::BindLocalUdp(const net::IPEndPoint& local_hint) {
  udp_socket_ = std::make_unique<net::UDPServerSocket>(
      net_log_.net_log(), net::NetLogSource());
#if BUILDFLAG(IS_WIN)
  udp_socket_->UseNonBlockingIO();
#endif
  net::IPEndPoint bind_addr(local_hint.address(), 0);
  int rv = udp_socket_->Listen(bind_addr);
  if (rv != net::OK) {
    LOG(ERROR) << "Socks5UdpTunnel: UDP bind failed: " << rv;
    OnSetupComplete(rv);
    return;
  }
  net::IPEndPoint actual_local;
  if (udp_socket_->GetLocalAddress(&actual_local) == net::OK)
    local_udp_address_ = actual_local;

  udp_recv_buf_ =
      base::MakeRefCounted<net::IOBufferWithSize>(kUdpRelayReadBufferSize);
  state_ = State::kReady;
  OnSetupComplete(net::OK);
}

void Socks5UdpTunnel::OnSetupComplete(int net_error) {
  if (net_error != net::OK)
    state_ = State::kError;
  if (listen_done_callback_)
    std::move(listen_done_callback_).Run(net_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// SendTo — wrap in SOCKS5 UDP header, send to relay
// ─────────────────────────────────────────────────────────────────────────────

scoped_refptr<net::IOBuffer> Socks5UdpTunnel::WrapUdpPacket(
    net::IOBuffer* buf,
    int buf_len,
    const net::IPEndPoint& dest,
    int* out_wrapped_len) {
  const net::IPAddress& addr = dest.address();
  const int addr_len = addr.IsIPv4() ? 4 : 16;
  const uint8_t atyp = addr.IsIPv4() ? kSocks5AtypIpv4 : kSocks5AtypIpv6;
  const int header_len = 2 + 1 + 1 + addr_len + 2;
  *out_wrapped_len = header_len + buf_len;

  auto wrapped =
      base::MakeRefCounted<net::IOBufferWithSize>(*out_wrapped_len);
  auto writer = base::SpanWriter(wrapped->span());

  writer.WriteU16BigEndian(uint16_t{0});    // RSV
  writer.WriteU8BigEndian(uint8_t{0x00});   // FRAG
  writer.WriteU8BigEndian(atyp);
  writer.Write(addr.bytes());               // DST.ADDR
  writer.WriteU16BigEndian(dest.port());    // DST.PORT

  // Payload
  writer.Write(base::span<const uint8_t>(buf->span().first(
      static_cast<size_t>(buf_len))));

  return wrapped;
}

int Socks5UdpTunnel::SendTo(net::IOBuffer* buf,
                             int buf_len,
                             const net::IPEndPoint& address,
                             net::CompletionOnceCallback callback) {
  if (state_ != State::kReady)
    return net::ERR_FAILED;

  int wrapped_len = 0;
  auto wrapped = WrapUdpPacket(buf, buf_len, address, &wrapped_len);
  if (!wrapped)
    return net::ERR_ADDRESS_INVALID;

  return udp_socket_->SendTo(wrapped.get(), wrapped_len, relay_endpoint_,
                             std::move(callback));
}

// ─────────────────────────────────────────────────────────────────────────────
// RecvFrom — receive from relay, strip SOCKS5 header
// ─────────────────────────────────────────────────────────────────────────────

bool Socks5UdpTunnel::UnwrapUdpPacket(
    base::span<const uint8_t> data,
    net::IPEndPoint* source_out,
    base::span<const uint8_t>* payload_span_out) {
  if (static_cast<int>(data.size()) < kUdpHeaderMinLen)
    return false;

  base::SpanReader<const uint8_t> reader(data);
  uint16_t rsv = 0;
  uint8_t frag = 0, atyp = 0;
  reader.ReadU16BigEndian(rsv);
  reader.ReadU8BigEndian(frag);
  reader.ReadU8BigEndian(atyp);

  if (frag != 0x00) {
    VLOG(1) << "Socks5UdpTunnel: dropping fragmented datagram";
    return false;
  }

  net::IPAddress src_addr;
  uint16_t port = 0;

  if (atyp == kSocks5AtypIpv4) {
    auto addr_opt = reader.Read<4>();
    if (!addr_opt || !reader.ReadU16BigEndian(port))
      return false;
    const auto& ab = *addr_opt;
    src_addr = net::IPAddress(ab[0u], ab[1u], ab[2u], ab[3u]);
  } else if (atyp == kSocks5AtypIpv6) {
    auto addr_opt = reader.Read<16>();
    if (!addr_opt || !reader.ReadU16BigEndian(port))
      return false;
    std::array<uint8_t, 16> addr_bytes;
    base::span(addr_bytes).copy_from(*addr_opt);
    src_addr = net::IPAddress(addr_bytes);
  } else {
    return false;
  }

  *source_out = net::IPEndPoint(src_addr, port);
  *payload_span_out = reader.remaining_span();
  return true;
}

int Socks5UdpTunnel::RecvFrom(net::IOBuffer* buf,
                               int buf_len,
                               net::IPEndPoint* address,
                               net::CompletionOnceCallback callback) {
  if (state_ != State::kReady)
    return net::ERR_FAILED;

  pending_recv_buf_ = buf;
  pending_recv_buf_len_ = buf_len;
  pending_recv_address_ = address;
  pending_recv_callback_ = std::move(callback);

  DoRecvFrom();
  return net::ERR_IO_PENDING;
}

void Socks5UdpTunnel::DoRecvFrom() {
  int rv = udp_socket_->RecvFrom(
      udp_recv_buf_.get(), kUdpRelayReadBufferSize, &udp_recv_addr_,
      base::BindOnce(&Socks5UdpTunnel::OnRecvFrom,
                     weak_factory_.GetWeakPtr()));
  if (rv != net::ERR_IO_PENDING)
    OnRecvFrom(rv);
}

void Socks5UdpTunnel::OnRecvFrom(int result) {
  if (result <= 0) {
    if (pending_recv_callback_) {
      std::move(pending_recv_callback_)
          .Run(result == 0 ? net::ERR_FAILED : result);
    }
    return;
  }

  base::span<const uint8_t> raw_span(
      udp_recv_buf_->span().first(static_cast<size_t>(result)));

  net::IPEndPoint source;
  base::span<const uint8_t> payload;
  if (!UnwrapUdpPacket(raw_span, &source, &payload)) {
    DoRecvFrom();
    return;
  }

  int copy_len =
      std::min(static_cast<int>(payload.size()), pending_recv_buf_len_);
  pending_recv_buf_->span()
      .first(static_cast<size_t>(copy_len))
      .copy_from(payload.first(static_cast<size_t>(copy_len)));

  if (pending_recv_address_)
    *pending_recv_address_ = source;

  auto cb = std::move(pending_recv_callback_);
  pending_recv_buf_ = nullptr;
  pending_recv_address_ = nullptr;
  pending_recv_buf_len_ = 0;
  std::move(cb).Run(copy_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// Misc overrides
// ─────────────────────────────────────────────────────────────────────────────

int Socks5UdpTunnel::SetReceiveBufferSize(int32_t size) {
  if (!udp_socket_) return net::ERR_FAILED;
  return udp_socket_->SetReceiveBufferSize(size);
}
int Socks5UdpTunnel::SetSendBufferSize(int32_t size) {
  if (!udp_socket_) return net::ERR_FAILED;
  return udp_socket_->SetSendBufferSize(size);
}
int Socks5UdpTunnel::SetDiffServCodePoint(net::DiffServCodePoint dscp) {
  if (!udp_socket_) return net::ERR_FAILED;
  return udp_socket_->SetDiffServCodePoint(dscp);
}
int Socks5UdpTunnel::SetTos(net::DiffServCodePoint dscp,
                             net::EcnCodePoint ecn) {
  if (!udp_socket_) return net::ERR_FAILED;
  return udp_socket_->SetTos(dscp, ecn);
}
int Socks5UdpTunnel::SetRecvTos() {
  if (!udp_socket_) return net::ERR_FAILED;
  return udp_socket_->SetRecvTos();
}
int Socks5UdpTunnel::SetDoNotFragment() {
  if (!udp_socket_) return net::ERR_FAILED;
  return udp_socket_->SetDoNotFragment();
}
net::DscpAndEcn Socks5UdpTunnel::GetLastTos() const {
  if (!udp_socket_)
    return net::DscpAndEcn(net::DSCP_DEFAULT, net::ECN_DEFAULT);
  return udp_socket_->GetLastTos();
}
void Socks5UdpTunnel::Close() {
  if (udp_socket_) udp_socket_->Close();
  control_socket_.reset();
  state_ = State::kError;
}
int Socks5UdpTunnel::GetPeerAddress(net::IPEndPoint* address) const {
  *address = relay_endpoint_;
  return net::OK;
}
int Socks5UdpTunnel::GetLocalAddress(net::IPEndPoint* address) const {
  if (state_ != State::kReady) return net::ERR_FAILED;
  *address = local_udp_address_;
  return net::OK;
}
void Socks5UdpTunnel::UseNonBlockingIO() {}
void Socks5UdpTunnel::DetachFromThread() {
  if (udp_socket_) udp_socket_->DetachFromThread();
}
const net::NetLogWithSource& Socks5UdpTunnel::NetLog() const {
  return net_log_;
}
int Socks5UdpTunnel::JoinGroup(const net::IPAddress&) const {
  return net::ERR_NOT_IMPLEMENTED;
}
int Socks5UdpTunnel::LeaveGroup(const net::IPAddress&) const {
  return net::ERR_NOT_IMPLEMENTED;
}
int Socks5UdpTunnel::SetMulticastInterface(uint32_t) {
  return net::ERR_NOT_IMPLEMENTED;
}
int Socks5UdpTunnel::SetMulticastTimeToLive(int) {
  return net::ERR_NOT_IMPLEMENTED;
}
int Socks5UdpTunnel::SetMulticastLoopbackMode(bool) {
  return net::ERR_NOT_IMPLEMENTED;
}

}  // namespace network
