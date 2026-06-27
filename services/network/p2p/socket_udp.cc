// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/p2p/socket_udp.h"

#include <tuple>
#include <vector>

#include "base/containers/contains.h"
#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "base/memory/weak_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "net/base/backoff_entry.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/base/port_util.h"
#include "net/log/net_log_source.h"
#include "services/network/p2p/socket_throttler.h"
#include "services/network/p2p/socks5_udp_tunnel.h"
#include "services/network/public/cpp/p2p_socket_type.h"
#include "services/network/public/mojom/p2p.mojom.h"
#include "services/network/throttling/throttling_controller.h"
#include "services/network/throttling/throttling_network_interceptor.h"
#include "services/network/throttling/throttling_p2p_network_interceptor.h"
#include "third_party/perfetto/include/perfetto/tracing/track.h"
#include "third_party/webrtc/api/transport/stun.h"
#include "third_party/webrtc/media/base/rtp_utils.h"
#include "third_party/webrtc/rtc_base/byte_buffer.h"
#include "third_party/webrtc/rtc_base/socket_address.h"
#include "third_party/webrtc/rtc_base/time_utils.h"

namespace {

// Frequently used type of service (ToS) values. We're using this enum to log
// failures of commonly used SetTos() arguments.
//
// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class SetTosArguments {
  OTHER = 0,
  DSCP_OTHER_ECN_NOT_ECT = 1,
  DSCP_OTHER_ECN_ECT1 = 2,
  DSCP_CS0_ECN_NOT_ECT = 3,
  DSCP_CS0_ECN_ECT1 = 4,
  DSCP_CS1_ECN_NOT_ECT = 5,
  DSCP_CS1_ECN_ECT1 = 6,
  DSCP_AF41_ECN_NOT_ECT = 7,
  DSCP_AF41_ECN_ECT1 = 8,
  DSCP_AF42_ECN_NOT_ECT = 9,
  DSCP_AF42_ECN_ECT1 = 10,
  kMaxValue = DSCP_AF42_ECN_ECT1,
};

// UDP packets cannot be bigger than 64k.
const int kUdpReadBufferSize = 65536;
// Socket receive buffer size.
const int kUdpRecvSocketBufferSize = 65536;  // 64K
// Socket send buffer size.
const int kUdpSendSocketBufferSize = 65536;

constexpr net::BackoffEntry::Policy kSetTosBackoffPolicy = {
    0,          // Number of initial errors to ignore before backing off.
    100,        // Initial delay for exponential back-off in ms.
    2,          // Factor by which the delay will be multiplied.
    0.0,        // Fuzzing percentage. We're not using any fuzzing.
    60 * 1000,  // Maximum delay in ms.
    -1,         // Never discard the entry.
    false,      // Don't use initial delay.
};

// Defines set of transient errors. These errors are ignored when we get them
// from sendto() or recvfrom() calls.
//
// net::ERR_OUT_OF_MEMORY
//
// This is caused by ENOBUFS which means the buffer of the network interface
// is full.
//
// net::ERR_CONNECTION_RESET
//
// This is caused by WSAENETRESET or WSAECONNRESET which means the
// last send resulted in an "ICMP Port Unreachable" message.
struct {
  int code;
  const char* name;
} static const kTransientErrors[]{
    {net::ERR_ADDRESS_UNREACHABLE, "net::ERR_ADDRESS_UNREACHABLE"},
    {net::ERR_ADDRESS_INVALID, "net::ERR_ADDRESS_INVALID"},
    {net::ERR_ACCESS_DENIED, "net::ERR_ACCESS_DENIED"},
    {net::ERR_CONNECTION_RESET, "net::ERR_CONNECTION_RESET"},
    {net::ERR_OUT_OF_MEMORY, "net::ERR_OUT_OF_MEMORY"},
    {net::ERR_INTERNET_DISCONNECTED, "net::ERR_INTERNET_DISCONNECTED"}};

bool IsTransientError(int error) {
  for (const auto& transient_error : kTransientErrors) {
    if (transient_error.code == error)
      return true;
  }
  return false;
}

const char* GetTransientErrorName(int error) {
  for (const auto& transient_error : kTransientErrors) {
    if (transient_error.code == error)
      return transient_error.name;
  }
  return "";
}

std::unique_ptr<net::DatagramServerSocket> DefaultSocketFactory(
    net::NetLog* net_log) {
  net::UDPServerSocket* socket =
      new net::UDPServerSocket(net_log, net::NetLogSource());
#if BUILDFLAG(IS_WIN)
  socket->UseNonBlockingIO();
#endif

  return base::WrapUnique(socket);
}

// A UDP socket that binds/listens normally — so a real local port exists and
// a genuine "host" ICE candidate is produced, mDNS-obfuscated exactly like
// real Chrome — and never actually sends or receives a single real packet
// over the network. Used as the fallback when a Socks5UdpTunnel's UDP
// ASSOCIATE fails at runtime (see P2PSocketUdp::OnSocks5ListenDone): some
// SOCKS5 proxies are entirely real and working for ordinary TCP traffic but
// reply REP=0x7 "command not supported" to UDP ASSOCIATE specifically.
// Falling back to a real unproxied UDP socket there would let WebRTC use it
// for actual ICE connectivity-check pings — which are STUN-format packets
// sent directly peer-to-peer, not just to a configured STUN server — silently
// leaking the real public IP over the network exactly like the leak this
// whole project exists to close.
//
// Rather than blocking every send/recv unconditionally (this class's
// original behavior), STUN Binding Requests specifically — which is what
// WebRTC's own StunPort sends to gather a srflx candidate — get a locally
// synthesized STUN Binding Success Response with XOR-MAPPED-ADDRESS set to
// |fake_external_ip_|, without ever putting a packet on the wire. This makes
// WebRTC's own gathering logic create a genuinely real srflx Candidate in its
// internal pool (not just SDP text), so it shows up correctly in
// getStats()/chrome://webrtc-internals too — see SanitizeSdp's comment
// elsewhere in this file for why pure SDP-text fabrication can't reach those.
// Any other packet (real media, or a real ICE connectivity-check ping to an
// actual remote peer) gets no response, same as before — this profile's use
// case (leak-detection test pages) never pairs these candidates with a real
// second peer, so that distinction is moot in practice; see
// force_no_udp_egress in peer_connection_dependency_factory.cc for the same
// "not used for real calls" trade-off applied to non-SOCKS5 proxies.
class NoEgressUdpSocket : public net::DatagramServerSocket {
 public:
  NoEgressUdpSocket(net::NetLog* net_log,
                    const net::IPAddress& fake_external_ip)
      : socket_(net_log, net::NetLogSource()),
        fake_external_ip_(fake_external_ip) {
#if BUILDFLAG(IS_WIN)
    socket_.UseNonBlockingIO();
#endif
  }
  ~NoEgressUdpSocket() override = default;

  int Listen(const net::IPEndPoint& address) override {
    return socket_.Listen(address);
  }
  int RecvFrom(net::IOBuffer* buf,
               int buf_len,
               net::IPEndPoint* address,
               net::CompletionOnceCallback callback) override {
    if (!pending_fake_response_.empty()) {
      size_t copy_len = std::min(pending_fake_response_.size(),
                                 static_cast<size_t>(buf_len));
      buf->span().first(copy_len).copy_from(
          base::span(pending_fake_response_).first(copy_len));
      *address = pending_fake_response_from_;
      pending_fake_response_.clear();
      return static_cast<int>(copy_len);
    }
    // No real network I/O ever happens on this socket (see class comment
    // above) — stay pending, matching genuine async DatagramServerSocket
    // semantics, until SendTo() synthesizes a fake STUN response for us to
    // deliver via CompleteRecvFrom(). Returning a synchronous error here
    // instead (this class's original behavior) made
    // P2PSocketUdp::HandleReadResult() treat it as a fatal socket error and
    // immediately call OnError(), destroying the owning P2PSocketUdp (and
    // resetting its mojo connection) before WebRTC ever got a chance to send
    // a STUN request over it — there was no live channel left to answer.
    pending_recv_buf_ = buf;
    pending_recv_buf_len_ = buf_len;
    pending_recv_address_ = address;
    pending_recv_callback_ = std::move(callback);
    return net::ERR_IO_PENDING;
  }
  int SendTo(net::IOBuffer* buf,
             int buf_len,
             const net::IPEndPoint& address,
             net::CompletionOnceCallback callback) override {
    std::vector<uint8_t> fake_response =
        MaybeBuildFakeStunResponse(buf, buf_len);
    if (!fake_response.empty()) {
      VLOG(1) << "NoEgressUdpSocket: faking STUN Binding Success Response "
                 "for request to " << address.ToString()
              << " (no real packet sent — see class comment)";
      // Posted, not delivered inline: SendTo() runs from inside WebRTC's own
      // send path, and completing the matching RecvFrom() callback
      // synchronously from here would re-enter P2PSocketUdp's read loop
      // (DoRead() -> OnRecv() -> DoRead()) from within DoSend()'s call
      // stack — real network I/O could never do that, and callers up the
      // stack assume async completions run from a fresh task.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&NoEgressUdpSocket::CompleteRecvFrom,
                         weak_factory_.GetWeakPtr(), std::move(fake_response),
                         address));
      return buf_len;  // Pretend the send succeeded.
    }
    VLOG(1) << "NoEgressUdpSocket: blocked outgoing packet to "
            << address.ToString()
            << " (not a STUN Binding Request, or this socket has no fake "
               "external address configured)";
    return net::ERR_FAILED;
  }
  int SetReceiveBufferSize(int32_t size) override {
    return socket_.SetReceiveBufferSize(size);
  }
  int SetSendBufferSize(int32_t size) override {
    return socket_.SetSendBufferSize(size);
  }
  void AllowAddressReuse() override { socket_.AllowAddressReuse(); }
  void AllowBroadcast() override { socket_.AllowBroadcast(); }
  void AllowAddressSharingForMulticast() override {
    socket_.AllowAddressSharingForMulticast();
  }
  int JoinGroup(const net::IPAddress& group_address) const override {
    return net::ERR_NOT_IMPLEMENTED;
  }
  int LeaveGroup(const net::IPAddress& group_address) const override {
    return net::ERR_NOT_IMPLEMENTED;
  }
  int SetMulticastInterface(uint32_t interface_index) override {
    return net::ERR_NOT_IMPLEMENTED;
  }
  int SetMulticastTimeToLive(int time_to_live) override {
    return net::ERR_NOT_IMPLEMENTED;
  }
  int SetMulticastLoopbackMode(bool loopback) override {
    return net::ERR_NOT_IMPLEMENTED;
  }
  int SetDiffServCodePoint(net::DiffServCodePoint dscp) override {
    return socket_.SetDiffServCodePoint(dscp);
  }
  void DetachFromThread() override { socket_.DetachFromThread(); }
  void Close() override { socket_.Close(); }
  int GetPeerAddress(net::IPEndPoint* address) const override {
    return net::ERR_FAILED;
  }
  int GetLocalAddress(net::IPEndPoint* address) const override {
    return socket_.GetLocalAddress(address);
  }
  void UseNonBlockingIO() override { socket_.UseNonBlockingIO(); }
  int SetDoNotFragment() override { return socket_.SetDoNotFragment(); }
  int SetRecvTos() override { return socket_.SetRecvTos(); }
  int SetTos(net::DiffServCodePoint dscp, net::EcnCodePoint ecn) override {
    return socket_.SetTos(dscp, ecn);
  }
  void SetMsgConfirm(bool confirm) override {}
  const net::NetLogWithSource& NetLog() const override {
    return socket_.NetLog();
  }
  net::DscpAndEcn GetLastTos() const override { return socket_.GetLastTos(); }

 private:
  // Returns a serialized STUN Binding Success Response (XOR-MAPPED-ADDRESS =
  // |fake_external_ip_|:local-port) if |buf| is a STUN Binding Request,
  // otherwise an empty vector. See the class comment for why a real ICE
  // connectivity-check ping (also STUN Binding Request format) gets the same
  // treatment without that being a problem in practice.
  std::vector<uint8_t> MaybeBuildFakeStunResponse(net::IOBuffer* buf,
                                                  int buf_len) {
    if (buf_len <= 0 || fake_external_ip_.empty()) {
      return {};
    }
    webrtc::ByteBufferReader reader(webrtc::MakeArrayView(
        reinterpret_cast<const uint8_t*>(buf->data()),
        static_cast<size_t>(buf_len)));
    webrtc::StunMessage request;
    if (!request.Read(&reader) ||
        request.type() != webrtc::STUN_BINDING_REQUEST) {
      return {};
    }
    net::IPEndPoint local_address;
    int port = 0;
    if (socket_.GetLocalAddress(&local_address) == net::OK) {
      port = local_address.port();
    }
    webrtc::SocketAddress mapped_address(fake_external_ip_.ToString(), port);
    webrtc::StunMessage response(webrtc::STUN_BINDING_RESPONSE,
                                 request.transaction_id());
    response.AddAttribute(std::make_unique<webrtc::StunXorAddressAttribute>(
        webrtc::STUN_ATTR_XOR_MAPPED_ADDRESS, mapped_address));
    webrtc::ByteBufferWriter writer;
    if (!response.Write(&writer)) {
      return {};
    }
    webrtc::ArrayView<const uint8_t> written = writer.DataView();
    return std::vector<uint8_t>(written.begin(), written.end());
  }

  // Delivers |response| to a RecvFrom() caller: immediately if one is
  // already waiting, or queued for the next RecvFrom() call otherwise (a
  // real race is possible since SendTo() and RecvFrom() are independent
  // calls from WebRTC's perspective).
  void CompleteRecvFrom(std::vector<uint8_t> response,
                        net::IPEndPoint from_address) {
    if (!pending_recv_callback_) {
      pending_fake_response_ = std::move(response);
      pending_fake_response_from_ = from_address;
      return;
    }
    size_t copy_len = std::min(response.size(),
                               static_cast<size_t>(pending_recv_buf_len_));
    pending_recv_buf_->span().first(copy_len).copy_from(
        base::span(response).first(copy_len));
    *pending_recv_address_ = from_address;
    net::CompletionOnceCallback callback = std::move(pending_recv_callback_);
    pending_recv_buf_ = nullptr;
    pending_recv_address_ = nullptr;
    std::move(callback).Run(static_cast<int>(copy_len));
  }

  net::UDPServerSocket socket_;
  net::IPAddress fake_external_ip_;

  // Pending RecvFrom() state, valid only while a call is outstanding
  // (net::ERR_IO_PENDING was returned and |callback| not yet run).
  scoped_refptr<net::IOBuffer> pending_recv_buf_;
  int pending_recv_buf_len_ = 0;
  raw_ptr<net::IPEndPoint> pending_recv_address_ = nullptr;
  net::CompletionOnceCallback pending_recv_callback_;

  // A fake response synthesized by SendTo() before any RecvFrom() call was
  // outstanding to deliver it to; consumed by the next RecvFrom() call.
  std::vector<uint8_t> pending_fake_response_;
  net::IPEndPoint pending_fake_response_from_;

  base::WeakPtrFactory<NoEgressUdpSocket> weak_factory_{this};
};

webrtc::EcnMarking GetEcnMarking(net::DscpAndEcn tos) {
  switch (tos.ecn) {
    case net::ECN_NO_CHANGE:
      NOTREACHED();
    case net::ECN_NOT_ECT:
      return webrtc::EcnMarking::kNotEct;
    case net::ECN_ECT1:
      return webrtc::EcnMarking::kEct1;
    case net::ECN_ECT0:
      return webrtc::EcnMarking::kEct0;
    case net::ECN_CE:
      return webrtc::EcnMarking::kCe;
  }
}

SetTosArguments GetSetTosEnumForLogging(net::DiffServCodePoint dscp,
                                        net::EcnCodePoint ecn) {
  if (ecn == net::ECN_NOT_ECT) {
    switch (dscp) {
      case net::DSCP_CS0:
        return SetTosArguments::DSCP_CS0_ECN_NOT_ECT;
      case net::DSCP_CS1:
        return SetTosArguments::DSCP_CS1_ECN_NOT_ECT;
      case net::DSCP_AF41:
        return SetTosArguments::DSCP_AF41_ECN_NOT_ECT;
      case net::DSCP_AF42:
        return SetTosArguments::DSCP_AF42_ECN_NOT_ECT;
      default:
        return SetTosArguments::DSCP_OTHER_ECN_NOT_ECT;
    }
  } else if (ecn == net::ECN_ECT1) {
    switch (dscp) {
      case net::DSCP_CS0:
        return SetTosArguments::DSCP_CS0_ECN_ECT1;
      case net::DSCP_CS1:
        return SetTosArguments::DSCP_CS1_ECN_ECT1;
      case net::DSCP_AF41:
        return SetTosArguments::DSCP_AF41_ECN_ECT1;
      case net::DSCP_AF42:
        return SetTosArguments::DSCP_AF42_ECN_ECT1;
      default:
        return SetTosArguments::DSCP_OTHER_ECN_ECT1;
    }
  }

  return SetTosArguments::OTHER;
}

}  // namespace

namespace network {

P2PPendingPacket::P2PPendingPacket(
    const net::IPEndPoint& to,
    base::span<const uint8_t> content,
    const webrtc::AsyncSocketPacketOptions& options,
    uint64_t id)
    : to(to),
      data(base::MakeRefCounted<net::VectorIOBuffer>(content)),
      size(content.size()),
      packet_options(options),
      id(id) {}

P2PPendingPacket::P2PPendingPacket(const P2PPendingPacket& other) = default;
P2PPendingPacket::~P2PPendingPacket() = default;

P2PSocketUdp::P2PSocketUdp(
    Delegate* Delegate,
    mojo::PendingRemote<mojom::P2PSocketClient> client,
    mojo::PendingReceiver<mojom::P2PSocket> socket,
    P2PMessageThrottler* throttler,
    const net::NetworkTrafficAnnotationTag& traffic_annotation,
    net::NetLog* net_log,
    const DatagramServerSocketFactory& socket_factory,
    std::optional<base::UnguessableToken> devtools_token,
    bool is_socks5_tunnel)
    : P2PSocket(Delegate, std::move(client), std::move(socket), P2PSocket::UDP),
      set_tos_backoff_(&kSetTosBackoffPolicy),
      throttler_(throttler),
      traffic_annotation_(traffic_annotation),
      net_log_with_source_(
          net::NetLogWithSource::Make(net_log, net::NetLogSourceType::SOCKET)),
      throttling_token_(network::ScopedThrottlingToken::MaybeCreate(
          net_log_with_source_.source().id,
          devtools_token)),
      socket_factory_(socket_factory),
      is_socks5_tunnel_(is_socks5_tunnel),
      interceptor_(ThrottlingController::GetP2PInterceptor(
          net_log_with_source_.source().id)) {
  if (interceptor_) {
    interceptor_->RegisterSocket(this);
  }
}

P2PSocketUdp::P2PSocketUdp(
    Delegate* Delegate,
    mojo::PendingRemote<mojom::P2PSocketClient> client,
    mojo::PendingReceiver<mojom::P2PSocket> socket,
    P2PMessageThrottler* throttler,
    const net::NetworkTrafficAnnotationTag& traffic_annotation,
    net::NetLog* net_log,
    std::optional<base::UnguessableToken> devtools_token)
    : P2PSocketUdp(Delegate,
                   std::move(client),
                   std::move(socket),
                   throttler,
                   traffic_annotation,
                   net_log,
                   base::BindRepeating(&DefaultSocketFactory),
                   devtools_token) {}

P2PSocketUdp::~P2PSocketUdp() {
  if (interceptor_) {
    interceptor_->UnregisterSocket(this);
  }
}

void P2PSocketUdp::Init(
    const net::IPEndPoint& local_address,
    uint16_t min_port,
    uint16_t max_port,
    const P2PHostAndIPEndPoint& remote_address,
    const net::NetworkAnonymizationKey& network_anonymization_key) {
  DCHECK(!socket_);
  DCHECK((min_port == 0 && max_port == 0) || min_port > 0);
  DCHECK_LE(min_port, max_port);

  socket_ = socket_factory_.Run(net_log());

  DoListen(socket_factory_, local_address, min_port, max_port, remote_address);
}

void P2PSocketUdp::DoListen(const DatagramServerSocketFactory& factory,
                             const net::IPEndPoint& local_address,
                             uint16_t min_port,
                             uint16_t max_port,
                             const P2PHostAndIPEndPoint& remote_address) {
  if (is_socks5_tunnel_) {
    // Socks5UdpTunnel::Listen() is asynchronous (it runs a real TCP connect
    // + SOCKS5 handshake to the proxy before it can know whether the tunnel
    // is usable) and reports completion via SetListenDoneCallback rather
    // than its synchronous return value — see socks5_udp_tunnel.h's Listen()
    // comment. The static_cast below is safe specifically because
    // |is_socks5_tunnel_| is the caller's guarantee about |socket_|'s
    // concrete type (Chromium builds with -fno-rtti, so a checked
    // dynamic_cast isn't available here). A min/max port range is
    // meaningless for a tunneled socket — the real local port is chosen
    // automatically inside Socks5UdpTunnel::BindLocalUdp — so the port-range
    // retry loop below doesn't apply and is skipped entirely.
    static_cast<Socks5UdpTunnel*>(socket_.get())
        ->SetListenDoneCallback(base::BindOnce(
            &P2PSocketUdp::OnSocks5ListenDone, weak_ptr_factory_.GetWeakPtr(),
            local_address, remote_address));
    int result = socket_->Listen(local_address);
    if (result == net::ERR_IO_PENDING) {
      return;  // OnSocks5ListenDone() will call FinishInit() or OnError().
    }
    // Listen() only returns synchronously here on a failure that didn't
    // need any async I/O (e.g. unexpected state); handle it the same way as
    // the async failure path.
    OnSocks5ListenDone(local_address, remote_address, result);
    return;
  }

  int result = -1;
  if (min_port == 0) {
    result = socket_->Listen(local_address);
  } else if (local_address.port() == 0) {
    for (unsigned port = min_port; port <= max_port && result < 0; ++port) {
      result = socket_->Listen(net::IPEndPoint(local_address.address(), port));
      if (result < 0 && port != max_port) {
        socket_ = factory.Run(net_log());
      }
    }
  } else if (local_address.port() >= min_port &&
             local_address.port() <= max_port) {
    result = socket_->Listen(local_address);
  }
  if (result < 0) {
    LOG(ERROR) << "bind() to " << local_address.address().ToString()
               << (min_port == 0
                       ? base::StringPrintf(":%d", local_address.port())
                       : base::StringPrintf(", port range [%d-%d]", min_port,
                                            max_port))
               << " failed: " << result;
    OnError();
    return;
  }

  FinishInit(remote_address);
}

void P2PSocketUdp::OnSocks5ListenDone(
    const net::IPEndPoint& local_address,
    const P2PHostAndIPEndPoint& remote_address,
    int result) {
  if (result == net::OK) {
    FinishInit(remote_address);
    return;
  }
  // The proxy's TCP control connection works (we got this far), but it
  // rejected the UDP ASSOCIATE request itself — most commonly REP=0x7
  // "command not supported", i.e. a real, working SOCKS5 proxy that simply
  // doesn't tunnel UDP. Falling back to a real unproxied socket here would
  // let WebRTC use it for actual ICE connectivity-check pings (STUN-format
  // packets sent directly to whatever peer/leak-test address replies),
  // leaking the real public IP over the network — so instead, fall back to
  // NoEgressUdpSocket: it still binds a real local port (so a genuine "host"
  // candidate appears in the SDP, mDNS-obfuscated like real Chrome) and
  // fakes STUN Binding Responses (see its class comment) so a genuine srflx
  // candidate appears too, but no real packet of any kind ever leaves this
  // socket.
  LOG(WARNING) << "Socks5UdpTunnel Listen() failed: " << result
               << " (likely REP=0x7, a proxy that doesn't support UDP "
                  "ASSOCIATE). Falling back to a no-egress local socket.";
  // |socket_| is still the Socks5UdpTunnel whose Listen() just failed —
  // grab the proxy IP it was configured with before replacing it, so the
  // fallback's fake STUN responses report this machine's configured proxy
  // address as the (fabricated) external address.
  net::IPAddress fake_external_ip;
  if (is_socks5_tunnel_) {
    fake_external_ip = static_cast<Socks5UdpTunnel*>(socket_.get())
                            ->proxy_endpoint()
                            .address();
  }
  socket_ = std::make_unique<NoEgressUdpSocket>(net_log(), fake_external_ip);
  int listen_result = socket_->Listen(local_address);
  if (listen_result < 0) {
    LOG(ERROR) << "NoEgressUdpSocket bind to "
               << local_address.address().ToString() << " failed: "
               << listen_result;
    OnError();
    return;
  }
  FinishInit(remote_address);
}

void P2PSocketUdp::FinishInit(const P2PHostAndIPEndPoint& remote_address) {
  // Setting recv socket buffer size.
  if (socket_->SetReceiveBufferSize(kUdpRecvSocketBufferSize) != net::OK) {
    LOG(WARNING) << "Failed to set socket receive buffer size to "
                 << kUdpRecvSocketBufferSize;
  }

  // Setting socket send buffer size.
  if (socket_->SetSendBufferSize(kUdpSendSocketBufferSize) != net::OK) {
    LOG(WARNING) << "Failed to set socket send buffer size to "
                 << kUdpSendSocketBufferSize;
  }

  net::IPEndPoint address;
  int result = socket_->GetLocalAddress(&address);
  if (result < 0) {
    LOG(ERROR) << "P2PSocketUdp::Init(): unable to get local address: "
               << result;
    OnError();
    return;
  }
  VLOG(1) << "Local address: " << address.ToString();

  // NOTE: Remote address will be same as what renderer provided.
  client_->SocketCreated(address, remote_address.ip_address);

  recv_buffer_ =
      base::MakeRefCounted<net::IOBufferWithSize>(kUdpReadBufferSize);
  DoRead();
}

void P2PSocketUdp::DoRead() {
  while (true) {
    DCHECK(recv_buffer_);
    const int result = socket_->RecvFrom(
        recv_buffer_.get(), kUdpReadBufferSize, &recv_address_,
        base::BindOnce(&P2PSocketUdp::OnRecv, base::Unretained(this)));
    // If there's an error, this object is destroyed by the internal call to
    // P2PSocket::OnError, so do not reference this after `HandleReadResult`
    // returns false.
    if (!HandleReadResult(result)) {
      return;
    }
  }
}

void P2PSocketUdp::OnRecv(int result) {
  if (HandleReadResult(result))
    DoRead();
}

void P2PSocketUdp::MaybeDrainReceivedPackets(bool force) {
  if (pending_received_packets_.empty()) {
    return;
  }

  // Early drain pending received packets:
  // - If reaching maxmium allowed batching size for burst packets arrived.
  // - If reaching maxmium allowed batching buffering to mitigate impact on
  // latency.
  if (!force) {
    base::TimeDelta batching_buffering;
    if (pending_received_packets_.size() > 1) {
      batching_buffering = pending_received_packets_.back()->timestamp -
                           pending_received_packets_.front()->timestamp;
    }

    if (pending_received_packets_.size() < kUdpMaxBatchingRecvPackets &&
        batching_buffering < kUdpMaxBatchingRecvBuffering) {
      return;
    }
  }

  std::vector<mojom::P2PReceivedPacketPtr> received_packets;
  received_packets.swap(pending_received_packets_);

  UMA_HISTOGRAM_CUSTOM_COUNTS(
      "WebRTC.P2P.UDP.BatchingNumberOfReceivedPackets", received_packets.size(),
      1, kUdpMaxBatchingRecvPackets, kUdpMaxBatchingRecvPackets);

  TRACE_EVENT1("p2p", __func__, "number_of_packets", received_packets.size());
  client_->DataReceived(std::move(received_packets));

  // Release `IOBuffer` of received packets.
  std::vector<scoped_refptr<net::IOBuffer>>().swap(pending_received_buffers_);
}

bool P2PSocketUdp::HandleReadResult(int result) {
  if (result > 0) {
    auto data = recv_buffer_->first(static_cast<size_t>(result));

    if (!base::Contains(connected_peers_, recv_address_)) {
      P2PSocket::StunMessageType type;
      bool stun = GetStunPacketType(data, &type);
      if ((stun && IsRequestOrResponse(type))) {
        connected_peers_.insert(recv_address_);
      } else if (!stun || type == STUN_DATA_INDICATION) {
        LOG(ERROR) << "Received unexpected data packet from "
                   << recv_address_.ToString()
                   << " before STUN binding is finished.";
        return true;
      }
    }

    delegate_->DumpPacket(data, true);
    net::DscpAndEcn last_tos =
        socket_ == nullptr
            ? net::DscpAndEcn(net::DSCP_DEFAULT, net::ECN_DEFAULT)
            : socket_->GetLastTos();
    auto packet = mojom::P2PReceivedPacket::New(
        data, recv_address_,
        base::TimeTicks() + base::Nanoseconds(webrtc::TimeNanos()),
        GetEcnMarking(last_tos));

    if (interceptor_) {
      interceptor_->EnqueueReceive(std::move(packet), std::move(recv_buffer_),
                                   this);
    } else {
      // Save the packet to buffer and check if more packets available to batch
      // together. Socket is non-blocking IO, and that it immediately returns
      // 'ERR_IO_PENDING' if drained.
      pending_received_packets_.push_back(std::move(packet));
      pending_received_buffers_.push_back(std::move(recv_buffer_));
    }
    recv_buffer_ =
        base::MakeRefCounted<net::IOBufferWithSize>(kUdpReadBufferSize);

    MaybeDrainReceivedPackets(false);
  } else if (result == net::ERR_IO_PENDING) {
    MaybeDrainReceivedPackets(true);

    return false;
  } else if (result < 0 && !IsTransientError(result)) {
    MaybeDrainReceivedPackets(true);

    LOG(ERROR) << "Error when reading from UDP socket: " << result;
    OnError();
    return false;
  }

  return true;
}

bool P2PSocketUdp::DoSend(const P2PPendingPacket& packet) {
  int64_t send_time_us = webrtc::TimeMicros();

  if (!net::IsPortAllowedForIpEndpoint(packet.to)) {
    OnError();
    return false;
  }

  // The peer is considered not connected until the first incoming STUN
  // request/response. In that state the renderer is allowed to send only STUN
  // messages to that peer and they are throttled using the |throttler_|. This
  // has to be done here instead of Send() to ensure P2PMsg_OnSendComplete
  // messages are sent in correct order.
  if (!base::Contains(connected_peers_, packet.to)) {
    P2PSocket::StunMessageType type = P2PSocket::StunMessageType();
    bool stun = GetStunPacketType(packet.data->first(packet.size), &type);
    if (!stun || type == STUN_DATA_INDICATION) {
      LOG(ERROR) << "Page tried to send a data packet to "
                 << packet.to.ToString() << " before STUN binding is finished.";
      OnError();
      return false;
    }

    if (throttler_->DropNextPacket(packet.size) && !interceptor_) {
      VLOG(0) << "Throttling outgoing STUN message.";
      // The renderer expects P2PMsg_OnSendComplete for all packets it generates
      // and in the same order it generates them, so we need to respond even
      // when the packet is dropped.
      send_completions_.emplace_back(packet.id, packet.packet_options.packet_id,
                                     send_time_us / 1000);
      // Do not reset the socket.
      return true;
    }
  }

  TRACE_EVENT_BEGIN("p2p", "UdpAsyncSendTo", perfetto::Track(packet.id), "size",
                    packet.size);

  MaybeUpdateTos(
      static_cast<net::DiffServCodePoint>(packet.packet_options.dscp),
      packet.packet_options.ect_1 ? net::ECN_ECT1 : net::ECN_NOT_ECT);

  webrtc::ApplyPacketOptions(
      webrtc::ArrayView<uint8_t>(packet.data->bytes(), packet.size),
      packet.packet_options.packet_time_params, send_time_us);
  auto callback_binding = base::BindRepeating(
      &P2PSocketUdp::OnSend, base::Unretained(this), packet.id,
      packet.packet_options.packet_id, send_time_us / 1000);

  // TODO(crbug.com/40489281): Pass traffic annotation after
  // DatagramSocketServer is updated.
  int result = socket_->SendTo(packet.data.get(), packet.size, packet.to,
                               base::BindOnce(callback_binding));

  // sendto() may return an error, e.g. if we've received an ICMP Destination
  // Unreachable message. When this happens try sending the same packet again,
  // and just drop it if it fails again.
  if (IsTransientError(result)) {
    result = socket_->SendTo(packet.data.get(), packet.size, packet.to,
                             std::move(callback_binding));
  }

  if (result == net::ERR_IO_PENDING) {
    send_pending_ = true;
  } else {
    if (!HandleSendResult(packet.id, packet.packet_options.packet_id,
                          send_time_us / 1000, result)) {
      return false;
    }
  }

  delegate_->DumpPacket(packet.data->first(packet.size), false);

  return true;
}

void P2PSocketUdp::OnSend(uint64_t packet_id,
                          int32_t transport_sequence_number,
                          int64_t send_time_ms,
                          int result) {
  DCHECK(send_pending_);
  DCHECK_NE(result, net::ERR_IO_PENDING);

  send_pending_ = false;

  if (!HandleSendResult(packet_id, transport_sequence_number, send_time_ms,
                        result)) {
    return;
  }

  // Send next packets if we have them waiting in the buffer.
  while (!send_queue_.empty() && !send_pending_) {
    P2PPendingPacket packet = send_queue_.front();
    send_queue_.pop_front();
    if (!DoSend(packet))
      return;
  }
}

bool P2PSocketUdp::HandleSendResult(uint64_t packet_id,
                                    int32_t transport_sequence_number,
                                    int64_t send_time_ms,
                                    int result) {
  TRACE_EVENT_END("p2p", perfetto::Track(packet_id), "result", result);
  if (result < 0) {
    if (!IsTransientError(result)) {
      LOG(ERROR) << "Error when sending data in UDP socket: " << result;
      OnError();
      return false;
    }
    VLOG(0) << "sendto() has failed twice returning a "
               " transient error "
            << GetTransientErrorName(result) << ". Dropping the packet.";
  }

  if (!interceptor_) {
    send_completions_.emplace_back(packet_id, transport_sequence_number,
                                   send_time_ms);
  }

  return true;
}

void P2PSocketUdp::Send(base::span<const uint8_t> data,
                        const P2PPacketInfo& packet_info) {
  TRACE_EVENT0("net", "P2PSocketUdp::Send");
  // If there's an error in SendPacket, `this` is destroyed by the internal call
  // to P2PSocket::OnError, so do not reference this after SendPacket returns
  // false.
  if (SendPacket(data, packet_info)) {
    ProcessSendCompletions();
  }
}

bool P2PSocketUdp::SendPacket(base::span<const uint8_t> data,
                              const P2PPacketInfo& packet_info) {
  if (data.size() > kMaximumPacketSize) {
    NOTREACHED();
  }
  if (interceptor_) {
    P2PPendingPacket packet(packet_info.destination, data,
                            packet_info.packet_options, packet_info.packet_id);
    interceptor_->EnqueueSend(std::move(packet), this);
    return true;
  }

  bool result = true;
  if (send_pending_) {
    send_queue_.push_back(P2PPendingPacket(packet_info.destination, data,
                                           packet_info.packet_options,
                                           packet_info.packet_id));
  } else {
    P2PPendingPacket packet(packet_info.destination, data,
                            packet_info.packet_options, packet_info.packet_id);
    result = DoSend(packet);
  }
  return result;
}

void P2PSocketUdp::SendBatch(
    std::vector<mojom::P2PSendPacketPtr> packet_batch) {
  TRACE_EVENT0("net", "P2PSocketUdp::SendBatch");
  for (auto& packet : packet_batch) {
    // If there's an error in SendPacket, this object is destroyed by the
    // internal call to P2PSocket::OnError, so do not reference this after
    // SendPacket returns false.
    if (!SendPacket(packet->data, packet->packet_info)) {
      return;
    }
  }
  ProcessSendCompletions();
}

void P2PSocketUdp::SendFromInterceptor(const P2PPendingPacket& packet) {
  if (send_pending_) {
    send_queue_.push_back(packet);
  } else {
    std::ignore = DoSend(packet);
  }
}

void P2PSocketUdp::SetOption(P2PSocketOption option, int32_t value) {
  switch (option) {
    case P2P_SOCKET_OPT_RCVBUF:
      socket_->SetReceiveBufferSize(value);
      break;
    case P2P_SOCKET_OPT_SNDBUF:
      socket_->SetSendBufferSize(value);
      break;
    case P2P_SOCKET_OPT_DSCP:
      socket_->SetDiffServCodePoint(static_cast<net::DiffServCodePoint>(value));
      break;
    case P2P_SOCKET_OPT_RECV_ECN:
      socket_->SetRecvTos();
      break;
    default:
      NOTREACHED();
  }
}

void P2PSocketUdp::ProcessSendCompletions() {
  TRACE_EVENT0("net", "P2PSocketUdp::ProcessSendCompletions");
  if (send_completions_.empty()) {
    return;
  }
  if (send_completions_.size() == 1) {
    client_->SendComplete(send_completions_[0]);
  } else {
    client_->SendBatchComplete(send_completions_);
  }
  send_completions_.clear();
}

void P2PSocketUdp::SendCompletionFromInterceptor(P2PSendPacketMetrics metrics) {
  client_->SendComplete(metrics);
}

void P2PSocketUdp::MaybeUpdateTos(net::DiffServCodePoint dscp,
                                  net::EcnCodePoint ecn) {
  bool dscp_changed = dscp != net::DSCP_NO_CHANGE && dscp != last_dscp_;
  bool ecn_changed = ecn != net::ECN_NO_CHANGE && ecn != last_ecn_;
  if (set_tos_backoff_.ShouldRejectRequest() ||
      (!dscp_changed && !ecn_changed)) {
    return;
  }

  int result = socket_->SetTos(dscp, ecn);
  if (result == net::OK) {
    if (dscp_changed) {
      last_dscp_ = dscp;
    }
    if (ecn_changed) {
      last_ecn_ = ecn;
    }
    // Don't throttle future attempts to set the ToS byte.
    set_tos_backoff_.Reset();
  } else if (!IsTransientError(result)) {
    // A non-transient error may mean that the OS does not support setting
    // the ToS byte we want. To avoid frequent costly retries, we throttle the
    // next attempt to call SetTos.
    base::UmaHistogramEnumeration("WebRTC.P2P.UDP.SetTosErrorCountByArgument",
                              GetSetTosEnumForLogging(dscp, ecn));
    set_tos_backoff_.InformOfRequest(false);
  }
}

void P2PSocketUdp::DisconnectInterceptor() {
  interceptor_ = nullptr;
}

void P2PSocketUdp::ReceiveFromInterceptor(mojom::P2PReceivedPacketPtr packet,
                                          scoped_refptr<net::IOBuffer> buffer) {
  pending_received_packets_.push_back(std::move(packet));
  pending_received_buffers_.push_back(std::move(buffer));
  MaybeDrainReceivedPackets(true);
}

}  // namespace network
