/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Audio over the network as PulseAudio's module-rtp-send and module-rtp-recv carry it (and PipeWire's RTP modules, which speak the same):
//   - RTP (RFC 3550) packets of uncompressed audio: L16 (16-bit big-endian, RFC 3551), PCMU (mu-law) or PCMA (A-law), the payload type the
//     static one when the format has one (L16 44.1 kHz stereo 10, mono 11; PCMU 8 kHz mono 0; PCMA 8 kHz mono 8), else the dynamic 127;
//   - each stream announced with SAP (RFC 2974) every 5 seconds on 224.0.0.56 port 9875 (and withdrawn when it ends), the announcement an SDP
//     (RFC 4566) description: the stream's address, port, payload type, encoding, rate and channels.
// RtpSender records a source (or a sink's monitor) and sends it; RtpReceiver listens for announcements and plays each announced stream into a sink,
// keeping about `latency` queued (packets that come late or twice are dropped, a few lost ones replaced by silence). Both are pumped by their
// owner's thread (PulseServer::tick); the network is IRtpNetwork (the kernel's network stack, or the tests' own). IPv4 only; no Opus, no RTCP.
#pragma once
#include <stdint.h>
#include "Network/NetworkTypes.hpp"
#include "Network/Udp/IUdpSocket.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/Loopback.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace pipewire {

// The network the RTP modules use.
class IRtpNetwork {
 public:
  // A UDP socket on `port` (null when it cannot be had).
  virtual SharedPointer<network::IUdpSocket> open(uint16_t port) = 0;
  // Multicast reception of `group` (false: not on this network).
  virtual bool join(network::Ipv4Address group) = 0;
  virtual void leave(network::Ipv4Address group) = 0;
  // This machine's address (an announcement's origin; 0.0.0.0 when it has none).
  virtual network::Ipv4Address local_address() = 0;

 protected:
  constexpr IRtpNetwork() = default;
  ~IRtpNetwork() = default;
};

namespace rtp {

constexpr uint16_t kSapPort = 9875;
constexpr uint16_t kDefaultPort = 46000;
constexpr uint32_t kDefaultMtu = 1280;
constexpr uint32_t kHeaderBytes = 12;
constexpr network::Ipv4Address kSapAddress = network::Ipv4Address::make(224, 0, 0, 56);
constexpr uint8_t kDynamicPayload = 127;

struct Header {
  uint8_t payload_type = 0;
  bool marker = false;
  uint16_t sequence = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
};
// Writes the 12 bytes of a packet's header (version 2, no padding, extension or contributing sources).
[[nodiscard]] bool write_header(const Header& h, ByteView out);
// A packet's header and payload (its contributing sources, extension and padding stepped over); false for one that is not RTP version 2 or is
// cut short.
[[nodiscard]] bool parse_packet(ConstByteView packet, Header* h, ConstByteView* payload);

// What a stream is, as SDP describes it.
struct Session {
  explicit Session(allocators::IAllocator& a) : name(a), user(a), encoding(a) {}
  String name;                          // s=
  String user;                          // o='s user name
  uint64_t session_id = 0;              // o='s session id
  network::Ipv4Address origin = network::kAnyAddress;      // o='s address
  network::Ipv4Address address = network::kAnyAddress;     // c=
  uint16_t port = 0;                    // m=
  uint8_t payload_type = 0;             // m=
  String encoding;                      // a=rtpmap: L16, PCMU, PCMA
  uint32_t rate = 0, channels = 0;      // a=rtpmap: (channels 1 when it says none)
};
// The payload type a format has (a static one where RFC 3551 gives one, else kDynamicPayload), and the graph format of an encoding (false for one
// that is not L16, PCMU or PCMA, or a rate or channel count out of range).
uint8_t payload_type_for(StringView encoding, uint32_t rate, uint32_t channels);
[[nodiscard]] bool pcm_format_of(StringView encoding, uint32_t rate, uint32_t channels, PcmFormat* out);
// The SDP text for a session; and back (false for a description without an audio stream the receiver can play: no c= or m=, another transport,
// an encoding it does not know).
[[nodiscard]] bool make_sdp(const Session& s, uint64_t ntp_seconds, String* out);
[[nodiscard]] bool parse_sdp(StringView text, Session* out);
// A SAP packet (version 1, IPv4 origin, no authentication or encryption) carrying `sdp`; and back. `deletion`: the stream ends.
[[nodiscard]] bool make_sap(StringView sdp, network::Ipv4Address origin, uint16_t message_hash, bool deletion, ByteBuffer* out);
[[nodiscard]] bool parse_sap(ConstByteView packet, bool* deletion, uint16_t* message_hash, network::Ipv4Address* origin, StringView* sdp);
// "a.b.c.d" (false for anything else).
[[nodiscard]] bool parse_address(StringView text, network::Ipv4Address* out);

}  // namespace rtp

class RtpSender {
 public:
  static constexpr uint64_t kAnnounceNs = 5000000000ull;      // (SAP every 5 s)

  RtpSender(Graph& graph, IRtpNetwork& network, allocators::IAllocator& allocator);
  ~RtpSender() { close(); }
  RtpSender(const RtpSender&) = delete;
  RtpSender& operator=(const RtpSender&) = delete;

  struct Options {
    StringView encoding = "L16";      // (or PCMU, PCMA)
    uint32_t rate = 48000, channels = 2;
    network::Ipv4Address destination = rtp::kSapAddress;
    uint16_t port = rtp::kDefaultPort;
    uint32_t mtu = rtp::kDefaultMtu;
    StringView stream_name;
  };
  // Records `from` (a source or a sink's monitor) for the network. False when the format, MTU or port will not do, or the streams or socket
  // cannot be had.
  [[nodiscard]] bool open(uint32_t client, Loopback::From from, const Options& options, uint32_t random_seed);
  void close();
  // Sends what was recorded (whole packets) and the announcement when it is due. Returns the packets sent.
  uint32_t pump(uint64_t now_ns);
  uint64_t packets() const { return packets_; }
  uint32_t ssrc() const { return header_.ssrc; }
  uint8_t payload_type() const { return header_.payload_type; }
  const String& sdp() const { return sdp_; }

 private:
  Graph& graph_;
  IRtpNetwork& network_;
  allocators::IAllocator& allocator_;
  SharedPointer<RecordStream> record_;
  uint32_t record_node_ = 0;
  SharedPointer<network::IUdpSocket> socket_, sap_socket_;
  network::Ipv4Address destination_ = rtp::kSapAddress;
  uint16_t port_ = 0;
  uint32_t payload_bytes_ = 0, frame_bytes_ = 0;
  rtp::Header header_;
  ByteBuffer pending_, packet_, sap_;
  String sdp_;
  uint16_t message_hash_ = 0;
  uint64_t next_announce_ = 0, packets_ = 0;
  bool announced_ = false;
};

class RtpReceiver {
 public:
  static constexpr uint32_t kMaxSessions = 8;
  static constexpr uint64_t kSessionTimeoutNs = 20000000000ull;      // (no announcement for this long: the stream is gone)
  static constexpr uint32_t kMaxGapPackets = 64;                     // (more lost than this at once: a new start, not silence)

  RtpReceiver(Graph& graph, IRtpNetwork& network, allocators::IAllocator& allocator);
  ~RtpReceiver() { close(); }
  RtpReceiver(const RtpReceiver&) = delete;
  RtpReceiver& operator=(const RtpReceiver&) = delete;

  // Listens for announcements on `sap_address` (multicast: joined) and plays what they announce into `sink` (0: the speakers).
  [[nodiscard]] bool open(uint32_t client, uint32_t sink, network::Ipv4Address sap_address, uint32_t latency_ms);
  void close();
  uint32_t pump(uint64_t now_ns);      // (the packets taken)
  uint32_t session_count() const { return static_cast<uint32_t>(sessions_.count()); }
  // A session's playback node (0 past the last), for tests and listings.
  uint32_t session_node(uint32_t index) const;
  uint64_t lost() const { return lost_; }

 private:
  struct Stream {
    explicit Stream(allocators::IAllocator& a) : session(a) {}
    rtp::Session session;
    uint16_t hash = 0;
    network::Ipv4Address origin = network::kAnyAddress;
    SharedPointer<network::IUdpSocket> socket;
    SharedPointer<PlaybackStream> play;
    uint32_t node = 0;
    PcmFormat format;
    bool started = false, joined = false;
    uint16_t next_sequence = 0;
    uint32_t ssrc = 0;
    uint64_t last_heard = 0;
  };
  void drop(uint64_t index);
  bool group_needed(network::Ipv4Address group, uint64_t except) const;
  void announced(ConstByteView packet, uint64_t now_ns);

  Graph& graph_;
  IRtpNetwork& network_;
  allocators::IAllocator& allocator_;
  uint32_t client_ = 0, sink_ = 0, latency_ms_ = 500;
  network::Ipv4Address sap_address_ = rtp::kSapAddress;
  bool sap_joined_ = false;
  SharedPointer<network::IUdpSocket> sap_socket_;
  List<UniquePointer<Stream>> sessions_;
  ByteBuffer buffer_;
  uint64_t lost_ = 0;
};

}  // namespace pipewire
