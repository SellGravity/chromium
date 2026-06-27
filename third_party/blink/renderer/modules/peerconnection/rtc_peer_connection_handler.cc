// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/peerconnection/rtc_peer_connection_handler.h"

#include <string.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"

#include "base/compiler_specific.h"
#include "base/containers/contains.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread_checker.h"
#include "base/trace_event/trace_event.h"
#include "build/chromecast_buildflags.h"
#include "media/base/media_switches.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_rtc_session_description_init.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_union_boolean_constrainbooleanparameters.h"
#include "third_party/blink/renderer/core/frame/deprecation/deprecation.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/modules/mediastream/media_constraints.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_constraints_util.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/web_rtc_cross_thread_copier.h"
#include "third_party/blink/renderer/modules/peerconnection/peer_connection_dependency_factory.h"
#include "third_party/blink/renderer/modules/peerconnection/peer_connection_features.h"
#include "third_party/blink/renderer/modules/peerconnection/peer_connection_tracker.h"
#include "third_party/blink/renderer/modules/peerconnection/rtc_rtp_receiver_impl.h"
#include "third_party/blink/renderer/modules/peerconnection/webrtc_set_description_observer.h"
#include "third_party/blink/renderer/modules/webrtc/webrtc_audio_device_impl.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/json/json_values.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_component.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_track_platform.h"
#include "third_party/blink/renderer/platform/mediastream/webrtc_uma_histograms.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_answer_options_platform.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_event_log_output_sink.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_event_log_output_sink_proxy.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_ice_candidate_platform.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_offer_options_platform.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_rtp_sender_platform.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_rtp_transceiver_platform.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_scoped_refptr_cross_thread_copier.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_session_description_platform.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_session_description_request.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_stats.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_void_request.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_base.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_std.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/text/base64.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/wtf/thread_safe_ref_counted.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "third_party/webrtc/api/rtc_event_log_output.h"
#include "third_party/webrtc/api/units/time_delta.h"
#include "third_party/webrtc/pc/session_description.h"
#include "third_party/webrtc/rtc_base/crc32.h"

using webrtc::DataChannelInterface;
using webrtc::IceCandidate;
using webrtc::MediaStreamInterface;
using webrtc::PeerConnectionInterface;
using webrtc::PeerConnectionObserver;
using webrtc::StatsReport;
using webrtc::StatsReports;

namespace blink {

template <>
struct CrossThreadCopier<scoped_refptr<DataChannelInterface>>
    : public CrossThreadCopierPassThrough<scoped_refptr<DataChannelInterface>> {
  STATIC_ONLY(CrossThreadCopier);
};

template <>
struct CrossThreadCopier<scoped_refptr<PeerConnectionInterface>>
    : public CrossThreadCopierPassThrough<
          scoped_refptr<PeerConnectionInterface>> {
  STATIC_ONLY(CrossThreadCopier);
};

template <>
struct CrossThreadCopier<webrtc::scoped_refptr<webrtc::StatsObserver>>
    : public CrossThreadCopierPassThrough<
          webrtc::scoped_refptr<webrtc::StatsObserver>> {
  STATIC_ONLY(CrossThreadCopier);
};

namespace {

// Used to back histogram value of "WebRTC.PeerConnection.RtcpMux",
// so treat as append-only.
enum class RtcpMux { kDisabled, kEnabled, kNoMedia, kMax };

static std::string NormalizeProxyIp(const std::string& proxy_ip_raw) {
  std::string pure_ip = proxy_ip_raw;
  size_t prefix_pos = pure_ip.find("://");
  if (prefix_pos != std::string::npos) {
    pure_ip = pure_ip.substr(prefix_pos + 3);
  }
  if (!pure_ip.empty() && pure_ip.front() == '[') {
    size_t close_bracket = pure_ip.find(']');
    if (close_bracket != std::string::npos) {
      pure_ip = pure_ip.substr(1, close_bracket - 1);
    }
  } else {
    size_t last_colon = pure_ip.rfind(':');
    if (last_colon != std::string::npos) {
      if (std::count(pure_ip.begin(), pure_ip.end(), ':') == 1) {
        pure_ip = pure_ip.substr(0, last_colon);
      }
    }
  }
  return pure_ip;
}

static std::string GetWebRtcSdpProxyIp(
    const base::CommandLine* command_line) {
  if (!command_line) {
    return {};
  }

  // Priority 1: Explicit --webrtc-proxy-ip flag (user provides the desired IP directly).
  if (command_line->HasSwitch("webrtc-proxy-ip")) {
    return command_line->GetSwitchValueASCII("webrtc-proxy-ip");
  }

  // Priority 2: Auto-detect from --proxy-server flag.
  // This handles both HTTP proxy (http://IP:port) and SOCKS5 proxy (socks5://IP:port).
  // We extract the IP/host portion to use as the replacement IP in SDP sanitization.
  // NOTE: this runs for both "replace" and "forward" modes. For "forward" with a
  // SOCKS5 proxy, the network layer (Socks5UdpTunnel) already produces candidates
  // carrying the correct proxy IP, so SanitizeSdp below is normally a no-op for
  // those candidates — but it still runs unconditionally as a safety net: if the
  // SOCKS5 UDP ASSOCIATE handshake fails, P2PSocketUdp transparently falls back to
  // a native unproxied socket (see services/network/p2p/socket_udp.cc), which would
  // otherwise leak the real IP into the candidate. SanitizeSdp catches that case too.
  if (command_line->HasSwitch("webrtc-mode") &&
      command_line->HasSwitch("proxy-server")) {
    std::string mode = command_line->GetSwitchValueASCII("webrtc-mode");
    if (mode == "replace" || mode == "forward") {
      std::string proxy_str = command_line->GetSwitchValueASCII("proxy-server");

    // Strip known scheme prefixes
    for (const char* prefix : {"socks5://", "socks4://", "http://", "https://"}) {
      if (proxy_str.rfind(prefix, 0) == 0) {
        proxy_str = proxy_str.substr(strlen(prefix));
        break;
      }
    }
    
    // Strip user:pass@ credentials if present
    size_t at_pos = proxy_str.find('@');
    if (at_pos != std::string::npos) {
      proxy_str = proxy_str.substr(at_pos + 1);
    }
    
    // Now proxy_str is "host:port" or "[ipv6]:port" or just "host"
    // Extract the host/IP portion (strip the port)
    if (!proxy_str.empty() && proxy_str[0] == '[') {
      // IPv6 literal: [::1]:1080
      size_t close = proxy_str.find(']');
      if (close != std::string::npos) {
        proxy_str = proxy_str.substr(1, close - 1);
      }
    } else {
      // IPv4 or hostname: 1.2.3.4:1080
      size_t colon = proxy_str.rfind(':');
      if (colon != std::string::npos) {
        // Only strip if there's exactly one colon (IPv4:port), not IPv6
        if (std::count(proxy_str.begin(), proxy_str.end(), ':') == 1) {
          proxy_str = proxy_str.substr(0, colon);
        }
      }
    }
    
    if (!proxy_str.empty()) {
      VLOG(3) << "WebRTC replace mode: auto-detected proxy IP from --proxy-server: " << proxy_str;
      return proxy_str;
    }
    } // Closes if (mode == "replace" || mode == "forward")
  }

  return {};
}

// "replace" mode runs WebRTC's gathering completely unmodified (real STUN,
// real public-IP srflx) and only rewrites the leaked IP text afterward — as
// opposed to "forward" mode, which tunnels real traffic through a proxy (or,
// for non-SOCKS5 proxies, fabricates candidates from scratch). SanitizeSdp's
// typ=="srflx" branch needs to tell these apart: forward mode's
// BuildFakeSrflxPair call there exists to synthesize a believable multi-
// homed-looking PAIR when no second real srflx exists yet (see that call's
// own comment) — replace mode already has a genuinely real, singular srflx
// per host that just needs its address text swapped, so reusing forward
// mode's pairing logic for replace mode fabricates an extra candidate that
// was never in genuine Chrome's actual output for that connection.
static bool IsReplaceMode(const base::CommandLine* command_line) {
  return command_line && command_line->HasSwitch("webrtc-mode") &&
         command_line->GetSwitchValueASCII("webrtc-mode") == "replace";
}

// Mirrors the is_socks5 literal-IP check in
// peer_connection_dependency_factory.cc's CreatePortAllocator(), which
// decides force_no_udp_egress (disabling STUN at the network layer because
// HTTP/SOCKS4/hostname proxies can't tunnel UDP). When that check is false
// there, no real srflx candidate will ever be gathered for this connection,
// so SanitizeSdp needs to know to fabricate one from a host candidate
// instead of waiting for (and duplicating) a real one.
static bool IsForwardModeUsingNonSocks5Proxy(
    const base::CommandLine* command_line) {
  if (!command_line || !command_line->HasSwitch("webrtc-mode") ||
      command_line->GetSwitchValueASCII("webrtc-mode") != "forward" ||
      !command_line->HasSwitch("proxy-server")) {
    return false;
  }
  std::string proxy_str = command_line->GetSwitchValueASCII("proxy-server");
  const char kSocks5Prefix[] = "socks5://";
  constexpr size_t kSocks5PrefixLen = sizeof(kSocks5Prefix) - 1;
  if (proxy_str.length() < kSocks5PrefixLen ||
      !std::equal(proxy_str.begin(), proxy_str.begin() + kSocks5PrefixLen,
                  kSocks5Prefix,
                  [](char a, char b) { return tolower(a) == b; })) {
    return true;  // Not socks5:// at all.
  }
  std::string host_port_str = proxy_str.substr(kSocks5PrefixLen);
  auto at_pos = host_port_str.rfind('@');
  if (at_pos != std::string::npos) {
    host_port_str = host_port_str.substr(at_pos + 1);
  }
  while (!host_port_str.empty() && host_port_str.back() == '/') {
    host_port_str.pop_back();
  }
  auto colon_pos = host_port_str.rfind(':');
  std::string host_only = (colon_pos != std::string::npos)
                               ? host_port_str.substr(0, colon_pos)
                               : host_port_str;
  if (!host_only.empty() && host_only.front() == '[' &&
      host_only.back() == ']') {
    host_only = host_only.substr(1, host_only.length() - 2);
  }
  for (char c : host_only) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F') || c == '.' || c == ':')) {
      return true;  // Hostname, not an IP literal -> can't be tunneled.
    }
  }
  return false;  // socks5:// with an IP literal -> real tunnel will be used.
}

// Builds a fake 2-candidate srflx pair (one with raddr 0.0.0.0, one with
// raddr=proxy_ip) from |seed_parts| — the space-split fields of a real
// candidate line. |seed_parts| can be either a real srflx candidate
// (priority_type_delta=0) or, when no real srflx will ever be gathered
// (force_no_udp_egress / non-SOCKS5 forward-mode proxy — see
// peer_connection_dependency_factory.cc), a host candidate's fields used as
// the closest real network event to derive a believable srflx from
// (priority_type_delta = ICE_TYPE_PREFERENCE_HOST - ICE_TYPE_PREFERENCE_SRFLX
// = 26, per third_party/webrtc/p2p/base/p2p_constants.h, shifted into the
// priority formula's top byte).
static void BuildFakeSrflxPair(const std::vector<std::string>& seed_parts,
                                const std::string& proxy_ip,
                                bool is_ipv6,
                                uint32_t priority_type_delta,
                                std::string* cand1,
                                std::string* cand2) {
  // Candidate 1: raddr 0.0.0.0, foundation/component untouched (only valid
  // when seed is already srflx; for a host seed we still inherit its real,
  // in-range foundation rather than fabricating one for this slot, exactly
  // mirroring how a genuine second-NIC srflx would carry its own foundation).
  std::vector<std::string> parts1 = seed_parts;
  std::string prefix = parts1[0].substr(0, parts1[0].find(':') + 1);
  std::string foundation = parts1[0].substr(parts1[0].find(':') + 1);
  parts1[4] = proxy_ip;
  parts1[7] = "srflx";

  uint32_t prio1 = 0;
  if (base::StringToUint(parts1[3], &prio1)) {
    prio1 -= priority_type_delta << 24;
    parts1[3] = base::NumberToString(prio1);
  }

  auto raddr1_it = std::find(parts1.begin(), parts1.end(), "raddr");
  if (raddr1_it != parts1.end() && (raddr1_it + 1) != parts1.end()) {
    *(raddr1_it + 1) = is_ipv6 ? "::" : "0.0.0.0";
  } else {
    // Insert right after "typ TYPE" (index 8), not at the end. Real
    // Chrome's canonical attribute order is "typ TYPE raddr R rport P
    // generation G ufrag U network-cost C" — appending at the end (this
    // function's previous behavior) breaks that when the seed has no
    // raddr/rport already, e.g. a host candidate seed for the
    // SOCKS5-ASSOCIATE-fail derive path, producing a visibly wrong
    // "generation 0 ufrag X network-cost 999 raddr ... rport ..." order.
    parts1.insert(parts1.begin() + 8, {"raddr", is_ipv6 ? "::" : "0.0.0.0"});
  }
  auto rport1_it = std::find(parts1.begin(), parts1.end(), "rport");
  if (rport1_it != parts1.end() && (rport1_it + 1) != parts1.end()) {
    *(rport1_it + 1) = "0";
  } else {
    auto raddr1_it2 = std::find(parts1.begin(), parts1.end(), "raddr");
    size_t insert_idx = (raddr1_it2 != parts1.end())
                             ? (raddr1_it2 - parts1.begin() + 2)
                             : 8;
    parts1.insert(parts1.begin() + insert_idx, {"rport", "0"});
  }

  // Candidate 2: raddr=proxy_ip. Foundation must look like an independent
  // real candidate's, i.e. a plausible CRC32 output (real Chrome foundations
  // are always <= 4294967295 — at most 10 decimal digits), so we hash an
  // analogous, distinguishing string through the same CRC32 routine WebRTC
  // uses rather than mutating the original foundation string directly.
  std::vector<std::string> parts2 = parts1;
  std::string second_seed = "srflx" + proxy_ip + "udp" + foundation;
  uint32_t second_foundation_crc = webrtc::ComputeCrc32(second_seed);
  parts2[0] = prefix + base::NumberToString(second_foundation_crc);

  auto raddr2_it = std::find(parts2.begin(), parts2.end(), "raddr");
  if (raddr2_it != parts2.end() && (raddr2_it + 1) != parts2.end()) {
    *(raddr2_it + 1) = proxy_ip;
  }

  // Vary priority/port to simulate a second adapter, using the EXACT fixed
  // delta genuine Chrome uses — confirmed from real packet captures of two
  // genuine host candidates gathered on the same machine: priority always
  // differs by exactly 2560 (= 10 * 256, i.e. local_pref rank differs by 10)
  // and port is always exactly +1 (two sequentially-bound sockets). This is
  // NOT a per-session/per-candidate random or pseudo-random value: real
  // Chrome's own pairing is this exact fixed delta every time, on every
  // machine, so reproducing it exactly is what matches genuine output —
  // varying it would itself be the deviation from real Chrome.
  static constexpr uint32_t kSecondCandidatePriorityDelta = 2560;
  static constexpr uint32_t kSecondCandidatePortDelta = 1;
  uint32_t prio2 = 0;
  if (base::StringToUint(parts2[3], &prio2)) {
    parts2[3] = base::NumberToString(prio2 + kSecondCandidatePriorityDelta);
  }
  uint32_t port2 = 0;
  if (base::StringToUint(parts2[5], &port2) && port2 > 0) {
    uint32_t new_port = port2 + kSecondCandidatePortDelta;
    if (new_port > 65535) {
      new_port = port2 - kSecondCandidatePortDelta;
    }
    parts2[5] = base::NumberToString(new_port);
  }

  auto join = [](const std::vector<std::string>& p) {
    std::string s;
    for (size_t i = 0; i < p.size(); ++i) {
      if (i > 0) s += " ";
      s += p[i];
    }
    return s;
  };
  *cand1 = join(parts1);
  *cand2 = join(parts2);
}

// Extracts the value of the first "a=ice-ufrag:" line found in |sdp|, or an
// empty string if none exists. The ICE ufrag is stable for the lifetime of
// one ICE generation (only changes on an ICE restart), so it doubles as a
// natural per-session seed for fabricating candidates deterministically.
static std::string ExtractIceUfrag(const std::string& sdp) {
  const char kUfragPrefix[] = "a=ice-ufrag:";
  size_t pos = sdp.find(kUfragPrefix);
  if (pos == std::string::npos) {
    return {};
  }
  pos += strlen(kUfragPrefix);
  size_t end = sdp.find_first_of("\r\n", pos);
  return sdp.substr(pos, end == std::string::npos ? std::string::npos
                                                    : end - pos);
}

// Fabricates a believable host+srflx candidate triplet entirely from
// scratch, deriving every field deterministically (via CRC32) from |ufrag|
// instead of from real network state or randomness. Used when forward mode
// is using a non-SOCKS5 proxy: force_no_udp_egress (see
// peer_connection_dependency_factory.cc) guarantees libwebrtc never creates
// a single real local Port for this connection (so no real ICE connectivity
// check can ever leak the real IP), which also means there is no real host
// candidate left to seed BuildFakeSrflxPair from. Deriving purely from
// |ufrag| (rather than base::RandInt/base::Uuid) means this function
// produces byte-identical output no matter which of the two independent
// call sites invokes it — RTCPeerConnectionHandler::
// MaybeFabricateForwardModeCandidates (trickled onicecandidate events) and
// SanitizeSdp's full-SDP finalization below (pc.localDescription.sdp) — so
// the two never visibly disagree about what the "same" candidate looks like.
static std::vector<std::string> BuildFabricatedHostParts(
    const std::string& ufrag,
    uint32_t foundation_crc,
    uint32_t host_priority,
    uint32_t host_port,
    uint32_t uuid_seed_a,
    uint32_t uuid_seed_b) {
  // Shaped like a real UUID v4 (genuine Chrome's mDNS-obfuscated host
  // candidate hostname), not a real RFC 4122 UUID — just needs to look like
  // one in the SDP.
  std::string host_address = base::StringPrintf(
      "%08x-%04x-4%03x-%04x-%08x%04x.local", uuid_seed_a,
      (uuid_seed_a >> 16) & 0xFFFFu, uuid_seed_b & 0xFFFu,
      0x8000u | ((uuid_seed_b >> 16) & 0x3FFFu), foundation_crc,
      host_port & 0xFFFFu);

  std::vector<std::string> host_parts = {
      "candidate:" + base::NumberToString(foundation_crc),
      "1",
      "udp",
      base::NumberToString(host_priority),
      host_address,
      base::NumberToString(host_port),
      "typ",
      "host",
      "generation",
      "0",
  };
  if (!ufrag.empty()) {
    host_parts.push_back("ufrag");
    host_parts.push_back(ufrag);
  }
  host_parts.push_back("network-cost");
  host_parts.push_back("999");
  return host_parts;
}

static std::string JoinParts(const std::vector<std::string>& parts) {
  std::string s;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) s += " ";
    s += parts[i];
  }
  return s;
}

// True if |address| is already a genuine-Chrome-style mDNS-obfuscated
// hostname (a UUID-shaped name ending in ".local"), as opposed to a raw IP
// literal. Chromium's native mDNS hiding does not fire for an opaque origin
// (e.g. about:blank) — when it doesn't, a host candidate's address field is
// the real local IP/IPv6 literal, exactly the leak this forward-mode feature
// exists to prevent. This is the check for that safety net below.
static bool IsMdnsHostname(const std::string& address) {
  return address.size() > 6 &&
         address.compare(address.size() - 6, 6, ".local") == 0;
}

// Masks a real local address that leaked into a host candidate's address
// field (see IsMdnsHostname's comment) with a synthetic UUID-shaped ".local"
// hostname, deterministically derived from the real address so repeated SDP
// reads of the same candidate (full-sdp vs trickle call sites, multiple
// localDescription.sdp reads) produce the identical mask.
static std::string MaskRealHostAddress(const std::string& real_address) {
  uint32_t seed_a = webrtc::ComputeCrc32("mask-host-addr-a:" + real_address);
  uint32_t seed_b = webrtc::ComputeCrc32("mask-host-addr-b:" + real_address);
  return base::StringPrintf(
      "%08x-%04x-4%03x-%04x-%08x%04x.local", seed_a,
      (seed_a >> 16) & 0xFFFFu, seed_b & 0xFFFu,
      0x8000u | ((seed_b >> 16) & 0x3FFFu), seed_a, seed_b & 0xFFFFu);
}

// Fabricates a believable 2-host + 2-srflx candidate set entirely from
// scratch, deriving every field deterministically (via CRC32) from |ufrag|
// instead of from real network state or randomness. Used when forward mode
// is using a non-SOCKS5 proxy: force_no_udp_egress (see
// peer_connection_dependency_factory.cc) guarantees libwebrtc never creates
// a single real local Port for this connection (so no real ICE connectivity
// check can ever leak the real IP), which also means there is no real host
// candidate left to seed BuildFakeSrflxPair from. Deriving purely from
// |ufrag| (rather than base::RandInt/base::Uuid) means this function
// produces byte-identical output no matter which of the two independent
// call sites invokes it — RTCPeerConnectionHandler::
// MaybeFabricateForwardModeCandidates (trickled onicecandidate events) and
// SanitizeSdp's full-SDP finalization below (pc.localDescription.sdp) — so
// the two never visibly disagree about what the "same" candidate looks like.
//
// |host_line2| mirrors the second host candidate genuine multi-homed Chrome
// almost always also gathers (confirmed from real packet captures of two
// genuine host candidates on the same machine): its priority and port are
// exactly host1's + the same fixed deltas (2560 / +1) BuildFakeSrflxPair
// uses for the srflx pair — see its comment for why this is a fixed,
// reproduced-exactly delta rather than a randomized one. No srflx is derived
// from host_line2; only host1 seeds the srflx pair, to keep exactly 2 srflx
// total regardless of host-candidate count.
static void FabricateHostAndSrflxTriplet(const std::string& ufrag,
                                          const std::string& proxy_ip,
                                          bool is_ipv6,
                                          std::string* host_line,
                                          std::string* host_line2,
                                          std::string* srflx1,
                                          std::string* srflx2) {
  uint32_t foundation_crc = webrtc::ComputeCrc32("fabricated-host:" + ufrag);
  // Real Chrome's host candidate local_pref is small (network-interface
  // ranking, not a port/random value) — confirmed from real packet captures
  // showing local_pref values like 30/40 for two host candidates on the same
  // machine (priorities 2113937151/2113939711, delta exactly 2560 = 10*256).
  // The full 16-bit range (0-65535) this used to draw from produced
  // local_pref values like 45172, which no genuine network ranking algorithm
  // would ever assign — bound it to the same small, believable range real
  // Chrome's local_pref actually occupies.
  uint32_t local_pref =
      1 + webrtc::ComputeCrc32("fabricated-local-pref:" + ufrag) % 100;
  uint32_t host_priority = (126u << 24) | (local_pref << 8) | 255u;
  uint32_t port_seed = webrtc::ComputeCrc32("fabricated-port:" + ufrag);
  uint32_t host_port = 1024 + (port_seed % (65536 - 1024));
  uint32_t uuid_seed_a = webrtc::ComputeCrc32("fabricated-uuid-a:" + ufrag);
  uint32_t uuid_seed_b = webrtc::ComputeCrc32("fabricated-uuid-b:" + ufrag);

  std::vector<std::string> host_parts = BuildFabricatedHostParts(
      ufrag, foundation_crc, host_priority, host_port, uuid_seed_a,
      uuid_seed_b);
  *host_line = JoinParts(host_parts);

  static constexpr uint32_t kSecondHostPriorityDelta = 2560;
  static constexpr uint32_t kSecondHostPortDelta = 1;
  uint32_t foundation_crc2 =
      webrtc::ComputeCrc32("fabricated-host2:" + ufrag);
  uint32_t uuid_seed_a2 = webrtc::ComputeCrc32("fabricated-uuid-a2:" + ufrag);
  uint32_t uuid_seed_b2 = webrtc::ComputeCrc32("fabricated-uuid-b2:" + ufrag);
  uint32_t host_port2 = host_port + kSecondHostPortDelta;
  if (host_port2 > 65535) {
    host_port2 = host_port - kSecondHostPortDelta;
  }
  *host_line2 = JoinParts(BuildFabricatedHostParts(
      ufrag, foundation_crc2, host_priority + kSecondHostPriorityDelta,
      host_port2, uuid_seed_a2, uuid_seed_b2));

  BuildFakeSrflxPair(host_parts, proxy_ip, is_ipv6, /*priority_type_delta=*/26,
                      srflx1, srflx2);
}

static String SanitizeSdp(const String& sdp, const std::string& proxy_ip_raw, int* shared_srflx_count = nullptr, std::vector<std::string>* extra_candidates = nullptr, bool fabricate_srflx_from_host = false, HashSet<int>* trickle_mline_srflx_done = nullptr, int sdp_mline_index = 0, HashMap<int, int>* trickle_native_srflx_count = nullptr, HashMap<int, unsigned>* trickle_native_srflx_first_priority = nullptr, bool is_replace_mode = false) {
  std::string sdp_str = sdp.Utf8();
  if (proxy_ip_raw.empty()) return sdp;

  std::string proxy_ip = NormalizeProxyIp(proxy_ip_raw);
  bool is_ipv6 = proxy_ip.find(':') != std::string::npos;

  int local_srflx_count = 0;
  int* srflx_count = shared_srflx_count ? shared_srflx_count : &local_srflx_count;
  // Set when a "typ host" line is found anywhere in |sdp|. Tracked
  // separately from |*srflx_count| (which also gates the unrelated
  // is_first_srflx real-srflx-duplication logic above) so that seeing a
  // host candidate doesn't perturb that counter — it only needs to make the
  // from-scratch fabrication below a no-op. See its use below for why.
  bool host_already_present = false;
  // Extracted once up front: once this SanitizeSdp output (with fabricated
  // candidates embedded as text) is handed to setLocalDescription(),
  // libwebrtc parses it into real internal cricket::Candidate objects — and
  // a later read of local_description()->ToString() re-serializes from
  // those parsed objects, not the original string. That parse/re-serialize
  // round-trip silently drops extension attributes libwebrtc's Candidate
  // parser doesn't track (ufrag, raddr, rport), even though it keeps others
  // (network-cost). NOTE: this is a real native-pool reflection of our
  // fabricated text (the candidates ARE genuinely present in
  // local_description() from that point on, not just SDP text) — but it
  // still isn't visible via getStats(), which reads from WebRTC's gathering
  // pipeline (StunPort etc.), not from the parsed SessionDescription. Re-add
  // the dropped attributes below from this SDP's own ufrag/position so
  // every read looks identically complete, not just the first
  // (pre-round-trip) one.
  std::string current_ufrag = ExtractIceUfrag(sdp_str);
  auto find_value = [](const std::vector<std::string>& p,
                        const std::string& key,
                        const std::string& fallback) {
    auto it = std::find(p.begin(), p.end(), key);
    return (it != p.end() && (it + 1) != p.end()) ? *(it + 1) : fallback;
  };
  // Rebuilds |p|'s attributes after "typ TYPE" (index 8 on) in the exact
  // canonical order genuine Chrome emits: [raddr R rport P] generation G
  // ufrag U network-cost C. Naively re-appending only the attributes
  // current_ufrag's round-trip dropped — this function's first version —
  // produced "generation 0 network-cost 999 ufrag X" instead of "generation
  // 0 ufrag X network-cost 999", a visible deviation from genuine Chrome's
  // fixed attribute order. Pulls existing values from |p| where present so
  // a field that survived the round-trip keeps its real value.
  auto rebuild_candidate_attrs = [&find_value](
      std::vector<std::string>& p, bool has_raddr,
      const std::string& raddr_default, const std::string& ufrag_value,
      bool force_raddr = false) {
    // |force_raddr| bypasses the "keep whatever raddr is already present"
    // default below: native srflx candidates (the ones already
    // addr==proxy_ip) always arrive with an explicit "raddr 0.0.0.0" baked
    // in (set by NoEgressUdpSocket's synthetic STUN responder), so
    // find_value's normal "only use raddr_default if the field is MISSING"
    // logic never actually applies raddr_default for them — the existing
    // 0.0.0.0 always wins, leaving every native srflx in a connection
    // showing 0.0.0.0 instead of alternating with the proxy IP like genuine
    // multi-homed Chrome. force_raddr=true makes the caller's computed
    // value (0.0.0.0 for the very first srflx in the connection, proxy_ip
    // for every other one) win outright instead.
    std::string raddr_val =
        !has_raddr ? std::string()
                   : force_raddr ? raddr_default
                                 : find_value(p, "raddr", raddr_default);
    std::string rport_val = has_raddr ? find_value(p, "rport", "0")
                                       : std::string();
    std::string generation_val = find_value(p, "generation", "0");
    std::string ufrag_val =
        !ufrag_value.empty() ? ufrag_value : find_value(p, "ufrag", "");
    std::string netcost_val = find_value(p, "network-cost", "999");
    std::vector<std::string> rebuilt(p.begin(), p.begin() + 8);
    if (has_raddr) {
      rebuilt.push_back("raddr");
      rebuilt.push_back(raddr_val);
      rebuilt.push_back("rport");
      rebuilt.push_back(rport_val);
    }
    rebuilt.push_back("generation");
    rebuilt.push_back(generation_val);
    if (!ufrag_val.empty()) {
      rebuilt.push_back("ufrag");
      rebuilt.push_back(ufrag_val);
    }
    rebuilt.push_back("network-cost");
    rebuilt.push_back(netcost_val);
    p = rebuilt;
  };
  // Pre-split into per-"m=" section text (boundaries at "\nm=") so the host
  // branch below can check "does THIS host's OWN m= section already have a
  // real srflx", independent of other sections. A connection's m=audio,
  // m=video, m=application lines are not always unified under one BUNDLEd
  // ICE transport — each can gather its own independent host candidates on
  // its own local port — so a single connection-wide "have we derived a
  // pair yet" flag wrongly makes only the FIRST section's host group ever
  // get a derived srflx pair, leaving every other section host-only.
  // Index 0 is the preamble (session-level lines before the first m=);
  // index N is the Nth "m=" section's own content.
  std::vector<bool> section_has_real_srflx;
  {
    size_t scan_start = 0;
    while (true) {
      size_t next_m = sdp_str.find("\nm=", scan_start);
      size_t section_end = (next_m == std::string::npos) ? sdp_str.size() : next_m;
      std::string section_text = sdp_str.substr(scan_start, section_end - scan_start);
      section_has_real_srflx.push_back(section_text.find(" typ srflx") !=
                                       std::string::npos);
      if (next_m == std::string::npos) break;
      scan_start = next_m + 1;
    }
  }
  // One entry per "m=" section encountered so far in this call (0-indexed,
  // parallel to |host_insert_positions| below): the srflx pair (if any)
  // derived for THAT section's own first host line this call, to be
  // inserted into |result| only after the loop finishes — right after that
  // section's own LAST host line, not immediately after the one that
  // seeded it. Genuine Chrome groups all host candidates together before
  // any srflx (host1, host2, srflx1, srflx2); inserting inline right after
  // the seeding host line instead produced a visibly wrong
  // host1/srflx1/srflx2/host2 order.
  std::vector<std::pair<std::string, std::string>> section_pending_pairs;
  // One entry per "m=" section encountered so far, holding the position in
  // |result| right after that section's own last host line (npos if none
  // seen yet in that section).
  std::vector<size_t> host_insert_positions;
  // One entry per "m=" section encountered so far this call: count of REAL
  // native srflx candidates (address already equals the proxy IP) let
  // through in THAT section. A connection whose m=audio/m=video/application
  // each gather their own independent host candidates genuinely produces 2
  // real srflx PER section when the SOCKS5 UDP tunnel works end-to-end —
  // capping at 2 once for the whole SDP (the old behavior) spends the
  // budget on the first section's pair and drops every later section's
  // genuine srflx outright, which is exactly the "m=video/application
  // missing srflx" symptom. Parallel to |host_insert_positions|.
  std::vector<int> section_native_srflx_count;
  // Parallel to |section_native_srflx_count|: the priority field of the
  // FIRST native srflx let through for that section, so the second one
  // (which NoEgressUdpSocket's synthetic STUN responder always gives an
  // IDENTICAL priority to — a native-pipeline bug, see
  // trickle_native_srflx_first_priority_'s comment) can be rewritten to
  // genuine Chrome's fixed +2560 delta instead of staying identical.
  std::vector<unsigned> section_native_srflx_first_priority;

  std::string result;
  size_t start = 0;
  while (start < sdp_str.length()) {
    size_t end = sdp_str.find('\n', start);
    std::string line = (end == std::string::npos) ? sdp_str.substr(start) : sdp_str.substr(start, end - start);

    bool has_cr = false;
    if (!line.empty() && line.back() == '\r') {
      has_cr = true;
      line.pop_back();
    }

    bool drop_line = false;
    bool is_host_line = false;

    if (line.rfind("m=", 0) == 0) {
      // New media section starting — give it its own slot to track where
      // ITS host group ends, so the deferred derived-pair insertion below
      // can target every section, not just the first.
      host_insert_positions.push_back(std::string::npos);
      section_pending_pairs.push_back({});
      section_native_srflx_count.push_back(0);
      section_native_srflx_first_priority.push_back(0);
    }

    if (line.rfind("c=IN IP4 ", 0) == 0 || line.rfind("c=IN IP6 ", 0) == 0) {
      line = (is_ipv6 ? "c=IN IP6 " : "c=IN IP4 ") + proxy_ip;
    } else if (line.rfind("a=candidate:", 0) == 0 || line.rfind("candidate:", 0) == 0) {
      std::vector<std::string> parts;
      size_t pos = 0;
      while (pos < line.length()) {
        size_t space = line.find(' ', pos);
        if (space == std::string::npos) {
          parts.push_back(line.substr(pos));
          break;
        }
        parts.push_back(line.substr(pos, space - pos));
        pos = space + 1;
      }

      // Format: candidate:foundation comp transport priority IP port typ type ...
      if (parts.size() >= 8 && parts[6] == "typ") {
        std::string typ = parts[7];

        bool line_fully_built = false;
        is_host_line = (typ == "host");

        if (typ == "srflx") {
          VLOG(1) << "SanitizeSdp: srflx line seen, call_site="
                  << (extra_candidates ? "trickle(OnIceCandidate)"
                                       : "full-sdp(CreateWebKitSessionDescription)")
                  << " addr=" << parts[4] << " proxy_ip=" << proxy_ip
                  << " addr_eq_proxy=" << (parts[4] == proxy_ip)
                  << " *srflx_count=" << *srflx_count
                  << " shared_srflx_count_ptr=" << shared_srflx_count
                  << " line='" << line << "'";
          // Only rewrite if the address isn't already the proxy IP. When the
          // SOCKS5 tunnel (forward mode) ran correctly, this candidate's IP
          // already IS proxy_ip — mutating foundation/raddr/rport in that case
          // would itself be a deviation from genuine unmodified Chrome output,
          // not a "safe no-op".
          if (parts[4] != proxy_ip && is_replace_mode) {
            // Replace mode: this is a genuinely real srflx (real STUN, real
            // public IP, since replace mode never tunnels) that just needs
            // its address text swapped to the configured proxy IP — a
            // simple 1:1 in-place rewrite, NOT the forward-mode pairing
            // below (see IsReplaceMode's comment for why reusing that here
            // fabricated an extra candidate genuine Chrome never produced
            // for this connection).
            //
            // raddr is forced to the wildcard address, NOT preserved: a
            // side-by-side capture against genuine unmodified Chrome on the
            // same connection (same mDNS-hidden mode, evidenced by the
            // .local-masked host candidates alongside it) showed its own
            // real srflx candidates ALWAYS carry raddr=0.0.0.0 (or "::" for
            // IPv6) — never the real local LAN address. Preserving the
            // genuine raddr (this branch's previous behavior) leaked the
            // real private IP (e.g. 192.168.1.48) in plain text.
            //
            // The wildcard's own family must match THIS candidate's
            // original address family, not |is_ipv6| (which reflects the
            // proxy_ip's family): a machine with both an IPv4 and an IPv6
            // interface produces one srflx of each family, and proxy_ip
            // here is IPv4-only — using |is_ipv6| would wrongly force "::"
            // (or "0.0.0.0") based on the proxy instead of this specific
            // candidate.
            bool candidate_was_ipv6 = parts[4].find(':') != std::string::npos;
            parts[4] = proxy_ip;
            rebuild_candidate_attrs(parts, /*has_raddr=*/true,
                                     candidate_was_ipv6 ? "::" : "0.0.0.0",
                                     current_ufrag, /*force_raddr=*/true);
          } else if (parts[4] != proxy_ip) {
            // Determine if this is the first srflx candidate
            bool is_first_srflx = (*srflx_count == 0);
            (*srflx_count)++;

            if (is_first_srflx) {
              std::string cand1, cand2;
              BuildFakeSrflxPair(parts, proxy_ip, is_ipv6,
                                 /*priority_type_delta=*/0, &cand1, &cand2);
              if (extra_candidates) {
                line = cand1;
                extra_candidates->push_back(cand2);
              } else {
                line = cand1 + (has_cr ? "\r\n" : "\n") + cand2;
              }
              line_fully_built = true;
            } else {
              // Not the first one. To ensure a stable fingerprint of exactly 2 srflx candidates
              // (one 0.0.0.0 and one ProxyIP) regardless of how many network interfaces the machine has,
              // we DROP all subsequent native srflx candidates.
              drop_line = true;
            }
          } else {
            // parts[4] == proxy_ip already: either a genuinely successful
            // SOCKS5 tunnel, or (forward mode, non-SOCKS5 proxy) a candidate
            // MaybeFabricateForwardModeCandidates previously injected into
            // the native PeerConnection (see its comment) — which makes it
            // show up here as a "real" candidate on every subsequent read of
            // localDescription.sdp. Re-add any of ufrag/raddr/rport the
            // native round-trip (see current_ufrag's comment) may have
            // dropped — raddr defaults to 0.0.0.0 for the first occurrence
            // and proxy_ip for the second, matching BuildFakeSrflxPair's own
            // pairing scheme.
            //
            // Capped at exactly 2 PER TRANSPORT (m= section for full-sdp,
            // sdp_mline_index for trickle), not once for the whole
            // connection: a connection whose m=audio/m=video/application
            // each gather independent host candidates (not unified under
            // one BUNDLEd ICE transport) genuinely produces its OWN real
            // srflx pair per section when the SOCKS5 UDP tunnel works
            // end-to-end. A single connection-wide cap spends the budget on
            // the first section's pair and DROPS every later section's
            // genuine srflx outright (the "m=video/application missing
            // srflx" symptom), while still needing a cap at all so a
            // multi-homed machine's extra real interfaces within the SAME
            // section don't grow unbounded (the "dư srflx" / excess srflx
            // symptom).
            int* native_count = nullptr;
            unsigned* first_priority = nullptr;
            if (extra_candidates) {
              if (trickle_native_srflx_count) {
                auto add_result =
                    trickle_native_srflx_count->insert(sdp_mline_index + 1, 0);
                native_count = &add_result.stored_value->value;
              }
              if (trickle_native_srflx_first_priority) {
                auto prio_result = trickle_native_srflx_first_priority->insert(
                    sdp_mline_index + 1, 0);
                first_priority = &prio_result.stored_value->value;
              }
            } else {
              size_t section_idx = host_insert_positions.empty()
                                        ? 0
                                        : host_insert_positions.size() - 1;
              if (section_idx < section_native_srflx_count.size()) {
                native_count = &section_native_srflx_count[section_idx];
              }
              if (section_idx < section_native_srflx_first_priority.size()) {
                first_priority = &section_native_srflx_first_priority[section_idx];
              }
            }
            if (native_count) {
              VLOG(1) << "SanitizeSdp: native srflx per-transport cap, "
                         "call_site="
                      << (extra_candidates ? "trickle" : "full-sdp")
                      << " sdp_mline_index=" << sdp_mline_index
                      << " section_idx="
                      << (extra_candidates
                              ? -1
                              : static_cast<int>(
                                    host_insert_positions.empty()
                                        ? 0
                                        : host_insert_positions.size() - 1))
                      << " native_count_before=" << *native_count
                      << " will_drop=" << (*native_count >= 2);
              if (*native_count >= 2) {
                drop_line = true;
              } else {
                // raddr alternation is intentionally GLOBAL
                // (*srflx_count), not per-transport (*native_count): genuine
                // multi-homed Chrome shows raddr=0.0.0.0 for only the very
                // FIRST srflx in the whole connection and the real local
                // address for every other one. Using the per-transport
                // counter here made EVERY transport's own first srflx show
                // 0.0.0.0 (since each transport's native_count independently
                // starts at 0), so a multi-transport connection showed
                // raddr=0.0.0.0 on every single srflx instead of just one.
                bool is_first_seen = (*srflx_count == 0);
                rebuild_candidate_attrs(
                    parts, /*has_raddr=*/true,
                    is_first_seen ? (is_ipv6 ? "::" : "0.0.0.0") : proxy_ip,
                    current_ufrag, /*force_raddr=*/true);
                // Priority alternation, unlike raddr, IS per-transport
                // (*native_count): the two real srflx candidates being
                // disambiguated here both belong to the SAME transport
                // (e.g. both gathered for m=audio), so they should follow
                // the same fixed +2560 pairing genuine Chrome uses between
                // a transport's own two interfaces — NOT the connection-
                // wide *srflx_count, which would compare across unrelated
                // transports. NoEgressUdpSocket's synthetic STUN responder
                // currently gives every native srflx on a transport the
                // SAME priority (a native-pipeline bug — see
                // trickle_native_srflx_first_priority_'s declaration),
                // so this rewrites the second one explicitly.
                if (first_priority) {
                  unsigned cur_prio = 0;
                  if (*native_count == 0) {
                    if (base::StringToUint(parts[3], &cur_prio)) {
                      *first_priority = cur_prio;
                    }
                  } else if (*first_priority != 0) {
                    parts[3] =
                        base::NumberToString(*first_priority + 2560u);
                  }
                }
              }
              (*native_count)++;
            } else {
              // No per-transport scope available (shouldn't normally
              // happen) — fall back to the old connection-wide cap rather
              // than passing the candidate through unbounded.
              if (*srflx_count >= 2) {
                drop_line = true;
              } else {
                bool is_first_seen = (*srflx_count == 0);
                rebuild_candidate_attrs(
                    parts, /*has_raddr=*/true,
                    is_first_seen ? (is_ipv6 ? "::" : "0.0.0.0") : proxy_ip,
                    current_ufrag, /*force_raddr=*/true);
              }
            }
            (*srflx_count)++;
          }
        } else if (typ == "host") {
          // Safety net for when Chromium's native mDNS hiding didn't run
          // (see IsMdnsHostname's comment) — without this, parts[4] (and
          // thus the final line, rebuilt from parts below) would carry the
          // real local IP/IPv6 literal straight through untouched, since
          // nothing else in this branch ever inspects or rewrites it.
          if (!IsMdnsHostname(parts[4])) {
            parts[4] = MaskRealHostAddress(parts[4]);
          }
          VLOG(1) << "SanitizeSdp: host line seen, call_site="
                  << (extra_candidates ? "trickle(OnIceCandidate)"
                                       : "full-sdp(CreateWebKitSessionDescription)")
                  << " sdp_mline_index=" << sdp_mline_index
                  << " trickle_set_ptr=" << trickle_mline_srflx_done
                  << " trickle_set_size_before="
                  << (trickle_mline_srflx_done
                          ? static_cast<int>(trickle_mline_srflx_done->size())
                          : -1)
                  << " line='" << line << "'";
          // Decide whether THIS host's own transport (m= section for
          // full-sdp, sdp_mline_index for trickle) still needs a derived
          // srflx pair — gated per-transport rather than once globally, so
          // a connection whose m=audio/m=video/m=application each gather
          // independent host candidates (not unified under one BUNDLEd ICE
          // transport) gets a pair for EVERY one of them, not just the
          // first ever seen. This covers the case a static proxy-type check
          // can't: a *SOCKS5* proxy whose UDP ASSOCIATE failed at runtime
          // (REP=0x7) and fell back to NoEgressUdpSocket — it still binds a
          // genuine host socket, but no srflx will ever be gathered (sends
          // are blocked), and fabricate_srflx_from_host is statically false
          // for it (the proxy scheme itself is valid SOCKS5).
          bool derive_for_this_host = false;
          size_t section_idx = 0;
          // Only fabricate from host text when there is NO real UDP socket
          // at all (HTTP/SOCKS4 forward mode, force_no_udp_egress) — i.e.
          // |fabricate_srflx_from_host| is true. For SOCKS5, a real UDP
          // socket always exists; when its ASSOCIATE fails at runtime, it
          // falls back to NoEgressUdpSocket, whose own SendTo() already
          // replies with a synthetic STUN Binding Response (see that
          // class), making libwebrtc itself genuinely gather a native
          // srflx candidate (addr already == proxy_ip) through the normal
          // pipeline — no text-level fabrication needed or wanted here.
          // Deriving from host text ANYWAY (the old, unconditional
          // behavior) raced against that native srflx: the host-derived
          // fake pair got pushed to the page immediately on host arrival,
          // then the genuinely-native one arrived moments later and (after
          // the per-transport cap fix above) was no longer suppressed
          // either — yielding 4 srflx per transport (2 fake + 2 real)
          // instead of 2. Confirmed via VLOG: ufrag r0cv's mline 0 shows
          // "derived srflx from host" (foundations 2082703547/1890134511)
          // followed by a separate "srflx line seen ... addr_eq_proxy=1"
          // (foundations 3098775684/3399576695) for the SAME host port.
          if (fabricate_srflx_from_host) {
            if (extra_candidates) {
              // Trickle: one specific m-line's host just arrived. +1
              // because WTF::HashSet<int>'s default HashTraits uses 0 as
              // the internal "empty slot" sentinel — storing the literal
              // value 0 (the common case: most connections have exactly
              // one, audio-only, m-line) is unreliable, making insert(0)
              // spuriously report "new entry" every time instead of only
              // the first. Confirmed via VLOG: trickle_set_size_before=1
              // yet insert(0) still returned is_new_entry=true on the
              // connection's SECOND host.
              if (trickle_mline_srflx_done &&
                  trickle_mline_srflx_done->insert(sdp_mline_index + 1)
                      .is_new_entry) {
                derive_for_this_host = true;
              }
            } else if (!host_insert_positions.empty()) {
              section_idx = host_insert_positions.size() - 1;
              bool this_section_has_real_srflx =
                  host_insert_positions.size() <
                      section_has_real_srflx.size() &&
                  section_has_real_srflx[host_insert_positions.size()];
              if (!this_section_has_real_srflx &&
                  section_pending_pairs[section_idx].first.empty()) {
                derive_for_this_host = true;
              }
            }
          }

          if (derive_for_this_host) {
            // Derive a believable srflx pair from this real host line so
            // the SDP isn't host-only, the same way BuildFakeSrflxPair
            // already does for the fully-synthetic forward-mode case below.
            rebuild_candidate_attrs(parts, /*has_raddr=*/false, "",
                                     current_ufrag);
            // cand1/cand2 already carry whatever prefix parts[0] had (e.g.
            // "a=" if the original line had it), inherited via
            // BuildFakeSrflxPair's seed_parts copy — do not prepend "a="
            // again when using them (that's what produced the
            // "a=a=candidate:" bug).
            std::string cand1, cand2;
            BuildFakeSrflxPair(parts, proxy_ip, is_ipv6,
                               /*priority_type_delta=*/26, &cand1, &cand2);
            if (extra_candidates) {
              extra_candidates->push_back(cand1);
              extra_candidates->push_back(cand2);
            } else {
              // Don't insert inline here — stash for insertion after this
              // SECTION's LAST host line once the loop finishes (see
              // host_insert_positions' declaration for why).
              section_pending_pairs[section_idx] = {cand1, cand2};
            }
            // Reconstruct this host line itself, normalized, in place.
            line = parts[0];
            for (size_t i = 1; i < parts.size(); ++i) {
              line += " " + parts[i];
            }
            line_fully_built = true;
            VLOG(1) << "SanitizeSdp: derived srflx from host, cand1='"
                    << cand1 << "' cand2='" << cand2 << "'";
          } else {
            // Already-complete candidate set (srflx exists elsewhere for
            // this transport, or this isn't the first host line of it) —
            // re-add ufrag/network-cost if the native round-trip (see
            // current_ufrag's comment) dropped them from this host line.
            rebuild_candidate_attrs(parts, /*has_raddr=*/false, "",
                                     current_ufrag);
          }
          // Still meaningful for the fully-synthetic fallback check below
          // (fabricate_srflx_from_host): that path should only fire when
          // there is truly no host candidate anywhere, regardless of which
          // per-transport derive decision was made above.
          host_already_present = true;
        } else if (typ == "relay") {
          // Only rewrite if raddr doesn't already point at the proxy IP.
          // raddr here reflects the srflx address used to reach the TURN
          // server; if it's already proxy_ip the tunnel worked correctly and
          // this is a genuine no-op. If it differs (native-fallback path
          // leaked the real IP), spoof it the same way srflx is spoofed above.
          auto raddr_it = std::find(parts.begin(), parts.end(), "raddr");
          bool raddr_already_correct = raddr_it != parts.end() &&
                                        (raddr_it + 1) != parts.end() &&
                                        *(raddr_it + 1) == proxy_ip;
          if (!raddr_already_correct) {
            // We are rewriting this candidate in place (not duplicating it
            // like srflx above), so its foundation is untouched: a relay
            // candidate's foundation only depends on type/base
            // address/protocol/relay_protocol, none of which we change here.
            // (An earlier revision appended a digit to the foundation
            // string, which is the same invalid-range bug already fixed for
            // srflx above — a real CRC32-derived foundation never exceeds 10
            // decimal digits, so blindly appending one can produce a value
            // no genuine Chrome foundation could ever take.)

            // DO NOT touch parts[4] because it is the TURN server's IP.
            // BUT the raddr of a relay candidate leaks the STUN/srflx IP (which is the real IP).
            // Since we spoofed the srflx IP to be proxy_ip, we MUST spoof the relay raddr to be proxy_ip!
            if (raddr_it != parts.end() && (raddr_it + 1) != parts.end()) {
              *(raddr_it + 1) = proxy_ip;
            } else {
              parts.push_back("raddr");
              parts.push_back(proxy_ip);
            }
            // We leave rport as is, because it points to the srflx port, which perfectly mimics Chrome!
          }
        }
        
        // Reconstruct the modified line only if we didn't override `line`
        // completely (srflx duplication / host fabrication, both set
        // line_fully_built) and we are not dropping it.
        if (!drop_line && !line_fully_built) {
          line = parts[0];
          for (size_t i = 1; i < parts.size(); ++i) {
            line += " " + parts[i];
          }
        }
      }
    }

    if (!drop_line) {
      result += line;
      if (has_cr) result += "\r";
      if (end != std::string::npos) result += "\n";
      if (is_host_line && !host_insert_positions.empty()) {
        host_insert_positions.back() = result.size();
      }
    }

    if (end == std::string::npos) break;
    start = end + 1;
  }

  // Insert each section's own derived srflx pair (if any — see the host
  // branch above) right after THAT section's LAST host line, matching
  // genuine Chrome's host1/host2/srflx1/srflx2 grouping instead of
  // interleaving. Each m= section gets its own independently-seeded pair
  // (not a single shared one copied everywhere), since m=audio/m=video/
  // m=application can each gather their own independent host candidates.
  // Trickle already pushed its (single, per-mline) pair directly into
  // |extra_candidates| inside the loop above, so there's nothing left to do
  // here for that call site.
  if (!extra_candidates) {
    // Insert from the last position backward so earlier offsets in
    // |host_insert_positions| stay valid as |result| grows.
    std::vector<size_t> indices(host_insert_positions.size());
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
      return host_insert_positions[a] < host_insert_positions[b];
    });
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
      size_t i = *it;
      if (host_insert_positions[i] == std::string::npos ||
          section_pending_pairs[i].first.empty()) {
        continue;
      }
      std::string insertion = "a=" + section_pending_pairs[i].first + "\r\n" +
                               "a=" + section_pending_pairs[i].second +
                               "\r\n";
      result.insert(host_insert_positions[i], insertion);
    }
  }

  if (fabricate_srflx_from_host && *srflx_count == 0 && !host_already_present) {
    // No real candidate (host or srflx) was found anywhere in |sdp| — see
    // FabricateHostAndSrflxTriplet's comment for why. Synthesize one so the
    // SDP doesn't look like a connection with zero local candidates.
    std::string ufrag = ExtractIceUfrag(sdp_str);
    VLOG(1) << "SanitizeSdp: fabrication trigger, call_site="
            << (extra_candidates ? "trickle(OnIceCandidate)"
                                  : "full-sdp(CreateWebKitSessionDescription)")
            << " ufrag='" << ufrag << "' sdp_str.length()=" << sdp_str.length()
            << " shared_srflx_count_ptr="
            << (shared_srflx_count ? "non-null(persistent)" : "null(local-fresh)");
    if (!ufrag.empty()) {
      std::string host_line, host_line2, srflx1, srflx2;
      FabricateHostAndSrflxTriplet(ufrag, proxy_ip, is_ipv6, &host_line,
                                    &host_line2, &srflx1, &srflx2);
      VLOG(1) << "SanitizeSdp: fabricated host_line='" << host_line << "'";
      (*srflx_count) = 1;
      if (extra_candidates) {
        extra_candidates->push_back(host_line);
        extra_candidates->push_back(host_line2);
        extra_candidates->push_back(srflx1);
        extra_candidates->push_back(srflx2);
      } else {
        // Insert right after the first m= section's a=ice-ufrag line, which
        // is where real candidate lines for that section would start
        // appearing.
        size_t insert_pos = result.find("a=ice-ufrag:");
        if (insert_pos != std::string::npos) {
          insert_pos = result.find('\n', insert_pos);
        }
        if (insert_pos != std::string::npos) {
          insert_pos += 1;
          std::string insertion = "a=" + host_line + "\r\n" + "a=" +
                                   host_line2 + "\r\n" + "a=" + srflx1 +
                                   "\r\n" + "a=" + srflx2 + "\r\n";
          result.insert(insert_pos, insertion);
        }
      }
    }
  }

  return String::FromUTF8(result);
}

RTCSessionDescriptionPlatform* CreateWebKitSessionDescription(
    const std::string& sdp,
    const std::string& type) {
  return MakeGarbageCollected<RTCSessionDescriptionPlatform>(
      String::FromUTF8(type), String::FromUTF8(sdp));
}

RTCSessionDescriptionPlatform* CreateWebKitSessionDescription(
    const webrtc::SessionDescriptionInterface* native_desc) {
  if (!native_desc) {
    LOG(ERROR) << "Native session description is null.";
    return nullptr;
  }

  std::string sdp;
  if (!native_desc->ToString(&sdp)) {
    LOG(ERROR) << "Failed to get SDP string of native session description.";
    return nullptr;
  }

  String sdp_str = String::FromUTF8(sdp);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  std::string proxy_ip = GetWebRtcSdpProxyIp(command_line);
  int real_candidate_count = 0;
  for (size_t pos = sdp.find("a=candidate:"); pos != std::string::npos;
       pos = sdp.find("a=candidate:", pos + 1)) {
    ++real_candidate_count;
  }
  VLOG(1) << "CreateWebKitSessionDescription(native_desc): type="
          << native_desc->type() << " raw_sdp.length()=" << sdp.length()
          << " real_candidate_lines_in_native_sdp=" << real_candidate_count
          << " proxy_ip='" << proxy_ip << "'";
  if (!proxy_ip.empty()) {
    sdp_str = SanitizeSdp(sdp_str, proxy_ip, /*shared_srflx_count=*/nullptr,
                          /*extra_candidates=*/nullptr,
                          IsForwardModeUsingNonSocks5Proxy(command_line),
                          /*trickle_mline_srflx_done=*/nullptr,
                          /*sdp_mline_index=*/0,
                          /*trickle_native_srflx_count=*/nullptr,
                          /*trickle_native_srflx_first_priority=*/nullptr,
                          IsReplaceMode(command_line));
  }

  return CreateWebKitSessionDescription(sdp_str.Utf8(), native_desc->type());
}

void RunClosureWithTrace(CrossThreadOnceClosure closure,
                         const char* trace_event_name) {
  TRACE_EVENT0("webrtc", trace_event_name);
  std::move(closure).Run();
}

void RunSynchronousOnceClosure(base::OnceClosure closure,
                               const char* trace_event_name,
                               base::WaitableEvent* event) {
  {
    TRACE_EVENT0("webrtc", trace_event_name);
    std::move(closure).Run();
  }
  event->Signal();
}

// Converter functions from Blink types to WebRTC types.

// Class mapping responses from calls to libjingle CreateOffer/Answer and
// the blink::RTCSessionDescriptionRequest.
class CreateSessionDescriptionRequest
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateSessionDescriptionRequest(
      const scoped_refptr<base::SingleThreadTaskRunner>& main_thread,
      blink::RTCSessionDescriptionRequest* request,
      const base::WeakPtr<RTCPeerConnectionHandler>& handler,
      PeerConnectionTracker* tracker,
      PeerConnectionTracker::Action action)
      : main_thread_(main_thread),
        webkit_request_(request),
        handler_(handler),
        tracker_(tracker),
        action_(action) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    // Explicitly take ownership of desc - as documented in the webrtc lib
    // comment.
    OnSuccessUniquePtr(base::WrapUnique(desc));
  }

  void OnSuccessUniquePtr(
      std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &CreateSessionDescriptionRequest::OnSuccessUniquePtr,
              webrtc::scoped_refptr<CreateSessionDescriptionRequest>(this),
              std::move(desc)));
      return;
    }

    auto tracker = tracker_.Lock();
    if (tracker && handler_) {
      StringBuilder result;
      if (desc) {
        std::string value;
        desc->ToString(&value);
        auto json = std::make_unique<JSONObject>();
        json->SetString("type", String::FromUTF8(desc->type()));
        if (!value.empty()) {
          json->SetString("sdp", String::FromUTF8(value));
        }
        json->WriteJSON(&result);
      }
      tracker->TrackSessionDescriptionCallback(handler_.get(), action_,
                                               "OnSuccess", result.ToString());
      tracker->TrackSessionId(handler_.get(),
                              String::FromUTF8(desc->session_id()));
    }
    webkit_request_->RequestSucceeded(
        CreateWebKitSessionDescription(desc.get()));
    webkit_request_ = nullptr;
  }
  void OnFailure(webrtc::RTCError error) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &CreateSessionDescriptionRequest::OnFailure,
              webrtc::scoped_refptr<CreateSessionDescriptionRequest>(this),
              std::move(error)));
      return;
    }

    auto tracker = tracker_.Lock();
    if (handler_ && tracker) {
      tracker->TrackSessionDescriptionCallback(
          handler_.get(), action_, "OnFailure",
          String::FromUTF8(error.message()));
    }
    // TODO(hta): Convert CreateSessionDescriptionRequest.OnFailure
    webkit_request_->RequestFailed(error);
    webkit_request_ = nullptr;
  }

 protected:
  ~CreateSessionDescriptionRequest() override {
    // This object is reference counted and its callback methods |OnSuccess| and
    // |OnFailure| will be invoked on libjingle's signaling thread and posted to
    // the main thread. Since the main thread may complete before the signaling
    // thread has deferenced this object there is no guarantee that this object
    // is destructed on the main thread.
    DLOG_IF(ERROR, webkit_request_)
        << "CreateSessionDescriptionRequest not completed. Shutting down?";
  }

  const scoped_refptr<base::SingleThreadTaskRunner> main_thread_;
  Persistent<RTCSessionDescriptionRequest> webkit_request_;
  const base::WeakPtr<RTCPeerConnectionHandler> handler_;
  const CrossThreadWeakPersistent<PeerConnectionTracker> tracker_;
  PeerConnectionTracker::Action action_;
};

using RTCStatsReportCallbackInternal =
    CrossThreadOnceFunction<void(std::unique_ptr<RTCStatsReportPlatform>)>;

void GetRTCStatsOnSignalingThread(
    const scoped_refptr<base::SingleThreadTaskRunner>& main_thread,
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface>
        native_peer_connection,
    RTCStatsReportCallbackInternal callback) {
  TRACE_EVENT0("webrtc", "GetRTCStatsOnSignalingThread");
  native_peer_connection->GetStats(
      CreateRTCStatsCollectorCallback(
          main_thread, ConvertToBaseOnceCallback(std::move(callback)))
          .get());
}

std::set<RTCPeerConnectionHandler*>* GetPeerConnectionHandlers() {
  static std::set<RTCPeerConnectionHandler*>* handlers =
      new std::set<RTCPeerConnectionHandler*>();
  return handlers;
}

// Counts the number of senders that have |stream_id| as an associated stream.
size_t GetLocalStreamUsageCount(
    const Vector<std::unique_ptr<blink::RTCRtpSenderImpl>>& rtp_senders,
    const std::string& stream_id) {
  size_t usage_count = 0;
  for (const auto& sender : rtp_senders) {
    for (const auto& sender_stream_id : sender->state().stream_ids()) {
      if (sender_stream_id == stream_id) {
        ++usage_count;
        break;
      }
    }
  }
  return usage_count;
}

MediaStreamTrackMetrics::Kind MediaStreamTrackMetricsKind(
    const MediaStreamComponent* component) {
  return component->GetSourceType() == MediaStreamSource::kTypeAudio
             ? MediaStreamTrackMetrics::Kind::kAudio
             : MediaStreamTrackMetrics::Kind::kVideo;
}

}  // namespace

// Implementation of ParsedSessionDescription
ParsedSessionDescription::ParsedSessionDescription(const String& sdp_type,
                                                   const String& sdp)
    : type_(sdp_type), sdp_(sdp) {}

// static
ParsedSessionDescription ParsedSessionDescription::Parse(
    const RTCSessionDescriptionInit* session_description_init) {
  ParsedSessionDescription temp(
      session_description_init->hasType()
          ? session_description_init->type().AsString()
          : String(),
      session_description_init->sdp());
  temp.DoParse();
  return temp;
}

// static
ParsedSessionDescription ParsedSessionDescription::Parse(
    const RTCSessionDescriptionPlatform* session_description_platform) {
  ParsedSessionDescription temp(session_description_platform->GetType(),
                                session_description_platform->Sdp());
  temp.DoParse();
  return temp;
}

// static
ParsedSessionDescription ParsedSessionDescription::Parse(const String& sdp_type,
                                                         const String& sdp) {
  ParsedSessionDescription temp(sdp_type, sdp);
  temp.DoParse();
  return temp;
}

void ParsedSessionDescription::DoParse() {
  std::optional<webrtc::SdpType> maybe_type =
      webrtc::SdpTypeFromString(type_.Utf8().c_str());
  if (!maybe_type.has_value()) {
    description_.reset();
    return;
  }
  description_ = webrtc::CreateSessionDescription(*maybe_type,
                                                  sdp_.Utf8().c_str(), &error_);
}

// Processes the resulting state changes of a SetLocalDescription() or
// SetRemoteDescription() call.
class RTCPeerConnectionHandler::WebRtcSetDescriptionObserverImpl
    : public WebRtcSetDescriptionObserver {
 public:
  WebRtcSetDescriptionObserverImpl(
      base::WeakPtr<RTCPeerConnectionHandler> handler,
      blink::RTCVoidRequest* web_request,
      PeerConnectionTracker* tracker,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner,
      PeerConnectionTracker::Action action,
      bool is_rollback)
      : handler_(handler),
        main_thread_(task_runner),
        web_request_(web_request),
        tracker_(tracker),
        action_(action),
        is_rollback_(is_rollback) {}

  void OnSetDescriptionComplete(
      webrtc::RTCError error,
      WebRtcSetDescriptionObserver::States states) override {
    auto tracker = tracker_.Lock();
    if (!error.ok()) {
      if (tracker && handler_) {
        tracker->TrackSessionDescriptionCallback(
            handler_.get(), action_, "OnFailure",
            String::FromUTF8(error.message()));
      }
      web_request_->RequestFailed(error);
      web_request_ = nullptr;
      return;
    }

    // Copy/move some of the states to be able to use them after moving
    // |state| below.
    webrtc::PeerConnectionInterface::SignalingState signaling_state =
        states.signaling_state;
    auto pending_local_description =
        std::move(states.pending_local_description);
    auto current_local_description =
        std::move(states.current_local_description);
    auto pending_remote_description =
        std::move(states.pending_remote_description);
    auto current_remote_description =
        std::move(states.current_remote_description);

    // Track result in chrome://webrtc-internals/.
    if (tracker && handler_) {
      StringBuilder value;
      if (action_ ==
          PeerConnectionTracker::kActionSetLocalDescriptionImplicit) {
        webrtc::SessionDescriptionInterface* created_session_description =
            nullptr;
        // Deduce which SDP was created based on signaling state.
        if (signaling_state ==
                webrtc::PeerConnectionInterface::kHaveLocalOffer &&
            pending_local_description) {
          created_session_description = pending_local_description.get();
        } else if (signaling_state ==
                       webrtc::PeerConnectionInterface::kStable &&
                   current_local_description) {
          created_session_description = current_local_description.get();
        }
        RTC_DCHECK(created_session_description);
        std::string sdp;
        created_session_description->ToString(&sdp);
        value.Append("type: ");
        value.Append(
            webrtc::SdpTypeToString(created_session_description->GetType()));
        value.Append(", sdp: ");
        value.Append(sdp.c_str());
      }
      tracker->TrackSessionDescriptionCallback(handler_.get(), action_,
                                               "OnSuccess", value.ToString());
      handler_->TrackSignalingChange(signaling_state);
    }

    if (handler_) {
      handler_->OnSessionDescriptionsUpdated(
          std::move(pending_local_description),
          std::move(current_local_description),
          std::move(pending_remote_description),
          std::move(current_remote_description));
    }

    // This fires JS events and could cause |handler_| to become null.
    ProcessStateChanges(std::move(states));
    ResolvePromise();
  }

 private:
  ~WebRtcSetDescriptionObserverImpl() override {}

  void ResolvePromise() {
    web_request_->RequestSucceeded();
    web_request_ = nullptr;
  }

  void ProcessStateChanges(WebRtcSetDescriptionObserver::States states) {
    if (handler_) {
      handler_->OnModifySctpTransport(std::move(states.sctp_transport_state));
    }
    // Since OnSessionDescriptionsUpdated can fire events, it may cause
    // garbage collection. Ensure that handler_ is still valid.
    if (handler_ && !handler_->is_unregistered_) {
      handler_->OnModifyTransceivers(
          states.signaling_state, std::move(states.transceiver_states),
          action_ == PeerConnectionTracker::kActionSetRemoteDescription,
          is_rollback_);
    }
  }

  base::WeakPtr<RTCPeerConnectionHandler> handler_;
  scoped_refptr<base::SequencedTaskRunner> main_thread_;
  Persistent<blink::RTCVoidRequest> web_request_;
  CrossThreadWeakPersistent<PeerConnectionTracker> tracker_;
  PeerConnectionTracker::Action action_;
  bool is_rollback_;
};

// Generally, output from a PeerConnection will go through the
// `RTCPeerConnectionHandler::Observer`, which is a class declared in the
// protected section of `RTCPeerConnectionHandler`. For this reason a plain
// `Observer` can not be referenced by other classes, which is necessary since
// an instance of the `DataChannelEventObserverInterface` interface needs to be
// injected into the PeerConnection.
class RtcDataChannelEventSink : public GarbageCollectedMixin {
 public:
  virtual ~RtcDataChannelEventSink() = default;

  virtual void OnWebRtcDataChannelLogWrite(const Vector<uint8_t>& output) = 0;
};

// Receives notifications from a PeerConnection object about state changes. The
// callbacks we receive here come on the webrtc signaling thread, so this class
// takes care of delivering them to an RTCPeerConnectionHandler instance on the
// main thread. In order to do safe PostTask-ing, the class is reference counted
// and checks for the existence of the RTCPeerConnectionHandler instance before
// delivering callbacks on the main thread.
class RTCPeerConnectionHandler::Observer
    : public GarbageCollected<RTCPeerConnectionHandler::Observer>,
      public PeerConnectionObserver,
      public RtcEventLogOutputSink,
      public RtcDataChannelEventSink {
 public:
  Observer(const base::WeakPtr<RTCPeerConnectionHandler>& handler,
           scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : handler_(handler), main_thread_(task_runner) {}
  ~Observer() override {
    // `signaling_thread_` may be null in some testing-only environments.
    if (!signaling_thread_) {
      return;
    }
    // To avoid a PROXY block-invoke to ~webrtc::PeerConnection in the event
    // that `native_peer_connection_` was the last reference, we move it to the
    // signaling thread in a PostTask.
    signaling_thread_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
              // The binding releases `pc` on the signaling thread as
              // this method goes out of scope.
            },
            std::move(native_peer_connection_)));
  }

  void Initialize(
      scoped_refptr<base::SingleThreadTaskRunner> signaling_thread) {
    DCHECK(main_thread_->BelongsToCurrentThread());
    DCHECK(!native_peer_connection_);
    DCHECK(handler_);
    native_peer_connection_ = handler_->native_peer_connection_;
    DCHECK(native_peer_connection_);
    signaling_thread_ = std::move(signaling_thread);
  }

  // When an RTC event log is sent back from PeerConnection, it arrives here.
  void OnWebRtcEventLogWrite(const Vector<uint8_t>& output) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &RTCPeerConnectionHandler::Observer::OnWebRtcEventLogWrite,
              WrapCrossThreadPersistent(this), output));
    } else if (handler_) {
      handler_->OnWebRtcEventLogWrite(output);
    }
  }

  void OnWebRtcDataChannelLogWrite(const Vector<uint8_t>& output) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &RTCPeerConnectionHandler::Observer::OnWebRtcDataChannelLogWrite,
              WrapCrossThreadPersistent(this), output));
    } else if (handler_) {
      handler_->OnWebRtcDataChannelLogWrite(output);
    }
  }

  void Trace(Visitor* visitor) const override {}

 protected:
  // TODO(hbos): Remove once no longer mandatory to implement.
  void OnSignalingChange(PeerConnectionInterface::SignalingState) override {}
  void OnAddStream(webrtc::scoped_refptr<MediaStreamInterface>) override {}
  void OnRemoveStream(webrtc::scoped_refptr<MediaStreamInterface>) override {}

  void OnDataChannel(
      webrtc::scoped_refptr<DataChannelInterface> data_channel) override {
    PostCrossThreadTask(
        *main_thread_.get(), FROM_HERE,
        CrossThreadBindOnce(
            &RTCPeerConnectionHandler::Observer::OnDataChannelImpl,
            WrapCrossThreadPersistent(this), data_channel));
  }

  void OnNegotiationNeededEvent(uint32_t event_id) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &RTCPeerConnectionHandler::Observer::OnNegotiationNeededEvent,
              WrapCrossThreadPersistent(this), event_id));
    } else if (handler_) {
      handler_->OnNegotiationNeededEvent(event_id);
    }
  }

  void OnIceConnectionChange(
      PeerConnectionInterface::IceConnectionState new_state) override {}
  void OnStandardizedIceConnectionChange(
      PeerConnectionInterface::IceConnectionState new_state) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(&RTCPeerConnectionHandler::Observer::
                                  OnStandardizedIceConnectionChange,
                              WrapCrossThreadPersistent(this), new_state));
    } else if (handler_) {
      handler_->OnIceConnectionChange(new_state);
    }
  }

  void OnConnectionChange(
      PeerConnectionInterface::PeerConnectionState new_state) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &RTCPeerConnectionHandler::Observer::OnConnectionChange,
              WrapCrossThreadPersistent(this), new_state));
    } else if (handler_) {
      handler_->OnConnectionChange(new_state);
    }
  }

  void OnIceGatheringChange(
      PeerConnectionInterface::IceGatheringState new_state) override {
    if (!main_thread_->BelongsToCurrentThread()) {
      PostCrossThreadTask(
          *main_thread_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &RTCPeerConnectionHandler::Observer::OnIceGatheringChange,
              WrapCrossThreadPersistent(this), new_state));
    } else if (handler_) {
      handler_->OnIceGatheringChange(new_state);
    }
  }

  void OnIceCandidate(const IceCandidate* candidate) override {
    DCHECK(native_peer_connection_);
    std::string sdp = candidate->ToString();
   DCHECK(!sdp.empty());
    // The generated candidate may have been added to the pending or current
    // local description, take a snapshot and surface them to the main thread.
    // Remote descriptions are also surfaced because
    // OnSessionDescriptionsUpdated() requires all four as arguments.
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        pending_local_description = CopySessionDescription(
            native_peer_connection_->pending_local_description());
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        current_local_description = CopySessionDescription(
            native_peer_connection_->current_local_description());
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        pending_remote_description = CopySessionDescription(
            native_peer_connection_->pending_remote_description());
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        current_remote_description = CopySessionDescription(
            native_peer_connection_->current_remote_description());

    PostCrossThreadTask(
        *main_thread_.get(), FROM_HERE,
        CrossThreadBindOnce(
            &RTCPeerConnectionHandler::Observer::OnIceCandidateImpl,
            WrapCrossThreadPersistent(this), String::FromUTF8(sdp),
            String::FromUTF8(candidate->sdp_mid()),
            candidate->sdp_mline_index(), candidate->candidate().component(),
            candidate->candidate().address().family(),
            String::FromUTF8(candidate->candidate().username()),
            String::FromUTF8(candidate->server_url()),
            std::move(pending_local_description),
            std::move(current_local_description),
            std::move(pending_remote_description),
            std::move(current_remote_description)));
  }

  void OnIceCandidateError(const std::string& address,
                           int port,
                           const std::string& url,
                           int error_code,
                           const std::string& error_text) override {
    PostCrossThreadTask(
        *main_thread_.get(), FROM_HERE,
        CrossThreadBindOnce(
            &RTCPeerConnectionHandler::Observer::OnIceCandidateErrorImpl,
            WrapCrossThreadPersistent(this),
            port ? String::FromUTF8(address) : String(),
            static_cast<uint16_t>(port),
            String::Format("%s:%d", address.c_str(), port),
            String::FromUTF8(url), error_code, String::FromUTF8(error_text)));
  }

  void OnDataChannelImpl(webrtc::scoped_refptr<DataChannelInterface> channel) {
    DCHECK(main_thread_->BelongsToCurrentThread());
    if (handler_)
      handler_->OnDataChannel(channel);
  }

  void OnIceCandidateImpl(const String& sdp,
                          const String& sdp_mid,
                          int sdp_mline_index,
                          int component,
                          int address_family,
                          const String& username_fragment,
                          const String& url,
                          std::unique_ptr<webrtc::SessionDescriptionInterface>
                              pending_local_description,
                          std::unique_ptr<webrtc::SessionDescriptionInterface>
                              current_local_description,
                          std::unique_ptr<webrtc::SessionDescriptionInterface>
                              pending_remote_description,
                          std::unique_ptr<webrtc::SessionDescriptionInterface>
                              current_remote_description) {
    DCHECK(main_thread_->BelongsToCurrentThread());
    if (handler_) {
      handler_->OnSessionDescriptionsUpdated(
          std::move(pending_local_description),
          std::move(current_local_description),
          std::move(pending_remote_description),
          std::move(current_remote_description));
    }
    // Since OnSessionDescriptionsUpdated can fire events, it may cause
    // garbage collection. Ensure that handler_ is still valid.
    if (handler_) {
      handler_->OnIceCandidate(sdp, sdp_mid, sdp_mline_index, component,
                               address_family, username_fragment, url);
    }
  }

  void OnIceCandidateErrorImpl(const String& address,
                               int port,
                               const String& host_candidate,
                               const String& url,
                               int error_code,
                               const String& error_text) {
    DCHECK(main_thread_->BelongsToCurrentThread());
    if (handler_) {
      handler_->OnIceCandidateError(
          address,
          port ? std::optional<uint16_t>(static_cast<uint16_t>(port))
               : std::nullopt,
          host_candidate, url, error_code, error_text);
    }
  }

  void OnInterestingUsage(int usage_pattern) override {
    PostCrossThreadTask(
        *main_thread_.get(), FROM_HERE,
        CrossThreadBindOnce(
            &RTCPeerConnectionHandler::Observer::OnInterestingUsageImpl,
            WrapCrossThreadPersistent(this), usage_pattern));
  }

  void OnInterestingUsageImpl(int usage_pattern) {
    DCHECK(main_thread_->BelongsToCurrentThread());
    if (handler_) {
      handler_->OnInterestingUsage(usage_pattern);
    }
  }

 private:
  const base::WeakPtr<RTCPeerConnectionHandler> handler_;
  const scoped_refptr<base::SingleThreadTaskRunner> main_thread_;
  // The rest of the members are set at Initialize() but are otherwise constant
  // until destruction.
  scoped_refptr<base::SingleThreadTaskRunner> signaling_thread_;
  // A copy of |handler_->native_peer_connection_| for use on the WebRTC
  // signaling thread.
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface>
      native_peer_connection_;
};

class RtcDataChannelLogOutputSinkProxy
    : public webrtc::DataChannelEventObserverInterface {
 public:
  using webrtc::DataChannelEventObserverInterface::Message;

  explicit RtcDataChannelLogOutputSinkProxy(RtcDataChannelEventSink* sink)
      : sink_(sink) {}

  void OnMessage(const Message& message) override {
    auto json = std::make_unique<JSONObject>();
    json->SetString("type", "message");
    // Write a double since a unix timestamp may overflow an int.
    json->SetDouble("unix_timestamp_ms", message.unix_timestamp_ms());
    json->SetInteger("datachannel_id", message.datachannel_id());
    json->SetString("label", String(base::span<const char>(message.label())));
    json->SetString(
        "direction",
        message.direction() == Message::Direction::kSend ? "send" : "receive");
    if (message.data_type() == Message::DataType::kString) {
      json->SetString("data_type", "string");
      json->SetString("data",
                      String(base::span<const unsigned char>(message.data())));
    } else {
      json->SetString("data_type", "binary");
      json->SetString("data", Base64Encode(message.data()));
    }

    StringBuilder string_builder;
    json->WriteJSON(&string_builder);
    string_builder.Append('\n');

    sink_.Lock()->OnWebRtcDataChannelLogWrite(
        Vector<uint8_t>(string_builder.Span8()));
  }

 private:
  const CrossThreadWeakPersistent<RtcDataChannelEventSink> sink_;
};

RTCPeerConnectionHandler::RTCPeerConnectionHandler(
    RTCPeerConnectionHandlerClient* client,
    blink::PeerConnectionDependencyFactory* dependency_factory,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    bool encoded_insertable_streams)
    : client_(client),
      dependency_factory_(dependency_factory),
      track_adapter_map_(
          base::MakeRefCounted<blink::WebRtcMediaStreamTrackAdapterMap>(
              dependency_factory_,
              task_runner)),
      encoded_insertable_streams_(encoded_insertable_streams),
      task_runner_(std::move(task_runner)) {
  CHECK(client_);

  GetPeerConnectionHandlers()->insert(this);
}

// Constructor to be used for creating mocks only.
RTCPeerConnectionHandler::RTCPeerConnectionHandler(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : is_unregistered_(true),  // Avoid CloseAndUnregister in destructor
      task_runner_(std::move(task_runner)) {}

RTCPeerConnectionHandler::~RTCPeerConnectionHandler() {
  if (!is_unregistered_) {
    CloseAndUnregister();
  }
  // Delete RTP Media API objects that may have references to the native peer
  // connection.
  rtp_senders_.clear();
  rtp_receivers_.clear();
  rtp_transceivers_.clear();
  // `signaling_thread_` may be null in some testing-only environments.
  if (!signaling_thread_) {
    return;
  }
  // To avoid a PROXY block-invoke to ~webrtc::PeerConnection in the event
  // that `native_peer_connection_` was the last reference, we move it to the
  // signaling thread in a PostTask.
  signaling_thread_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
            // The binding releases `pc` on the signaling thread as
            // this method goes out of scope.
          },
          std::move(native_peer_connection_)));
}

void RTCPeerConnectionHandler::CloseAndUnregister() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  Close();

  GetPeerConnectionHandlers()->erase(this);
  if (peer_connection_tracker_)
    peer_connection_tracker_->UnregisterPeerConnection(this);

  // Clear the pointer to client_ so that it does not interfere with
  // garbage collection.
  client_ = nullptr;
  is_unregistered_ = true;

  // Reset the `PeerConnectionDependencyFactory` so we don't prevent it from
  // being garbage-collected.
  dependency_factory_ = nullptr;
}

bool RTCPeerConnectionHandler::Initialize(
    ExecutionContext* context,
    const webrtc::PeerConnectionInterface::RTCConfiguration&
        server_configuration,
    WebLocalFrame* frame,
    ExceptionState& exception_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(dependency_factory_);

  CHECK(!initialize_called_);
  initialize_called_ = true;

  // Prevent garbage collection of client_ during processing.
  auto* client_on_stack = client_.Get();
  if (!client_on_stack) {
    return false;
  }

  DCHECK(frame);
  frame_ = frame;
  peer_connection_tracker_ = PeerConnectionTracker::From(*frame);

  configuration_ = server_configuration;

  // Choose between RTC smoothness algorithm and prerenderer smoothing.
  // Prerenderer smoothing is turned on if RTC smoothness is turned off.
  configuration_.set_prerenderer_smoothing(
      !blink::Platform::Current()->RTCSmoothnessAlgorithmEnabled());

  configuration_.set_experiment_cpu_load_estimator(true);

  // Configure optional SRTP configurations enabled via the command line.
  webrtc::CryptoOptions crypto_options;
  crypto_options.srtp.enable_gcm_crypto_suites = true;
  crypto_options.srtp.enable_encrypted_rtp_header_extensions =
      base::FeatureList::IsEnabled(kWebRtcEncryptedRtpHeaderExtensions);
  bool webrtc_post_quantum_key_agreement =
      LocalFrame::FromFrameToken(frame_->GetLocalFrameToken())
          ->GetPage()
          ->GetChromeClient()
          .GetWebRTCPostQuantumKeyAgreement()
          .value_or(base::FeatureList::IsEnabled(features::kWebRtcPqcForDtls));

  if (webrtc_post_quantum_key_agreement) {
    crypto_options.ephemeral_key_exchange_cipher_groups.AddFirst(
        webrtc::CryptoOptions::EphemeralKeyExchangeCipherGroups::
            kX25519_MLKEM768);
  }
  configuration_.crypto_options = crypto_options;
  configuration_.enable_implicit_rollback = true;

  // Apply 40 ms worth of bursting. See webrtc::TaskQueuePacedSender.
  configuration_.pacer_burst_interval = webrtc::TimeDelta::Millis(40);

  configuration_.set_stats_timestamp_with_environment_clock(true);

  peer_connection_observer_ =
      MakeGarbageCollected<Observer>(weak_factory_.GetWeakPtr(), task_runner_);
  native_peer_connection_ = dependency_factory_->CreatePeerConnection(
      configuration_, frame_, peer_connection_observer_, exception_state);
  if (!native_peer_connection_.get()) {
    LOG(ERROR) << "Failed to initialize native PeerConnection.";
    return false;
  }
  // Now the signaling thread exists.
  signaling_thread_ = dependency_factory_->GetWebRtcSignalingTaskRunner();
  peer_connection_observer_->Initialize(signaling_thread_);

  if (peer_connection_tracker_) {
    peer_connection_tracker_->RegisterPeerConnection(this, configuration_,
                                                     frame_);
  }
  // Gratuitous usage of client_on_stack to prevent compiler errors.
  return !!client_on_stack;
}

bool RTCPeerConnectionHandler::InitializeForTest(
    const webrtc::PeerConnectionInterface::RTCConfiguration&
        server_configuration,
    PeerConnectionTracker* peer_connection_tracker,
    ExceptionState& exception_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(dependency_factory_);

  CHECK(!initialize_called_);
  initialize_called_ = true;

  configuration_ = server_configuration;

  peer_connection_observer_ =
      MakeGarbageCollected<Observer>(weak_factory_.GetWeakPtr(), task_runner_);

  native_peer_connection_ = dependency_factory_->CreatePeerConnection(
      configuration_, nullptr, peer_connection_observer_, exception_state);
  if (!native_peer_connection_.get()) {
    LOG(ERROR) << "Failed to initialize native PeerConnection.";
    return false;
  }
  // Now the signaling thread exists.
  signaling_thread_ = dependency_factory_->GetWebRtcSignalingTaskRunner();
  peer_connection_observer_->Initialize(signaling_thread_);
  peer_connection_tracker_ = peer_connection_tracker;
  return true;
}

Vector<std::unique_ptr<RTCRtpTransceiverPlatform>>
RTCPeerConnectionHandler::CreateOffer(RTCSessionDescriptionRequest* request,
                                      RTCOfferOptionsPlatform* options) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::createOffer");

  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackCreateOffer(this, options);

  srflx_candidate_count_ = 0;
  host_derived_srflx_mlines_.clear();
  trickle_native_srflx_count_.clear();
  trickle_native_srflx_first_priority_.clear();

  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions webrtc_options;
  if (options) {
    webrtc_options.offer_to_receive_audio = options->OfferToReceiveAudio();
    webrtc_options.offer_to_receive_video = options->OfferToReceiveVideo();
    webrtc_options.voice_activity_detection = options->VoiceActivityDetection();
    webrtc_options.ice_restart = options->IceRestart();
  }

  scoped_refptr<CreateSessionDescriptionRequest> description_request(
      new webrtc::RefCountedObject<CreateSessionDescriptionRequest>(
          task_runner_, request, weak_factory_.GetWeakPtr(),
          peer_connection_tracker_, PeerConnectionTracker::kActionCreateOffer));

  blink::TransceiverStateSurfacer transceiver_state_surfacer(
      task_runner_, signaling_thread());
  RunSynchronousOnceClosureOnSignalingThread(
      base::BindOnce(&RTCPeerConnectionHandler::CreateOfferOnSignalingThread,
                     base::Unretained(this),
                     base::Unretained(description_request.get()),
                     std::move(webrtc_options),
                     base::Unretained(&transceiver_state_surfacer)),
      "CreateOfferOnSignalingThread");
  DCHECK(transceiver_state_surfacer.is_initialized());

  auto transceiver_states = transceiver_state_surfacer.ObtainStates();
  Vector<std::unique_ptr<RTCRtpTransceiverPlatform>> transceivers;
  for (auto& transceiver_state : transceiver_states) {
    auto transceiver = CreateOrUpdateTransceiver(
        std::move(transceiver_state), blink::TransceiverStateUpdateMode::kAll);
    transceivers.push_back(std::move(transceiver));
  }
  return transceivers;
}

void RTCPeerConnectionHandler::CreateOfferOnSignalingThread(
    webrtc::CreateSessionDescriptionObserver* observer,
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions offer_options,
    blink::TransceiverStateSurfacer* transceiver_state_surfacer) {
  native_peer_connection_->CreateOffer(observer, offer_options);
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceivers = native_peer_connection_->GetTransceivers();
  transceiver_state_surfacer->Initialize(
      native_peer_connection_, track_adapter_map_, std::move(transceivers));
}

void RTCPeerConnectionHandler::CreateAnswer(
    blink::RTCSessionDescriptionRequest* request,
    blink::RTCAnswerOptionsPlatform* options) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::createAnswer");
  scoped_refptr<CreateSessionDescriptionRequest> description_request(
      new webrtc::RefCountedObject<CreateSessionDescriptionRequest>(
          task_runner_, request, weak_factory_.GetWeakPtr(),
          peer_connection_tracker_,
          PeerConnectionTracker::kActionCreateAnswer));
          
  srflx_candidate_count_ = 0;
  host_derived_srflx_mlines_.clear();
  trickle_native_srflx_count_.clear();
  trickle_native_srflx_first_priority_.clear();

  // TODO(tommi): Do this asynchronously via e.g. PostTaskAndReply.
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions webrtc_options;
  if (options) {
    webrtc_options.voice_activity_detection = options->VoiceActivityDetection();
  }
  native_peer_connection_->CreateAnswer(description_request.get(),
                                        webrtc_options);

  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackCreateAnswer(this, options);
}

bool IsOfferOrAnswer(const webrtc::SessionDescriptionInterface* native_desc) {
  DCHECK(native_desc);
  return native_desc->type() == "offer" || native_desc->type() == "answer";
}

void RTCPeerConnectionHandler::SetLocalDescription(
    blink::RTCVoidRequest* request) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::setLocalDescription");

  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackSetSessionDescriptionImplicit(this);

  scoped_refptr<WebRtcSetDescriptionObserverImpl> content_observer =
      base::MakeRefCounted<WebRtcSetDescriptionObserverImpl>(
          weak_factory_.GetWeakPtr(), request, peer_connection_tracker_,
          task_runner_,
          PeerConnectionTracker::kActionSetLocalDescriptionImplicit,
          /*is_rollback=*/true);

  webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>
      webrtc_observer(WebRtcSetLocalDescriptionObserverHandler::Create(
                          task_runner_, signaling_thread(),
                          native_peer_connection_, track_adapter_map_,
                          content_observer)
                          .get());

  PostCrossThreadTask(
      *signaling_thread().get(), FROM_HERE,
      CrossThreadBindOnce(
          &RunClosureWithTrace,
          CrossThreadBindOnce(
              static_cast<void (webrtc::PeerConnectionInterface::*)(
                  webrtc::scoped_refptr<
                      webrtc::SetLocalDescriptionObserverInterface>)>(
                  &webrtc::PeerConnectionInterface::SetLocalDescription),
              native_peer_connection_, webrtc_observer),
          CrossThreadUnretained("SetLocalDescription")));
}

void RTCPeerConnectionHandler::SetLocalDescription(
    blink::RTCVoidRequest* request,
    ParsedSessionDescription parsed_sdp) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::setLocalDescription");

  String sdp = parsed_sdp.sdp();
  String type = parsed_sdp.type();

  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackSetSessionDescription(
        this, sdp, type, PeerConnectionTracker::kSourceLocal);
  }

  const webrtc::SessionDescriptionInterface* native_desc =
      parsed_sdp.description();
  if (!native_desc) {
    webrtc::SdpParseError error(parsed_sdp.error());
    StringBuilder reason_str;
    reason_str.Append("Failed to parse SessionDescription. ");
    reason_str.Append(error.line.c_str());
    reason_str.Append(" ");
    reason_str.Append(error.description.c_str());
    LOG(ERROR) << reason_str.ToString();
    if (peer_connection_tracker_) {
      peer_connection_tracker_->TrackSessionDescriptionCallback(
          this, PeerConnectionTracker::kActionSetLocalDescription, "OnFailure",
          reason_str.ToString());
    }
    // Warning: this line triggers the error callback to be executed, causing
    // arbitrary JavaScript to be executed synchronously. As a result, it is
    // possible for |this| to be deleted after this line. See
    // https://crbug.com/1005251.
    if (request) {
      request->RequestFailed(webrtc::RTCError(
          webrtc::RTCErrorType::INTERNAL_ERROR, reason_str.ToString().Utf8()));
    }
    return;
  }

  if (!first_local_description_ && IsOfferOrAnswer(native_desc)) {
    first_local_description_ =
        std::make_unique<FirstSessionDescription>(native_desc);
    if (first_remote_description_) {
      ReportFirstSessionDescriptions(*first_local_description_,
                                     *first_remote_description_);
    }
  }

  scoped_refptr<WebRtcSetDescriptionObserverImpl> content_observer =
      base::MakeRefCounted<WebRtcSetDescriptionObserverImpl>(
          weak_factory_.GetWeakPtr(), request, peer_connection_tracker_,
          task_runner_, PeerConnectionTracker::kActionSetLocalDescription,
          type == "rollback");

  webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>
      webrtc_observer(WebRtcSetLocalDescriptionObserverHandler::Create(
                          task_runner_, signaling_thread(),
                          native_peer_connection_, track_adapter_map_,
                          content_observer)
                          .get());

  PostCrossThreadTask(
      *signaling_thread().get(), FROM_HERE,
      CrossThreadBindOnce(
          &RunClosureWithTrace,
          CrossThreadBindOnce(
              static_cast<void (webrtc::PeerConnectionInterface::*)(
                  std::unique_ptr<webrtc::SessionDescriptionInterface>,
                  webrtc::scoped_refptr<
                      webrtc::SetLocalDescriptionObserverInterface>)>(
                  &webrtc::PeerConnectionInterface::SetLocalDescription),
              native_peer_connection_, parsed_sdp.release(), webrtc_observer),
          CrossThreadUnretained("SetLocalDescription")));
}

void RTCPeerConnectionHandler::SetRemoteDescription(
    blink::RTCVoidRequest* request,
    ParsedSessionDescription parsed_sdp) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::setRemoteDescription");

  String sdp = parsed_sdp.sdp();
  String type = parsed_sdp.type();

  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackSetSessionDescription(
        this, sdp, type, PeerConnectionTracker::kSourceRemote);
  }

  webrtc::SdpParseError error(parsed_sdp.error());
  const webrtc::SessionDescriptionInterface* native_desc =
      parsed_sdp.description();
  if (!native_desc) {
    StringBuilder reason_str;
    reason_str.Append("Failed to parse SessionDescription. ");
    reason_str.Append(error.line.c_str());
    reason_str.Append(" ");
    reason_str.Append(error.description.c_str());
    LOG(ERROR) << reason_str.ToString();
    if (peer_connection_tracker_) {
      peer_connection_tracker_->TrackSessionDescriptionCallback(
          this, PeerConnectionTracker::kActionSetRemoteDescription, "OnFailure",
          reason_str.ToString());
    }
    // Warning: this line triggers the error callback to be executed, causing
    // arbitrary JavaScript to be executed synchronously. As a result, it is
    // possible for |this| to be deleted after this line. See
    // https://crbug.com/1005251.
    if (request) {
      request->RequestFailed(
          webrtc::RTCError(webrtc::RTCErrorType::UNSUPPORTED_OPERATION,
                           reason_str.ToString().Utf8()));
    }
    return;
  }

  if (!first_remote_description_ && IsOfferOrAnswer(native_desc)) {
    first_remote_description_ =
        std::make_unique<FirstSessionDescription>(native_desc);
    if (first_local_description_) {
      ReportFirstSessionDescriptions(*first_local_description_,
                                     *first_remote_description_);
    }
  }

  scoped_refptr<WebRtcSetDescriptionObserverImpl> content_observer =
      base::MakeRefCounted<WebRtcSetDescriptionObserverImpl>(
          weak_factory_.GetWeakPtr(), request, peer_connection_tracker_,
          task_runner_, PeerConnectionTracker::kActionSetRemoteDescription,
          type == "rollback");

  webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>
      webrtc_observer(WebRtcSetRemoteDescriptionObserverHandler::Create(
                          task_runner_, signaling_thread(),
                          native_peer_connection_, track_adapter_map_,
                          content_observer)
                          .get());

  PostCrossThreadTask(
      *signaling_thread().get(), FROM_HERE,
      CrossThreadBindOnce(
          &RunClosureWithTrace,
          CrossThreadBindOnce(
              static_cast<void (webrtc::PeerConnectionInterface::*)(
                  std::unique_ptr<webrtc::SessionDescriptionInterface>,
                  webrtc::scoped_refptr<
                      webrtc::SetRemoteDescriptionObserverInterface>)>(
                  &webrtc::PeerConnectionInterface::SetRemoteDescription),
              native_peer_connection_, parsed_sdp.release(), webrtc_observer),
          CrossThreadUnretained("SetRemoteDescription")));
}

const webrtc::PeerConnectionInterface::RTCConfiguration&
RTCPeerConnectionHandler::GetConfiguration() const {
  return configuration_;
}

webrtc::RTCErrorType RTCPeerConnectionHandler::SetConfiguration(
    const webrtc::PeerConnectionInterface::RTCConfiguration& blink_config) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::setConfiguration");

  // Update the configuration with the potentially modified fields
  webrtc::PeerConnectionInterface::RTCConfiguration new_configuration =
      configuration_;
  new_configuration.servers = blink_config.servers;
  new_configuration.type = blink_config.type;
  new_configuration.bundle_policy = blink_config.bundle_policy;
  new_configuration.rtcp_mux_policy = blink_config.rtcp_mux_policy;
  new_configuration.certificates = blink_config.certificates;
  new_configuration.ice_candidate_pool_size =
      blink_config.ice_candidate_pool_size;

  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackSetConfiguration(this, new_configuration);

  webrtc::RTCError webrtc_error =
      native_peer_connection_->SetConfiguration(new_configuration);
  if (webrtc_error.ok()) {
    configuration_ = new_configuration;
  }

  return webrtc_error.type();
}

void RTCPeerConnectionHandler::AddIceCandidate(
    RTCVoidRequest* request,
    RTCIceCandidatePlatform* candidate) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(dependency_factory_);
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::addIceCandidate");
  std::unique_ptr<webrtc::IceCandidate> native_candidate(
      dependency_factory_->CreateIceCandidate(
          candidate->SdpMid(),
          candidate->SdpMLineIndex()
              ? static_cast<int>(*candidate->SdpMLineIndex())
              : -1,
          candidate->Candidate()));

  auto callback_on_task_runner =
      [](base::WeakPtr<RTCPeerConnectionHandler> handler_weak_ptr,
         CrossThreadPersistent<PeerConnectionTracker> tracker_ptr,
         std::unique_ptr<webrtc::SessionDescriptionInterface>
             pending_local_description,
         std::unique_ptr<webrtc::SessionDescriptionInterface>
             current_local_description,
         std::unique_ptr<webrtc::SessionDescriptionInterface>
             pending_remote_description,
         std::unique_ptr<webrtc::SessionDescriptionInterface>
             current_remote_description,
         CrossThreadPersistent<RTCIceCandidatePlatform> candidate,
         webrtc::RTCError result, RTCVoidRequest* request) {
        // Inform tracker (chrome://webrtc-internals).
        // Note that because the CrossThreadBindOnce() below uses a
        // CrossThreadWeakPersistent when binding |tracker_ptr| this lambda may
        // be invoked with a null |tracker_ptr| so we have to guard against it.
        if (handler_weak_ptr && tracker_ptr) {
          tracker_ptr->TrackAddIceCandidate(
              handler_weak_ptr.get(), candidate,
              PeerConnectionTracker::kSourceRemote, result.ok());
        }
        // Update session descriptions.
        if (handler_weak_ptr) {
          handler_weak_ptr->OnSessionDescriptionsUpdated(
              std::move(pending_local_description),
              std::move(current_local_description),
              std::move(pending_remote_description),
              std::move(current_remote_description));
        }
        // Resolve promise.
        if (result.ok())
          request->RequestSucceeded();
        else
          request->RequestFailed(result);
      };

  native_peer_connection_->AddIceCandidate(
      std::move(native_candidate),
      [pc = native_peer_connection_, task_runner = task_runner_,
       handler_weak_ptr = weak_factory_.GetWeakPtr(),
       tracker_weak_ptr =
           WrapCrossThreadWeakPersistent(peer_connection_tracker_.Get()),
       persistent_candidate = WrapCrossThreadPersistent(candidate),
       persistent_request = WrapCrossThreadPersistent(request),
       callback_on_task_runner =
           std::move(callback_on_task_runner)](webrtc::RTCError result) {
        // Grab a snapshot of all the session descriptions. AddIceCandidate may
        // have modified the remote description.
        std::unique_ptr<webrtc::SessionDescriptionInterface>
            pending_local_description =
                CopySessionDescription(pc->pending_local_description());
        std::unique_ptr<webrtc::SessionDescriptionInterface>
            current_local_description =
                CopySessionDescription(pc->current_local_description());
        std::unique_ptr<webrtc::SessionDescriptionInterface>
            pending_remote_description =
                CopySessionDescription(pc->pending_remote_description());
        std::unique_ptr<webrtc::SessionDescriptionInterface>
            current_remote_description =
                CopySessionDescription(pc->current_remote_description());
        // This callback is invoked on the webrtc signaling thread (this is true
        // in production, not in rtc_peer_connection_handler_test.cc which uses
        // a fake |native_peer_connection_|). Jump back to the renderer thread.
        PostCrossThreadTask(
            *task_runner, FROM_HERE,
            CrossThreadBindOnce(
                std::move(callback_on_task_runner), handler_weak_ptr,
                tracker_weak_ptr, std::move(pending_local_description),
                std::move(current_local_description),
                std::move(pending_remote_description),
                std::move(current_remote_description),
                std::move(persistent_candidate), std::move(result),
                std::move(persistent_request)));
      });
}

void RTCPeerConnectionHandler::RestartIce() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // The proxy invokes RestartIce() on the signaling thread.
  native_peer_connection_->RestartIce();
}

void RTCPeerConnectionHandler::GetStandardStatsForTracker(
    webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback> observer) {
  native_peer_connection_->GetStats(observer.get());
}

void RTCPeerConnectionHandler::EmitCurrentStateForTracker() {
  if (!peer_connection_tracker_) {
    return;
  }
  RTC_DCHECK(native_peer_connection_);
  const webrtc::SessionDescriptionInterface* local_desc =
      native_peer_connection_->local_description();
  // If the local desc is an answer, emit it after the offer.
  if (local_desc != nullptr &&
      local_desc->GetType() == webrtc::SdpType::kOffer) {
    std::string local_sdp;
    if (local_desc->ToString(&local_sdp)) {
      peer_connection_tracker_->TrackSetSessionDescription(
          this, String(local_sdp),
          String(SdpTypeToString(local_desc->GetType())),
          PeerConnectionTracker::kSourceLocal);
    }
  }
  const webrtc::SessionDescriptionInterface* remote_desc =
      native_peer_connection_->remote_description();
  if (remote_desc != nullptr) {
    std::string remote_sdp;
    if (remote_desc->ToString(&remote_sdp)) {
      peer_connection_tracker_->TrackSetSessionDescription(
          this, String(remote_sdp),
          String(SdpTypeToString(remote_desc->GetType())),
          PeerConnectionTracker::kSourceRemote);
    }
  }

  if (local_desc != nullptr &&
      local_desc->GetType() != webrtc::SdpType::kOffer) {
    std::string local_sdp;
    if (local_desc->ToString(&local_sdp)) {
      peer_connection_tracker_->TrackSetSessionDescription(
          this, String(local_sdp),
          String(SdpTypeToString(local_desc->GetType())),
          PeerConnectionTracker::kSourceLocal);
    }
  }
  peer_connection_tracker_->TrackSignalingStateChange(
      this, native_peer_connection_->signaling_state());
  peer_connection_tracker_->TrackIceConnectionStateChange(
      this, native_peer_connection_->standardized_ice_connection_state());
  peer_connection_tracker_->TrackConnectionStateChange(
      this, native_peer_connection_->peer_connection_state());
}

void RTCPeerConnectionHandler::GetStats(RTCStatsReportCallback callback) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  PostCrossThreadTask(
      *signaling_thread().get(), FROM_HERE,
      CrossThreadBindOnce(&GetRTCStatsOnSignalingThread, task_runner_,
                          native_peer_connection_,
                          CrossThreadBindOnce(std::move(callback))));
}

webrtc::RTCErrorOr<std::unique_ptr<RTCRtpTransceiverPlatform>>
RTCPeerConnectionHandler::AddTransceiverWithTrack(
    MediaStreamComponent* component,
    const webrtc::RtpTransceiverInit& init) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  std::unique_ptr<blink::WebRtcMediaStreamTrackAdapterMap::AdapterRef>
      track_ref = track_adapter_map_->GetOrCreateLocalTrackAdapter(component);
  blink::TransceiverStateSurfacer transceiver_state_surfacer(
      task_runner_, signaling_thread());
  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      error_or_transceiver;
  RunSynchronousOnceClosureOnSignalingThread(
      base::BindOnce(
          &RTCPeerConnectionHandler::AddTransceiverWithTrackOnSignalingThread,
          base::Unretained(this),
          base::RetainedRef(track_ref->webrtc_track().get()), std::cref(init),
          base::Unretained(&transceiver_state_surfacer),
          base::Unretained(&error_or_transceiver)),
      "AddTransceiverWithTrackOnSignalingThread");
  if (!error_or_transceiver.ok()) {
    // Don't leave the surfacer in a pending state.
    transceiver_state_surfacer.ObtainStates();
    return error_or_transceiver.MoveError();
  }

  auto transceiver_states = transceiver_state_surfacer.ObtainStates();
  auto transceiver =
      CreateOrUpdateTransceiver(std::move(transceiver_states[0]),
                                blink::TransceiverStateUpdateMode::kAll);
  std::unique_ptr<RTCRtpTransceiverPlatform> platform_transceiver =
      std::move(transceiver);
  if (peer_connection_tracker_) {
    size_t transceiver_index = GetTransceiverIndex(*platform_transceiver.get());
    peer_connection_tracker_->TrackAddTransceiver(
        this, PeerConnectionTracker::TransceiverUpdatedReason::kAddTransceiver,
        *platform_transceiver.get(), transceiver_index);
  }
  return platform_transceiver;
}

void RTCPeerConnectionHandler::AddTransceiverWithTrackOnSignalingThread(
    webrtc::MediaStreamTrackInterface* webrtc_track,
    webrtc::RtpTransceiverInit init,
    blink::TransceiverStateSurfacer* transceiver_state_surfacer,
    webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>*
        error_or_transceiver) {
  *error_or_transceiver = native_peer_connection_->AddTransceiver(
      webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>(webrtc_track),
      init);
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceivers;
  if (error_or_transceiver->ok())
    transceivers.push_back(error_or_transceiver->value());
  transceiver_state_surfacer->Initialize(native_peer_connection_,
                                         track_adapter_map_, transceivers);
}

webrtc::RTCErrorOr<std::unique_ptr<RTCRtpTransceiverPlatform>>
RTCPeerConnectionHandler::AddTransceiverWithKind(
    const String& kind,
    const webrtc::RtpTransceiverInit& init) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  webrtc::MediaType media_type;
  if (kind == webrtc::MediaStreamTrackInterface::kAudioKind) {
    media_type = webrtc::MediaType::AUDIO;
  } else {
    DCHECK_EQ(kind, webrtc::MediaStreamTrackInterface::kVideoKind);
    media_type = webrtc::MediaType::VIDEO;
  }
  blink::TransceiverStateSurfacer transceiver_state_surfacer(
      task_runner_, signaling_thread());
  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      error_or_transceiver;
  RunSynchronousOnceClosureOnSignalingThread(
      base::BindOnce(&RTCPeerConnectionHandler::
                         AddTransceiverWithMediaTypeOnSignalingThread,
                     base::Unretained(this), std::cref(media_type),
                     std::cref(init),
                     base::Unretained(&transceiver_state_surfacer),
                     base::Unretained(&error_or_transceiver)),
      "AddTransceiverWithMediaTypeOnSignalingThread");
  if (!error_or_transceiver.ok()) {
    // Don't leave the surfacer in a pending state.
    transceiver_state_surfacer.ObtainStates();
    return error_or_transceiver.MoveError();
  }

  auto transceiver_states = transceiver_state_surfacer.ObtainStates();
  auto transceiver =
      CreateOrUpdateTransceiver(std::move(transceiver_states[0]),
                                blink::TransceiverStateUpdateMode::kAll);
  std::unique_ptr<RTCRtpTransceiverPlatform> platform_transceiver =
      std::move(transceiver);
  if (peer_connection_tracker_) {
    size_t transceiver_index = GetTransceiverIndex(*platform_transceiver.get());
    peer_connection_tracker_->TrackAddTransceiver(
        this, PeerConnectionTracker::TransceiverUpdatedReason::kAddTransceiver,
        *platform_transceiver.get(), transceiver_index);
  }
  return std::move(platform_transceiver);
}

void RTCPeerConnectionHandler::AddTransceiverWithMediaTypeOnSignalingThread(
    webrtc::MediaType media_type,
    webrtc::RtpTransceiverInit init,
    blink::TransceiverStateSurfacer* transceiver_state_surfacer,
    webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>*
        error_or_transceiver) {
  *error_or_transceiver =
      native_peer_connection_->AddTransceiver(media_type, init);
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceivers;
  if (error_or_transceiver->ok())
    transceivers.push_back(error_or_transceiver->value());
  transceiver_state_surfacer->Initialize(native_peer_connection_,
                                         track_adapter_map_, transceivers);
}

webrtc::RTCErrorOr<std::unique_ptr<RTCRtpTransceiverPlatform>>
RTCPeerConnectionHandler::AddTrack(
    MediaStreamComponent* component,
    const MediaStreamDescriptorVector& descriptors) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::AddTrack");

  std::unique_ptr<blink::WebRtcMediaStreamTrackAdapterMap::AdapterRef>
      track_ref = track_adapter_map_->GetOrCreateLocalTrackAdapter(component);
  std::vector<std::string> stream_ids(descriptors.size());
  for (wtf_size_t i = 0; i < descriptors.size(); ++i) {
    stream_ids[i] = descriptors[i]->Id().Utf8();
  }

  // Invoke native AddTrack() on the signaling thread and surface the resulting
  // transceiver.
  blink::TransceiverStateSurfacer transceiver_state_surfacer(
      task_runner_, signaling_thread());
  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpSenderInterface>>
      error_or_sender;
  RunSynchronousOnceClosureOnSignalingThread(
      base::BindOnce(&RTCPeerConnectionHandler::AddTrackOnSignalingThread,
                     base::Unretained(this),
                     base::RetainedRef(track_ref->webrtc_track().get()),
                     std::move(stream_ids),
                     base::Unretained(&transceiver_state_surfacer),
                     base::Unretained(&error_or_sender)),
      "AddTrackOnSignalingThread");
  DCHECK(transceiver_state_surfacer.is_initialized());
  if (!error_or_sender.ok()) {
    // Don't leave the surfacer in a pending state.
    transceiver_state_surfacer.ObtainStates();
    return error_or_sender.MoveError();
  }
  track_metrics_.AddTrack(MediaStreamTrackMetrics::Direction::kSend,
                          MediaStreamTrackMetricsKind(component),
                          component->Id().Utf8());

  auto transceiver_states = transceiver_state_surfacer.ObtainStates();
  DCHECK_EQ(transceiver_states.size(), 1u);
  auto transceiver_state = std::move(transceiver_states[0]);

  std::unique_ptr<RTCRtpTransceiverPlatform> platform_transceiver;
  // Create or recycle a transceiver.
  auto transceiver = CreateOrUpdateTransceiver(
      std::move(transceiver_state), blink::TransceiverStateUpdateMode::kAll);
  platform_transceiver = std::move(transceiver);
  if (peer_connection_tracker_) {
    size_t transceiver_index = GetTransceiverIndex(*platform_transceiver.get());
    peer_connection_tracker_->TrackAddTransceiver(
        this, PeerConnectionTracker::TransceiverUpdatedReason::kAddTrack,
        *platform_transceiver.get(), transceiver_index);
  }
  for (const auto& stream_id : rtp_senders_.back()->state().stream_ids()) {
    if (GetLocalStreamUsageCount(rtp_senders_, stream_id) == 1u) {
      // This is the first occurrence of this stream.
      blink::PerSessionWebRTCAPIMetrics::GetInstance()
          ->IncrementStreamCounter();
    }
  }
  return platform_transceiver;
}

void RTCPeerConnectionHandler::AddTrackOnSignalingThread(
    webrtc::MediaStreamTrackInterface* track,
    std::vector<std::string> stream_ids,
    blink::TransceiverStateSurfacer* transceiver_state_surfacer,
    webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpSenderInterface>>*
        error_or_sender) {
  *error_or_sender = native_peer_connection_->AddTrack(
      webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>(track),
      stream_ids);
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceivers;
  if (error_or_sender->ok()) {
    auto sender = error_or_sender->value();
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
        transceiver_for_sender;
    for (const auto& transceiver : native_peer_connection_->GetTransceivers()) {
      if (transceiver->sender() == sender) {
        transceiver_for_sender = transceiver;
        break;
      }
    }
    DCHECK(transceiver_for_sender);
    transceivers = {transceiver_for_sender};
  }
  transceiver_state_surfacer->Initialize(
      native_peer_connection_, track_adapter_map_, std::move(transceivers));
}

webrtc::RTCErrorOr<std::unique_ptr<RTCRtpTransceiverPlatform>>
RTCPeerConnectionHandler::RemoveTrack(blink::RTCRtpSenderPlatform* web_sender) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::RemoveTrack");
  auto it = FindSender(web_sender->Id());
  if (it == rtp_senders_.end())
    return webrtc::RTCError(webrtc::RTCErrorType::INVALID_PARAMETER);
  const auto& sender = *it;
  auto webrtc_sender = sender->state().webrtc_sender();

  blink::TransceiverStateSurfacer transceiver_state_surfacer(
      task_runner_, signaling_thread());
  std::optional<webrtc::RTCError> result;
  RunSynchronousOnceClosureOnSignalingThread(
      base::BindOnce(&RTCPeerConnectionHandler::RemoveTrackOnSignalingThread,
                     base::Unretained(this),
                     base::RetainedRef(webrtc_sender.get()),
                     base::Unretained(&transceiver_state_surfacer),
                     base::Unretained(&result)),
      "RemoveTrackOnSignalingThread");
  DCHECK(transceiver_state_surfacer.is_initialized());
  if (!result || !result->ok()) {
    // Don't leave the surfacer in a pending state.
    transceiver_state_surfacer.ObtainStates();
    if (!result) {
      // Operation has been cancelled.
      return std::unique_ptr<RTCRtpTransceiverPlatform>(nullptr);
    }
    return std::move(*result);
  }

  auto transceiver_states = transceiver_state_surfacer.ObtainStates();
  DCHECK_EQ(transceiver_states.size(), 1u);
  auto transceiver_state = std::move(transceiver_states[0]);

  // Update the transceiver.
  auto transceiver = CreateOrUpdateTransceiver(
      std::move(transceiver_state), blink::TransceiverStateUpdateMode::kAll);
  if (peer_connection_tracker_) {
    size_t transceiver_index = GetTransceiverIndex(*transceiver);
    peer_connection_tracker_->TrackModifyTransceiver(
        this, PeerConnectionTracker::TransceiverUpdatedReason::kRemoveTrack,
        *transceiver.get(), transceiver_index);
  }
  std::unique_ptr<RTCRtpTransceiverPlatform> platform_transceiver =
      std::move(transceiver);
  return platform_transceiver;
}

void RTCPeerConnectionHandler::RemoveTrackOnSignalingThread(
    webrtc::RtpSenderInterface* sender,
    blink::TransceiverStateSurfacer* transceiver_state_surfacer,
    std::optional<webrtc::RTCError>* result) {
  *result = native_peer_connection_->RemoveTrackOrError(
      webrtc::scoped_refptr<webrtc::RtpSenderInterface>(sender));
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceivers;
  if ((*result)->ok()) {
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
        transceiver_for_sender = nullptr;
    for (const auto& transceiver : native_peer_connection_->GetTransceivers()) {
      if (transceiver->sender() == sender) {
        transceiver_for_sender = transceiver;
        break;
      }
    }
    if (!transceiver_for_sender) {
      // If the transceiver doesn't exist, it must have been rolled back while
      // we were performing removeTrack(). Abort this operation.
      *result = std::nullopt;
    } else {
      transceivers = {transceiver_for_sender};
    }
  }
  transceiver_state_surfacer->Initialize(
      native_peer_connection_, track_adapter_map_, std::move(transceivers));
}

Vector<std::unique_ptr<blink::RTCRtpSenderPlatform>>
RTCPeerConnectionHandler::GetPlatformSenders() const {
  Vector<std::unique_ptr<blink::RTCRtpSenderPlatform>> senders;
  for (const auto& sender : rtp_senders_) {
    senders.push_back(sender->ShallowCopy());
  }
  return senders;
}

void RTCPeerConnectionHandler::CloseClientPeerConnection() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!is_closed_)
    client_->ClosePeerConnection();
}

void RTCPeerConnectionHandler::OnThermalStateChange(
    mojom::blink::DeviceThermalState thermal_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (is_closed_)
    return;
  if (!base::FeatureList::IsEnabled(kWebRtcThermalResource))
    return;
  if (!thermal_resource_) {
    thermal_resource_ = ThermalResource::Create(task_runner_);
    native_peer_connection_->AddAdaptationResource(
        webrtc::scoped_refptr<ThermalResource>(thermal_resource_.get()));
  }
  thermal_resource_->OnThermalMeasurement(thermal_state);
}

void RTCPeerConnectionHandler::StartEventLog(int output_period_ms) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // TODO(eladalon): StartRtcEventLog() return value is not useful; remove it
  // or find a way to be able to use it.
  // https://crbug.com/775415
  native_peer_connection_->StartRtcEventLog(
      std::make_unique<RtcEventLogOutputSinkProxy>(peer_connection_observer_),
      output_period_ms);
}

void RTCPeerConnectionHandler::StopEventLog() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  native_peer_connection_->StopRtcEventLog();
}

void RTCPeerConnectionHandler::OnWebRtcEventLogWrite(
    const Vector<uint8_t>& output) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackRtcEventLogWrite(this, output);
  }
}

void RTCPeerConnectionHandler::StartDataChannelLog() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  native_peer_connection_->SetDataChannelEventObserver(
      std::make_unique<RtcDataChannelLogOutputSinkProxy>(
          peer_connection_observer_));
}

void RTCPeerConnectionHandler::StopDataChannelLog() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  native_peer_connection_->SetDataChannelEventObserver(nullptr);
}

void RTCPeerConnectionHandler::OnWebRtcDataChannelLogWrite(
    const Vector<uint8_t>& output) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackRtcDataChannelLogWrite(this, output);
  }
}

webrtc::scoped_refptr<DataChannelInterface>
RTCPeerConnectionHandler::CreateDataChannel(
    const String& label,
    const webrtc::DataChannelInit& init) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::createDataChannel");
  DVLOG(1) << "createDataChannel label " << label.Utf8();

  webrtc::RTCErrorOr<webrtc::scoped_refptr<DataChannelInterface>>
      webrtc_channel = native_peer_connection_->CreateDataChannelOrError(
          label.Utf8(), &init);
  if (!webrtc_channel.ok()) {
    DLOG(ERROR) << "Could not create native data channel: "
                << webrtc_channel.error().message();
    return nullptr;
  }
  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackCreateDataChannel(
        this, webrtc_channel.value().get(),
        PeerConnectionTracker::kSourceLocal);
  }

  return webrtc_channel.value();
}

void RTCPeerConnectionHandler::Close() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DVLOG(1) << "RTCPeerConnectionHandler::stop";

  if (is_closed_ || !native_peer_connection_.get())
    return;  // Already stopped.

  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackClose(this);

  native_peer_connection_->Close();

  // This object may no longer forward call backs to blink.
  is_closed_ = true;
}

webrtc::PeerConnectionInterface*
RTCPeerConnectionHandler::NativePeerConnection() {
  return native_peer_connection();
}

void RTCPeerConnectionHandler::RunSynchronousOnceClosureOnSignalingThread(
    base::OnceClosure closure,
    const char* trace_event_name) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  scoped_refptr<base::SingleThreadTaskRunner> thread(signaling_thread());
  if (!thread.get() || thread->BelongsToCurrentThread()) {
    TRACE_EVENT0("webrtc", trace_event_name);
    std::move(closure).Run();
  } else {
    base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                              base::WaitableEvent::InitialState::NOT_SIGNALED);
    thread->PostTask(
        FROM_HERE,
        base::BindOnce(&RunSynchronousOnceClosure, std::move(closure),
                       base::Unretained(trace_event_name),
                       base::Unretained(&event)));
    event.Wait();
  }
}

void RTCPeerConnectionHandler::OnSessionDescriptionsUpdated(
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        pending_local_description,
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        current_local_description,
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        pending_remote_description,
    std::unique_ptr<webrtc::SessionDescriptionInterface>
        current_remote_description) {
  // Prevent garbage collection of client_ during processing.
  auto* client_on_stack = client_.Get();
  if (!client_on_stack || is_closed_) {
    return;
  }
  client_on_stack->DidChangeSessionDescriptions(
      pending_local_description
          ? CreateWebKitSessionDescription(pending_local_description.get())
          : nullptr,
      current_local_description
          ? CreateWebKitSessionDescription(current_local_description.get())
          : nullptr,
      pending_remote_description
          ? CreateWebKitSessionDescription(pending_remote_description.get())
          : nullptr,
      current_remote_description
          ? CreateWebKitSessionDescription(current_remote_description.get())
          : nullptr);
}

// Note: This function is purely for chrome://webrtc-internals/ tracking
// purposes. The JavaScript visible event and attribute is processed together
// with transceiver or receiver changes.
void RTCPeerConnectionHandler::TrackSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::TrackSignalingChange");
  if (previous_signaling_state_ ==
          webrtc::PeerConnectionInterface::kHaveLocalOffer &&
      new_state == webrtc::PeerConnectionInterface::kHaveRemoteOffer) {
    // Inject missing kStable in case of implicit rollback.
    auto stable_state = webrtc::PeerConnectionInterface::kStable;
    if (peer_connection_tracker_)
      peer_connection_tracker_->TrackSignalingStateChange(this, stable_state);
  }
  previous_signaling_state_ = new_state;
  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackSignalingStateChange(this, new_state);
}

// Called any time the lower layer IceConnectionState changes, which is NOT in
// sync with the iceConnectionState that is exposed to JavaScript (that one is
// computed by RTCPeerConnection::UpdateIceConnectionState)! This method is
// purely used for UMA reporting. We may want to consider wiring this up to
// UpdateIceConnectionState() instead...
void RTCPeerConnectionHandler::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::OnIceConnectionChange");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  ReportICEState(new_state);
  track_metrics_.IceConnectionChange(new_state);
}

void RTCPeerConnectionHandler::TrackIceConnectionStateChange(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!peer_connection_tracker_)
    return;
  peer_connection_tracker_->TrackIceConnectionStateChange(this, state);
}

// Called any time the combined peerconnection state changes
void RTCPeerConnectionHandler::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackConnectionStateChange(this, new_state);
  if (!is_closed_)
    client_->DidChangePeerConnectionState(new_state);
}

// Called any time the IceGatheringState changes
void RTCPeerConnectionHandler::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::OnIceGatheringChange");
  if (new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
    // Must run BEFORE DidChangeIceGatheringState below: the renderer-level
    // RTCPeerConnection automatically fires the spec-mandated final
    // null-candidate "end of candidates" event in response to that state
    // change (see RTCPeerConnection::ChangeIceGatheringState), so any
    // fabricated candidates need to be delivered first to preserve the same
    // event ordering genuine Chrome would produce.
    MaybeFabricateForwardModeCandidates();
  }
  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackIceGatheringStateChange(this, new_state);
  if (!is_closed_)
    client_->DidChangeIceGatheringState(new_state);
}

void RTCPeerConnectionHandler::OnNegotiationNeededEvent(uint32_t event_id) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::OnNegotiationNeededEvent");
  if (is_closed_)
    return;
  if (!native_peer_connection_->ShouldFireNegotiationNeededEvent(event_id)) {
    return;
  }
  if (peer_connection_tracker_)
    peer_connection_tracker_->TrackOnRenegotiationNeeded(this);
  client_->NegotiationNeeded();
}

void RTCPeerConnectionHandler::OnModifySctpTransport(
    blink::WebRTCSctpTransportSnapshot state) {
  if (client_)
    client_->DidModifySctpTransport(state);
}

void RTCPeerConnectionHandler::OnModifyTransceivers(
    webrtc::PeerConnectionInterface::SignalingState signaling_state,
    std::vector<blink::RtpTransceiverState> transceiver_states,
    bool is_remote_description,
    bool is_rollback) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  Vector<std::unique_ptr<RTCRtpTransceiverPlatform>> platform_transceivers(
      base::checked_cast<wtf_size_t>(transceiver_states.size()));
  PeerConnectionTracker::TransceiverUpdatedReason update_reason =
      !is_remote_description ? PeerConnectionTracker::TransceiverUpdatedReason::
                                   kSetLocalDescription
                             : PeerConnectionTracker::TransceiverUpdatedReason::
                                   kSetRemoteDescription;
  Vector<uintptr_t> ids(
      base::checked_cast<wtf_size_t>(transceiver_states.size()));
  for (wtf_size_t i = 0; i < transceiver_states.size(); ++i) {
    // Figure out if this transceiver is new or if setting the state modified
    // the transceiver such that it should be logged by the
    // |peer_connection_tracker_|.
    uintptr_t transceiver_id = blink::RTCRtpTransceiverImpl::GetId(
        transceiver_states[i].webrtc_transceiver().get());
    ids[i] = transceiver_id;
    auto it = FindTransceiver(transceiver_id);
    bool transceiver_is_new = (it == rtp_transceivers_.end());
    bool transceiver_was_modified = false;
    if (!transceiver_is_new) {
      const auto& previous_state = (*it)->state();
      transceiver_was_modified =
          previous_state.mid() != transceiver_states[i].mid() ||
          previous_state.direction() != transceiver_states[i].direction() ||
          previous_state.current_direction() !=
              transceiver_states[i].current_direction() ||
          previous_state.header_extensions_negotiated() !=
              transceiver_states[i].header_extensions_negotiated();
    }

    // Update the transceiver.
    platform_transceivers[i] = CreateOrUpdateTransceiver(
        std::move(transceiver_states[i]),
        blink::TransceiverStateUpdateMode::kSetDescription);

    // Log a "transceiverAdded" or "transceiverModified" event in
    // chrome://webrtc-internals if new or modified.
    if (peer_connection_tracker_ &&
        (transceiver_is_new || transceiver_was_modified)) {
      size_t transceiver_index = GetTransceiverIndex(*platform_transceivers[i]);
      if (transceiver_is_new) {
        peer_connection_tracker_->TrackAddTransceiver(
            this, update_reason, *platform_transceivers[i].get(),
            transceiver_index);
      } else if (transceiver_was_modified) {
        peer_connection_tracker_->TrackModifyTransceiver(
            this, update_reason, *platform_transceivers[i].get(),
            transceiver_index);
      }
    }
  }
  // Search for removed transceivers by comparing to previous state. All of
  // these transceivers will have been stopped in the WebRTC layers, but we do
  // not have access to their states anymore. So it is up to `client_` to ensure
  // removed transceivers are reflected as "stopped" in JavaScript.
  Vector<uintptr_t> removed_transceivers;
  for (auto transceiver_id : previous_transceiver_ids_) {
    if (!base::Contains(ids, transceiver_id)) {
      removed_transceivers.emplace_back(transceiver_id);
      rtp_transceivers_.erase(FindTransceiver(transceiver_id));
    }
  }
  previous_transceiver_ids_ = ids;
  if (!is_closed_) {
    client_->DidModifyTransceivers(
        signaling_state, std::move(platform_transceivers), removed_transceivers,
        is_remote_description || is_rollback);
  }
}

void RTCPeerConnectionHandler::OnDataChannel(
    webrtc::scoped_refptr<DataChannelInterface> channel) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::OnDataChannelImpl");

  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackCreateDataChannel(
        this, channel.get(), PeerConnectionTracker::kSourceRemote);
  }

  if (!is_closed_)
    client_->DidAddRemoteDataChannel(std::move(channel));
}

void RTCPeerConnectionHandler::OnIceCandidate(const String& sdp,
                                               const String& sdp_mid,
                                               int sdp_mline_index,
                                               int component,
                                               int address_family,
                                               const String& usernameFragment,
                                               const String& url) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // In order to ensure that the RTCPeerConnection is not garbage collected
  // from under the function, we keep a pointer to it on the stack.
  auto* client_on_stack = client_.Get();
  if (!client_on_stack) {
    return;
  }
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::OnIceCandidateImpl");

  // WebRTC "Base on IP Proxy": Replace real IP with proxy IP in ICE candidates.
  // When --webrtc-proxy-ip=<IP> is set, or when --webrtc-mode=forward with
  // --proxy-server=<proxy> is set, the candidate SDP is sanitized before
  // exposing to JavaScript.
  String modified_sdp = sdp;
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  std::string proxy_ip = GetWebRtcSdpProxyIp(command_line);
  std::vector<std::string> extra_candidates;
  if (!proxy_ip.empty()) {
    modified_sdp = SanitizeSdp(sdp, proxy_ip, &srflx_candidate_count_,
                               &extra_candidates,
                               IsForwardModeUsingNonSocks5Proxy(command_line),
                               &host_derived_srflx_mlines_, sdp_mline_index,
                               &trickle_native_srflx_count_,
                               &trickle_native_srflx_first_priority_,
                               IsReplaceMode(command_line));
  }

  // This line can cause garbage collection.
  auto* platform_candidate = MakeGarbageCollected<RTCIceCandidatePlatform>(
      modified_sdp, sdp_mid, sdp_mline_index, usernameFragment, url);
  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackAddIceCandidate(
        this, platform_candidate, PeerConnectionTracker::kSourceLocal, true);
  }

  if (!is_closed_ && client_on_stack) {
    client_on_stack->DidGenerateICECandidate(platform_candidate);
  }

  for (const auto& extra : extra_candidates) {
    auto* extra_platform_candidate = MakeGarbageCollected<RTCIceCandidatePlatform>(
        String::FromUTF8(extra), sdp_mid, sdp_mline_index, usernameFragment, url);
    if (peer_connection_tracker_) {
      peer_connection_tracker_->TrackAddIceCandidate(
          this, extra_platform_candidate, PeerConnectionTracker::kSourceLocal, true);
    }
    if (!is_closed_ && client_on_stack) {
      client_on_stack->DidGenerateICECandidate(extra_platform_candidate);
    }
  }
}

void RTCPeerConnectionHandler::MaybeFabricateForwardModeCandidates() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  VLOG(1) << "MaybeFabricateForwardModeCandidates: entry, srflx_candidate_count_="
          << srflx_candidate_count_
          << " is_non_socks5_forward="
          << IsForwardModeUsingNonSocks5Proxy(command_line);
  if (!IsForwardModeUsingNonSocks5Proxy(command_line)) {
    return;
  }
  if (srflx_candidate_count_ != 0) {
    // Either already fabricated for this gathering cycle, or (shouldn't
    // happen for this mode, since force_no_udp_egress guarantees zero real
    // local ports — see peer_connection_dependency_factory.cc) a real
    // candidate already fired via OnIceCandidate.
    VLOG(1) << "MaybeFabricateForwardModeCandidates: skipping, already "
               "fabricated/real candidate exists this cycle.";
    return;
  }
  std::string proxy_ip_raw = GetWebRtcSdpProxyIp(command_line);
  if (proxy_ip_raw.empty()) {
    return;
  }

  auto* client_on_stack = client_.Get();
  if (!client_on_stack || is_closed_) {
    return;
  }

  std::string ufrag;
  if (native_peer_connection_) {
    const webrtc::SessionDescriptionInterface* local_desc =
        native_peer_connection_->local_description();
    std::string local_sdp;
    if (local_desc && local_desc->ToString(&local_sdp)) {
      ufrag = ExtractIceUfrag(local_sdp);
    }
  }
  if (ufrag.empty()) {
    return;
  }

  std::string proxy_ip = NormalizeProxyIp(proxy_ip_raw);
  bool is_ipv6 = proxy_ip.find(':') != std::string::npos;
  std::string host_line, host_line2, srflx1, srflx2;
  FabricateHostAndSrflxTriplet(ufrag, proxy_ip, is_ipv6, &host_line,
                                &host_line2, &srflx1, &srflx2);
  VLOG(1) << "MaybeFabricateForwardModeCandidates: emitting trickle events, "
             "ufrag='"
          << ufrag << "' host_line='" << host_line << "'";
  srflx_candidate_count_ = 1;  // Guard against double-fabrication.

  String sdp_mid = "0";
  String username_fragment = String::FromUTF8(ufrag);
  for (const std::string& cand : {host_line, host_line2, srflx1, srflx2}) {
    auto* platform_candidate = MakeGarbageCollected<RTCIceCandidatePlatform>(
        String::FromUTF8(cand), sdp_mid, /*sdp_mline_index=*/0,
        username_fragment, String());
    if (peer_connection_tracker_) {
      peer_connection_tracker_->TrackAddIceCandidate(
          this, platform_candidate, PeerConnectionTracker::kSourceLocal,
          true);
    }
    if (!is_closed_ && client_on_stack) {
      client_on_stack->DidGenerateICECandidate(platform_candidate);
    }
  }
}

void RTCPeerConnectionHandler::OnIceCandidateError(const String& address,
                                                   std::optional<uint16_t> port,
                                                   const String& host_candidate,
                                                   const String& url,
                                                   int error_code,
                                                   const String& error_text) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  TRACE_EVENT0("webrtc", "RTCPeerConnectionHandler::OnIceCandidateError");
  if (peer_connection_tracker_) {
    peer_connection_tracker_->TrackIceCandidateError(
        this, address, port, host_candidate, url, error_code, error_text);
  }
  if (!is_closed_) {
    client_->DidFailICECandidate(address, port, host_candidate, url, error_code,
                                 error_text);
  }
}

void RTCPeerConnectionHandler::OnInterestingUsage(int usage_pattern) {
  if (client_)
    client_->DidNoteInterestingUsage(usage_pattern);
}

RTCPeerConnectionHandler::FirstSessionDescription::FirstSessionDescription(
    const webrtc::SessionDescriptionInterface* sdesc) {
  DCHECK(sdesc);

  for (const auto& content : sdesc->description()->contents()) {
    if (content.type == webrtc::MediaProtocolType::kRtp) {
      const auto* mdesc = content.media_description();
      audio = audio || (mdesc->type() == webrtc::MediaType::AUDIO);
      video = video || (mdesc->type() == webrtc::MediaType::VIDEO);
      rtcp_mux = rtcp_mux || mdesc->rtcp_mux();
    }
  }
}

void RTCPeerConnectionHandler::ReportFirstSessionDescriptions(
    const FirstSessionDescription& local,
    const FirstSessionDescription& remote) {
  RtcpMux rtcp_mux = RtcpMux::kEnabled;
  if ((!local.audio && !local.video) || (!remote.audio && !remote.video)) {
    rtcp_mux = RtcpMux::kNoMedia;
  } else if (!local.rtcp_mux || !remote.rtcp_mux) {
    rtcp_mux = RtcpMux::kDisabled;
  }

  UMA_HISTOGRAM_ENUMERATION("WebRTC.PeerConnection.RtcpMux", rtcp_mux,
                            RtcpMux::kMax);

  // TODO(pthatcher): Reports stats about whether we have audio and
  // video or not.
}

Vector<std::unique_ptr<blink::RTCRtpSenderImpl>>::iterator
RTCPeerConnectionHandler::FindSender(uintptr_t id) {
  return std::ranges::find_if(
      rtp_senders_, [id](const auto& sender) { return sender->Id() == id; });
}

Vector<std::unique_ptr<blink::RTCRtpReceiverImpl>>::iterator
RTCPeerConnectionHandler::FindReceiver(uintptr_t id) {
  return std::ranges::find_if(rtp_receivers_, [id](const auto& receiver) {
    return receiver->Id() == id;
  });
}

Vector<std::unique_ptr<blink::RTCRtpTransceiverImpl>>::iterator
RTCPeerConnectionHandler::FindTransceiver(uintptr_t id) {
  return std::ranges::find_if(rtp_transceivers_, [id](const auto& transceiver) {
    return transceiver->Id() == id;
  });
}

wtf_size_t RTCPeerConnectionHandler::GetTransceiverIndex(
    const RTCRtpTransceiverPlatform& platform_transceiver) {
  for (wtf_size_t i = 0; i < rtp_transceivers_.size(); ++i) {
    if (platform_transceiver.Id() == rtp_transceivers_[i]->Id())
      return i;
  }
  NOTREACHED();
}

std::unique_ptr<blink::RTCRtpTransceiverImpl>
RTCPeerConnectionHandler::CreateOrUpdateTransceiver(
    blink::RtpTransceiverState transceiver_state,
    blink::TransceiverStateUpdateMode update_mode) {
  CHECK(dependency_factory_);
  DCHECK(transceiver_state.is_initialized());
  DCHECK(transceiver_state.sender_state());
  DCHECK(transceiver_state.receiver_state());
  auto webrtc_transceiver = transceiver_state.webrtc_transceiver();
  auto webrtc_sender = transceiver_state.sender_state()->webrtc_sender();
  auto webrtc_receiver = transceiver_state.receiver_state()->webrtc_receiver();

  std::unique_ptr<blink::RTCRtpTransceiverImpl> transceiver;
  auto it = FindTransceiver(
      blink::RTCRtpTransceiverImpl::GetId(webrtc_transceiver.get()));
  if (it == rtp_transceivers_.end()) {
    // Create a new transceiver, including a sender and a receiver.
    transceiver = std::make_unique<blink::RTCRtpTransceiverImpl>(
        native_peer_connection_, track_adapter_map_,
        std::move(transceiver_state),
        dependency_factory_->CreateDecodeMetronome());
    rtp_transceivers_.push_back(transceiver->ShallowCopy());
    DCHECK(FindSender(blink::RTCRtpSenderImpl::getId(webrtc_sender.get())) ==
           rtp_senders_.end());
    rtp_senders_.push_back(std::make_unique<blink::RTCRtpSenderImpl>(
        *transceiver->content_sender()));
    DCHECK(FindReceiver(blink::RTCRtpReceiverImpl::getId(
               webrtc_receiver.get())) == rtp_receivers_.end());
    rtp_receivers_.push_back(std::make_unique<blink::RTCRtpReceiverImpl>(
        *transceiver->content_receiver()));
  } else {
    // Update the transceiver. This also updates the sender and receiver.
    transceiver = (*it)->ShallowCopy();
    transceiver->set_state(std::move(transceiver_state), update_mode);
    DCHECK(FindSender(blink::RTCRtpSenderImpl::getId(webrtc_sender.get())) !=
           rtp_senders_.end());
    DCHECK(FindReceiver(blink::RTCRtpReceiverImpl::getId(
               webrtc_receiver.get())) != rtp_receivers_.end());
  }
  return transceiver;
}

scoped_refptr<base::SingleThreadTaskRunner>
RTCPeerConnectionHandler::signaling_thread() const {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  return signaling_thread_;
}

void RTCPeerConnectionHandler::ReportICEState(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (ice_state_seen_[new_state]) {
    return;
  }
  ice_state_seen_[new_state] = true;
  UMA_HISTOGRAM_ENUMERATION("WebRTC.PeerConnection.ConnectionState", new_state,
                            webrtc::PeerConnectionInterface::kIceConnectionMax);
}

}  // namespace blink
