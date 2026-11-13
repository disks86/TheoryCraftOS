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

#include "PipeWire/Rtp.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

namespace rtp {

namespace {
constexpr uint64_t kNtpOffset = 2208988800ull;      // (seconds from 1900 to 1970)
constexpr const char* kSdpType = "application/sdp";

bool parse_number(StringView text, uint64_t limit, uint64_t* out) {
  if (text.empty() || text.size() > 20) return false;
  uint64_t v = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const uint64_t digit = uint64_t(ch - '0');
    if (v > (limit - digit) / 10) return false;      // (would pass the limit)
    v = v * 10 + digit;
  }
  *out = v;
  return true;
}

// The next word of `rest` (separated by spaces), taken off it.
StringView word(StringView* rest) {
  StringView r = *rest;
  while (!r.empty() && r[0] == ' ') r = r.substr(1);
  uint64_t n = 0;
  while (n < r.size() && r[n] != ' ') ++n;
  const StringView w = r.first(n);
  *rest = r.substr(n);
  return w;
}

bool append_number(String* out, uint64_t v) { return out->append_uint(v); }

bool append_address(String* out, network::Ipv4Address a) {
  for (int i = 0; i < 4; ++i) {
    if (i && !out->append(".")) return false;
    if (!out->append_uint(a.octets[i])) return false;
  }
  return true;
}
}  // namespace

bool write_header(const Header& h, ByteView out) {
  if (out.size() < kHeaderBytes) return false;
  ByteWriter w(out);
  w.u8(0x80);      // version 2
  w.u8(static_cast<uint8_t>((h.marker ? 0x80 : 0) | (h.payload_type & 0x7F)));
  w.be16(h.sequence);
  w.be32(h.timestamp);
  w.be32(h.ssrc);
  return w.ok();
}

bool parse_packet(ConstByteView packet, Header* h, ConstByteView* payload) {
  ByteReader r(packet);
  const uint8_t first = r.u8(), second = r.u8();
  h->sequence = r.be16();
  h->timestamp = r.be32();
  h->ssrc = r.be32();
  if (!r.ok() || (first >> 6) != 2) return false;
  h->marker = (second & 0x80) != 0;
  h->payload_type = second & 0x7F;
  if (!r.skip(uint64_t(first & 0x0F) * 4)) return false;      // (contributing sources)
  if (first & 0x10) {                                         // (a header extension: 16 bits of profile, 16 of length in words)
    r.be16();
    const uint16_t words = r.be16();
    if (!r.ok() || !r.skip(uint64_t(words) * 4)) return false;
  }
  ConstByteView rest = r.rest();
  if (first & 0x20) {      // (padding: its last byte says how much)
    if (rest.empty()) return false;
    const uint8_t pad = rest[rest.size() - 1];
    if (pad == 0 || pad > rest.size()) return false;
    rest = rest.first(rest.size() - pad);
  }
  *payload = rest;
  return true;
}

uint8_t payload_type_for(StringView encoding, uint32_t rate, uint32_t channels) {
  if (encoding == StringView("L16") && rate == 44100) return channels == 2 ? 10 : channels == 1 ? 11 : kDynamicPayload;
  if (encoding == StringView("PCMU") && rate == 8000 && channels == 1) return 0;
  if (encoding == StringView("PCMA") && rate == 8000 && channels == 1) return 8;
  return kDynamicPayload;
}

bool pcm_format_of(StringView encoding, uint32_t rate, uint32_t channels, PcmFormat* out) {
  SampleFormat f;
  if (encoding == StringView("L16")) f = SampleFormat::S16Be;
  else if (encoding == StringView("PCMU")) f = SampleFormat::Ulaw;
  else if (encoding == StringView("PCMA")) f = SampleFormat::Alaw;
  else return false;
  const PcmFormat format{f, rate, channels};
  if (!format.valid()) return false;
  *out = format;
  return true;
}

bool make_sdp(const Session& s, uint64_t ntp_seconds, String* out) {
  out->clear();
  bool ok = out->append("v=0\no=") && out->append(s.user.empty() ? StringView("-") : s.user.view()) && out->append(" ") &&
            append_number(out, s.session_id) && out->append(" 0 IN IP4 ") && append_address(out, s.origin) && out->append("\ns=") &&
            out->append(s.name.view()) && out->append("\nc=IN IP4 ") && append_address(out, s.address) && out->append("\nt=") &&
            append_number(out, ntp_seconds) && out->append(" 0\na=recvonly\nm=audio ") && append_number(out, s.port) && out->append(" RTP/AVP ") &&
            append_number(out, s.payload_type) && out->append("\na=rtpmap:") && append_number(out, s.payload_type) && out->append(" ") &&
            out->append(s.encoding.view()) && out->append("/") && append_number(out, s.rate) && out->append("/") && append_number(out, s.channels) &&
            out->append("\na=type:broadcast\n");
  return ok;
}

bool parse_sdp(StringView text, Session* out) {
  if (text.size() > 8192) return false;
  bool have_c = false, have_m = false, have_map = false, version = false;
  uint64_t rtpmap_type = 256;
  StringView rest = text;
  while (!rest.empty()) {
    uint64_t end = rest.find('\n');
    StringView line = end == StringView::npos ? rest : rest.first(end);
    rest = end == StringView::npos ? StringView() : rest.substr(end + 1);
    if (!line.empty() && line[line.size() - 1] == '\r') line = line.first(line.size() - 1);
    if (line.size() < 2 || line[1] != '=') continue;
    const char kind = line[0];
    StringView value = line.substr(2);
    if (kind == 'v') {
      version = value == StringView("0");
    } else if (kind == 'o') {
      StringView v = value;
      const StringView user = word(&v), id = word(&v);
      word(&v);      // version
      if (word(&v) != StringView("IN") || word(&v) != StringView("IP4")) return false;
      uint64_t session = 0;
      if (!out->user.assign(user) || !parse_number(id, ~uint64_t{0}, &session) || !parse_address(word(&v), &out->origin)) return false;
      out->session_id = session;
    } else if (kind == 's') {
      if (!out->name.assign(value)) return false;
    } else if (kind == 'c') {
      StringView v = value;
      if (word(&v) != StringView("IN") || word(&v) != StringView("IP4")) return false;
      StringView address = word(&v);
      const uint64_t slash = address.find('/');      // (a multicast address's TTL)
      if (slash != StringView::npos) address = address.first(slash);
      if (!parse_address(address, &out->address)) return false;
      have_c = true;
    } else if (kind == 'm') {
      StringView v = value;
      if (word(&v) != StringView("audio")) continue;      // (another medium: not ours)
      uint64_t port = 0, type = 0;
      if (!parse_number(word(&v), 65535, &port) || port == 0 || word(&v) != StringView("RTP/AVP") || !parse_number(word(&v), 127, &type)) return false;
      out->port = static_cast<uint16_t>(port);
      out->payload_type = static_cast<uint8_t>(type);
      have_m = true;
    } else if (kind == 'a' && value.size() > 7 && value.first(7) == StringView("rtpmap:")) {
      StringView v = value.substr(7);
      if (!parse_number(word(&v), 127, &rtpmap_type)) return false;
      StringView map = word(&v);
      const uint64_t a = map.find('/');
      if (a == StringView::npos) return false;
      const StringView encoding = map.first(a);
      StringView after = map.substr(a + 1);
      const uint64_t b = after.find('/');
      uint64_t rate = 0, channels = 1;
      if (!parse_number(b == StringView::npos ? after : after.first(b), 384000, &rate)) return false;
      if (b != StringView::npos && !parse_number(after.substr(b + 1), 64, &channels)) return false;
      if (!out->encoding.assign(encoding)) return false;
      out->rate = static_cast<uint32_t>(rate);
      out->channels = static_cast<uint32_t>(channels);
      have_map = true;
    }
  }
  if (!version || !have_c || !have_m) return false;
  if (have_map && rtpmap_type != out->payload_type) return false;      // (the map is for another payload)
  if (!have_map) {
    // a static payload type says its format itself (RFC 3551)
    const struct {
      uint8_t type;
      const char* encoding;
      uint32_t rate, channels;
    } kStatic[] = {{0, "PCMU", 8000, 1}, {8, "PCMA", 8000, 1}, {10, "L16", 44100, 2}, {11, "L16", 44100, 1}};
    bool known = false;
    for (const auto& s : kStatic)
      if (s.type == out->payload_type) {
        if (!out->encoding.assign(s.encoding)) return false;
        out->rate = s.rate;
        out->channels = s.channels;
        known = true;
      }
    if (!known) return false;
  }
  PcmFormat f;
  return pcm_format_of(out->encoding.view(), out->rate, out->channels, &f);
}

bool make_sap(StringView sdp, network::Ipv4Address origin, uint16_t message_hash, bool deletion, ByteBuffer* out) {
  out->clear();
  const uint8_t head[4] = {static_cast<uint8_t>(0x20 | (deletion ? 0x04 : 0)), 0, static_cast<uint8_t>(message_hash >> 8), static_cast<uint8_t>(message_hash)};
  return out->append(ConstByteView(head, 4)) && out->append(ConstByteView(origin.octets)) && out->append(StringView(kSdpType).bytes()) &&
         out->put8(0) && out->append(sdp.bytes());
}

bool parse_sap(ConstByteView packet, bool* deletion, uint16_t* message_hash, network::Ipv4Address* origin, StringView* sdp) {
  ByteReader r(packet);
  const uint8_t flags = r.u8(), auth_words = r.u8();
  *message_hash = r.be16();
  if (!r.ok() || (flags >> 5) != 1) return false;      // (version 1)
  if (flags & 0x10) return false;                      // (an IPv6 origin)
  if (flags & 0x03) return false;                      // (encrypted or compressed)
  *deletion = (flags & 0x04) != 0;
  *origin = network::Ipv4Address::read(r);
  if (!r.ok() || !r.skip(uint64_t(auth_words) * 4)) return false;
  ConstByteView rest = r.rest();
  // the payload type, unless the payload starts at once with the description (RFC 2974: then it is SDP)
  const StringView text(rest);
  if (!(text.size() >= 3 && text.first(3) == StringView("v=0"))) {
    uint64_t nul = 0;
    while (nul < rest.size() && rest[nul] != 0) ++nul;
    if (nul >= rest.size() || StringView(rest.first(nul)) != StringView(kSdpType)) return false;
    rest = rest.subview(nul + 1);
  }
  *sdp = StringView(rest);
  return true;
}

bool parse_address(StringView text, network::Ipv4Address* out) {
  network::Ipv4Address a{};
  StringView rest = text;
  for (int i = 0; i < 4; ++i) {
    const uint64_t dot = rest.find('.');
    if ((i < 3) != (dot != StringView::npos)) return false;
    const StringView part = i < 3 ? rest.first(dot) : rest;
    uint64_t v = 0;
    if (part.empty() || part.size() > 3 || !parse_number(part, 255, &v)) return false;
    a.octets[i] = static_cast<uint8_t>(v);
    rest = i < 3 ? rest.substr(dot + 1) : StringView();
  }
  *out = a;
  return true;
}

}  // namespace rtp

// ---- the sender -------------------------------------------------------------------------------------------------------------------------------------

RtpSender::RtpSender(Graph& graph, IRtpNetwork& network, allocators::IAllocator& allocator)
    : graph_(graph), network_(network), allocator_(allocator), pending_(allocator), packet_(allocator), sap_(allocator), sdp_(allocator) {}

bool RtpSender::open(uint32_t client, Loopback::From from, const Options& o, uint32_t seed) {
  close();
  PcmFormat format;
  if (!rtp::pcm_format_of(o.encoding, o.rate, o.channels, &format) || o.port == 0 || o.mtu < rtp::kHeaderBytes + 64 || o.mtu > 9000) return false;
  frame_bytes_ = format.frame_size();
  payload_bytes_ = (o.mtu - rtp::kHeaderBytes) / frame_bytes_ * frame_bytes_;
  destination_ = o.destination;
  port_ = o.port;
  // (PulseAudio's: random sequence, timestamp and source starts)
  uint32_t state = seed ? seed : 0x2545F491u;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  header_.payload_type = rtp::payload_type_for(o.encoding, o.rate, o.channels);
  header_.sequence = static_cast<uint16_t>(next());
  header_.timestamp = next();
  header_.ssrc = next();
  message_hash_ = static_cast<uint16_t>(next());
  socket_ = network_.open(0);
  sap_socket_ = network_.open(0);
  if (!socket_ || !sap_socket_) {
    close();
    return false;
  }
  Properties props(allocator_);
  props.set("application.name", "RTP");
  props.set("media.name", o.stream_name.empty() ? StringView("RTP stream") : o.stream_name);
  props.set("node.name", "rtp-send");
  props.set("state.restore", "false");
  record_node_ = graph_.create_record(client, format, props, &record_, from.monitor, from.monitor ? from.sink : 0);
  if (!record_node_ || !record_) {
    close();
    return false;
  }
  // the description it announces
  rtp::Session s(allocator_);
  s.name.assign(o.stream_name.empty() ? StringView("TheoryCraftOS RTP Stream") : o.stream_name);
  s.user.assign("-");
  s.session_id = header_.ssrc;
  s.origin = network_.local_address();
  s.address = destination_;
  s.port = port_;
  s.payload_type = header_.payload_type;
  s.encoding.assign(o.encoding);
  s.rate = o.rate;
  s.channels = o.channels;
  if (!rtp::make_sdp(s, rtp::kNtpOffset + graph_.now_ns() / 1000000000ull, &sdp_)) {
    close();
    return false;
  }
  next_announce_ = 0;
  packets_ = 0;
  pending_.clear();
  return true;
}

void RtpSender::close() {
  // (the stream withdrawn: a SAP deletion)
  if (announced_ && sap_socket_ && rtp::make_sap(sdp_.view(), network_.local_address(), message_hash_, true, &sap_))
    sap_socket_->send_to(destination_, rtp::kSapPort, sap_.view());
  announced_ = false;
  if (record_node_) graph_.destroy_node(record_node_);
  record_node_ = 0;
  record_ = SharedPointer<RecordStream>();
  if (socket_) socket_->close();
  if (sap_socket_) sap_socket_->close();
  socket_ = sap_socket_ = SharedPointer<network::IUdpSocket>();
}

uint32_t RtpSender::pump(uint64_t now_ns) {
  if (!record_ || !socket_) return 0;
  if (now_ns >= next_announce_ && rtp::make_sap(sdp_.view(), network_.local_address(), message_hash_, false, &sap_)) {
    sap_socket_->send_to(destination_, rtp::kSapPort, sap_.view());      // (to the stream's own address, as PulseAudio sends it)
    announced_ = true;
    next_announce_ = now_ns + kAnnounceNs;
  }
  // what was recorded, in whole packets (the rest waits for the next round)
  uint32_t sent = 0;
  for (int rounds = 0; rounds < 64; ++rounds) {
    const uint64_t have = pending_.size();
    if (have < payload_bytes_) {
      const uint64_t want = payload_bytes_ * 16 - have;
      const uint64_t available = record_->available_bytes();
      if (available == 0) break;
      const uint64_t take = available < want ? available : want;
      if (!pending_.put_zeros(take)) break;
      const uint32_t got = record_->read(pending_.writable_view().subview(have, take));
      pending_.truncate(have + got);
      if (pending_.size() < payload_bytes_) break;
    }
    // one packet from the front
    packet_.clear();
    if (!packet_.put_zeros(rtp::kHeaderBytes)) break;
    if (!rtp::write_header(header_, packet_.writable_view()) || !packet_.append(pending_.view().first(payload_bytes_))) break;
    if (!socket_->send_to(destination_, port_, packet_.view())) break;      // (the network is full: it goes again next round)
    ++header_.sequence;
    header_.timestamp += payload_bytes_ / frame_bytes_;
    ++sent;
    ++packets_;
    // (what is left moved to the front)
    const uint64_t left = pending_.size() - payload_bytes_;
    ByteView p = pending_.writable_view();
    for (uint64_t i = 0; i < left; ++i) p[i] = p[payload_bytes_ + i];
    pending_.truncate(left);
  }
  return sent;
}

// ---- the receiver -----------------------------------------------------------------------------------------------------------------------------------

RtpReceiver::RtpReceiver(Graph& graph, IRtpNetwork& network, allocators::IAllocator& allocator)
    : graph_(graph), network_(network), allocator_(allocator), sessions_(allocator), buffer_(allocator) {}

bool RtpReceiver::open(uint32_t client, uint32_t sink, network::Ipv4Address sap_address, uint32_t latency_ms) {
  close();
  client_ = client;
  sink_ = sink;
  sap_address_ = sap_address;
  latency_ms_ = latency_ms ? latency_ms : 500;
  sap_socket_ = network_.open(rtp::kSapPort);
  if (!sap_socket_) return false;
  sap_joined_ = sap_address.is_multicast() && network_.join(sap_address);
  return buffer_.put_zeros(65536);
}

void RtpReceiver::close() {
  while (sessions_.count()) drop(sessions_.count() - 1);
  if (sap_joined_) {
    sap_joined_ = false;
    network_.leave(sap_address_);
  }
  if (sap_socket_) sap_socket_->close();
  sap_socket_ = SharedPointer<network::IUdpSocket>();
}

uint32_t RtpReceiver::session_node(uint32_t index) const { return index < sessions_.count() ? sessions_[index]->node : 0; }

bool RtpReceiver::group_needed(network::Ipv4Address group, uint64_t except) const {
  // (the network counts a group joined twice as joined once: it is left only when nothing else here listens on it)
  if (sap_joined_ && group == sap_address_) return true;
  for (uint64_t k = 0; k < sessions_.count(); ++k)
    if (k != except && sessions_[k]->joined && sessions_[k]->session.address == group) return true;
  return false;
}

void RtpReceiver::drop(uint64_t index) {
  Stream& s = *sessions_[index];
  if (s.node) graph_.destroy_node(s.node);
  if (s.socket) s.socket->close();
  if (s.joined && !group_needed(s.session.address, index)) network_.leave(s.session.address);
  sessions_.remove_at(index);
}

void RtpReceiver::announced(ConstByteView packet, uint64_t now_ns) {
  bool deletion = false;
  uint16_t hash = 0;
  network::Ipv4Address origin;
  StringView sdp;
  if (!rtp::parse_sap(packet, &deletion, &hash, &origin, &sdp)) return;
  for (uint64_t k = 0; k < sessions_.count(); ++k) {
    if (sessions_[k]->hash != hash || sessions_[k]->origin != origin) continue;
    if (deletion) drop(k);      // (the stream ended)
    else sessions_[k]->last_heard = now_ns;
    return;
  }
  if (deletion || sessions_.count() >= kMaxSessions) return;
  UniquePointer<Stream> s = UniquePointer<Stream>::make(allocator_, allocator_);
  if (!s || !rtp::parse_sdp(sdp, &s->session) || !rtp::pcm_format_of(s->session.encoding.view(), s->session.rate, s->session.channels, &s->format)) return;
  for (const UniquePointer<Stream>& other : sessions_.view())
    if (other->session.port == s->session.port) return;      // (one stream a port)
  s->hash = hash;
  s->origin = origin;
  s->last_heard = now_ns;
  s->socket = network_.open(s->session.port);
  if (!s->socket) return;
  s->joined = s->session.address.is_multicast() && network_.join(s->session.address);
  Properties props(allocator_);
  props.set("application.name", "RTP");
  props.set("media.name", s->session.name.empty() ? StringView("RTP stream") : s->session.name.view());
  props.set("node.name", "rtp-recv");
  props.set("state.restore", "false");
  s->node = graph_.create_playback(client_, s->format, props, &s->play, sink_);
  if (!s->node || !s->play) {
    if (s->socket) s->socket->close();
    if (s->joined && !group_needed(s->session.address, sessions_.count())) network_.leave(s->session.address);
    return;
  }
  sessions_.push_back(util::move(s));
}

uint32_t RtpReceiver::pump(uint64_t now_ns) {
  if (!sap_socket_) return 0;
  network::Ipv4Address from;
  uint16_t port = 0;
  uint32_t length = 0;
  for (int k = 0; k < 16 && sap_socket_->receive_from(buffer_.writable_view(), &from, &port, &length); ++k)
    if (length <= buffer_.size()) announced(buffer_.view().first(length), now_ns);
  // the sessions not announced for a while are gone
  for (uint64_t k = sessions_.count(); k > 0; --k)
    if (now_ns > sessions_[k - 1]->last_heard + kSessionTimeoutNs) drop(k - 1);
  uint32_t taken = 0;
  for (UniquePointer<Stream>& sp : sessions_.view()) {
    Stream& s = *sp;
    const uint64_t frame = s.format.frame_size();
    const uint64_t latency_bytes = uint64_t(s.format.rate) * latency_ms_ / 1000 * frame;
    for (int k = 0; k < 256 && s.socket->receive_from(buffer_.writable_view(), &from, &port, &length); ++k) {
      if (length > buffer_.size()) continue;
      rtp::Header h;
      ConstByteView payload;
      if (!rtp::parse_packet(buffer_.view().first(length), &h, &payload) || h.payload_type != s.session.payload_type) continue;
      payload = payload.first(payload.size() / frame * frame);
      if (payload.empty()) continue;
      if (!s.started || h.ssrc != s.ssrc) {      // (the first packet, or the sender started again)
        s.started = true;
        s.ssrc = h.ssrc;
        s.next_sequence = h.sequence;
      }
      const uint16_t ahead = static_cast<uint16_t>(h.sequence - s.next_sequence);
      if (ahead >= 0x8000) continue;      // (late, or twice: already played)
      if (ahead > kMaxGapPackets) {
        s.next_sequence = h.sequence;      // (a jump: taken as a new start)
      } else if (ahead > 0) {
        // (lost on the way: silence where they were, as long as what is queued leaves room)
        lost_ += ahead;
        const uint64_t gap = uint64_t(ahead) * payload.size();
        if (s.play->waiting_bytes() + gap < 2 * latency_bytes) {
          ByteBuffer silence(allocator_);
          const uint8_t zero = s.format.format == SampleFormat::Ulaw ? 0xFF : s.format.format == SampleFormat::Alaw ? 0xD5 : 0x00;
          if (silence.put_zeros(gap)) {
            if (zero) for (uint8_t& b : silence.writable_view()) b = zero;
            s.play->write(silence.view());
          }
        }
      }
      s.next_sequence = static_cast<uint16_t>(h.sequence + 1);
      // (more queued than twice the latency: the sender's clock runs ahead of the speakers'; the packet goes)
      if (s.play->waiting_bytes() < 2 * latency_bytes) s.play->write(payload);
      ++taken;
    }
  }
  return taken;
}

}  // namespace pipewire
