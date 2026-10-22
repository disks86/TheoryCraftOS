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

#include "Network/Quic/Http3.hpp"
#include "Utility/ByteStream.hpp"

namespace network {

namespace {

constexpr uint64_t kNoError = 0x100, kGeneralProtocolError = 0x101, kInternalError = 0x102, kStreamCreationError = 0x103, kClosedCriticalStream = 0x104,
                   kFrameUnexpected = 0x105, kFrameError = 0x106, kMissingSettings = 0x10a, kRequestRejected = 0x10b, kMessageError = 0x10e,
                   kQpackDecompressionFailed = 0x200;

constexpr uint64_t kFrameData = 0x00, kFrameHeaders = 0x01, kFrameCancelPush = 0x03, kFrameSettings = 0x04, kFramePushPromise = 0x05, kFrameGoaway = 0x07,
                   kFrameMaxPushId = 0x0d;

QuicConfig with_h3(const QuicConfig& config) {
  QuicConfig copy = config;
  copy.tls.alpn = "h3";
  if (copy.max_incoming_streams < 8) copy.max_incoming_streams = 8;
  return copy;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool append_varint(ByteBuffer* out, uint64_t v) {
  uint8_t bytes[8];
  ByteWriter w{ByteView(bytes)};
  quic::write_varint(w, v);
  return out->append(ConstByteView(bytes, quic::varint_size(v)));
}

}  // namespace

const char* http3_error_name(Http3Error error) {
  switch (error) {
    case Http3Error::None: return "none";
    case Http3Error::Connection: return "QUIC connection failed";
    case Http3Error::Protocol: return "HTTP/3 protocol error";
    case Http3Error::Reset: return "request stream reset";
    case Http3Error::Refused: return "request refused";
    case Http3Error::TooLarge: return "headers too large";
    case Http3Error::Internal: return "out of memory";
    case Http3Error::Cancelled: return "cancelled";
  }
  return "?";
}

Http3Exchange::Http3Exchange(INetworkStack& stack, allocators::IAllocator& allocator, const QuicConfig& config)
    : config_(with_h3(config)),
      connection_(stack, allocator, config_),
      allocator_(allocator),
      request_head_(allocator),
      control_(allocator),
      section_(allocator),
      response_(allocator),
      scratch_(allocator),
      body_(allocator) {}

bool Http3Exchange::start(Ipv4Address address, uint16_t port, StringView server_name, const h3::HeaderList& request, ConstByteView body) {
  request_head_.clear();
  ByteBuffer block(allocator_);
  if (!h3::qpack_encode(request, &block)) {
    fail(Http3Error::Internal);
    return false;
  }
  if (!append_varint(&request_head_, kFrameHeaders) || !append_varint(&request_head_, block.size()) || !request_head_.append(block.view())) {
    fail(Http3Error::Internal);
    return false;
  }
  if (!body.empty() && (!append_varint(&request_head_, kFrameData) || !append_varint(&request_head_, body.size()))) {
    fail(Http3Error::Internal);
    return false;
  }
  request_body_ = body;
  request_total_ = request_head_.size() + body.size();
  if (!connection_.connect(address, port, server_name)) {
    fail(Http3Error::Connection);
    return false;
  }
  phase_ = Http3Phase::Connecting;
  return true;
}

void Http3Exchange::fail(Http3Error error) {
  if (phase_ == Http3Phase::Done || phase_ == Http3Phase::Failed) return;
  error_ = error;
  phase_ = Http3Phase::Failed;
  if (!closed_ && connection_.established()) {
    closed_ = true;
    connection_.close(error == Http3Error::Cancelled ? 0x10c : kInternalError);
    connection_.poll();
  }
}

void Http3Exchange::h3_fail(uint64_t code, Http3Error error) {
  if (phase_ == Http3Phase::Done || phase_ == Http3Phase::Failed) return;
  h3_code_ = code;
  error_ = error;
  phase_ = Http3Phase::Failed;
  if (!closed_) {
    closed_ = true;
    connection_.close(code);
    connection_.poll();
  }
}

void Http3Exchange::cancel() { fail(Http3Error::Cancelled); }

void Http3Exchange::complete() {
  phase_ = Http3Phase::Done;
  if (!closed_) {
    closed_ = true;
    connection_.close(kNoError);
    connection_.poll();
  }
}

void Http3Exchange::consume_body(uint64_t bytes) {
  body_pos_ += bytes;
  if (body_pos_ >= body_.size()) {
    body_.clear();
    body_pos_ = 0;
  }
}

// A variable-length integer read a byte at a time, so that one split between packets is picked up where it stopped. False if more bytes are needed.
bool Http3Exchange::read_varint(uint64_t stream, Varint* state, uint64_t* out) {
  for (;;) {
    uint8_t b;
    if (connection_.read_stream(stream, ByteView(&b, 1)) == 0) return false;
    if (state->got == 0) {
      state->length = static_cast<uint8_t>(1u << (b >> 6));
      state->value = b & 0x3F;
    } else {
      state->value = (state->value << 8) | b;
    }
    if (++state->got == state->length) {
      *out = state->value;
      *state = Varint();
      return true;
    }
  }
}

void Http3Exchange::set_up() {
  set_up_ = true;
  // control stream: type 0, SETTINGS { QPACK_MAX_TABLE_CAPACITY 0, QPACK_BLOCKED_STREAMS 0 }; the QPACK encoder (2) and decoder (3) streams
  static const uint8_t kControl[] = {0x00, 0x04, 0x04, 0x01, 0x00, 0x07, 0x00};
  static const uint8_t kEncoder[] = {0x02}, kDecoder[] = {0x03};
  uint64_t id = 0;
  if (!connection_.open_stream(false, &id) || connection_.write_stream(id, ConstByteView(kControl), false) != sizeof kControl) return h3_fail(kInternalError, Http3Error::Internal);
  if (!connection_.open_stream(false, &id) || connection_.write_stream(id, ConstByteView(kEncoder), false) != 1) return h3_fail(kInternalError, Http3Error::Internal);
  if (!connection_.open_stream(false, &id) || connection_.write_stream(id, ConstByteView(kDecoder), false) != 1) return h3_fail(kInternalError, Http3Error::Internal);
  if (!connection_.open_stream(true, &request_stream_)) return h3_fail(kInternalError, Http3Error::Internal);
}

bool Http3Exchange::flush_request() {
  while (request_sent_ < request_total_) {
    const bool in_head = request_sent_ < request_head_.size();
    const ConstByteView part = in_head ? request_head_.view().subview(request_sent_) : request_body_.subview(request_sent_ - request_head_.size());
    // (The head and the body are written separately; the stream is finished with the last bytes of the body, or of the head when there is none.)
    const bool last = in_head ? request_body_.empty() : true;
    const uint64_t n = connection_.write_stream(request_stream_, part, last);
    request_sent_ += n;
    if (n < part.size()) return true;   // the send buffer is full; the rest goes later
  }
  request_done_ = true;
  return true;
}

void Http3Exchange::service_peer_streams() {
  uint64_t id;
  while (connection_.accept_stream(&id)) {
    if (!(id & 2)) {   // a bidirectional stream of the server's: not allowed (it has no stream credit), and nothing here could answer it
      connection_.reset_stream(id, kStreamCreationError);
      continue;
    }
    if (peer_count_ == kPeerStreams) {
      connection_.stop_sending(id, kStreamCreationError);
      continue;
    }
    peers_[peer_count_] = PeerStream();
    peers_[peer_count_++].id = id;
  }
  for (uint32_t i = 0; i < peer_count_ && phase_ == Http3Phase::Requesting; ++i) {
    PeerStream& p = peers_[i];
    if (p.type < 0) {
      uint64_t type;
      if (!read_varint(p.id, &p.partial, &type)) continue;
      p.type = static_cast<int64_t>(type);
      if (type == 0) {
        if (control_seen_) return h3_fail(kStreamCreationError, Http3Error::Protocol);
        control_seen_ = true;
      } else if (type == 1) {   // a push stream, though pushes were never allowed
        return h3_fail(0x108, Http3Error::Protocol);
      } else if (type != 2 && type != 3) {
        connection_.stop_sending(p.id, kStreamCreationError);   // unknown types are not read
      }
    }
    if (p.type == 0) {
      read_control();
    } else if (p.type == 2 || p.type == 3) {
      // QPACK instructions: with capacity 0 there are none that matter (an insert would be an error); drain them
      uint8_t sink[256];
      while (connection_.read_stream(p.id, ByteView(sink, sizeof sink)) > 0) {
      }
    }
  }
}

void Http3Exchange::read_control() {
  // find the control stream's id
  uint64_t id = 0;
  for (uint32_t i = 0; i < peer_count_; ++i)
    if (peers_[i].type == 0) id = peers_[i].id;
  uint8_t buffer[512];
  for (uint64_t n; (n = connection_.read_stream(id, ByteView(buffer, sizeof buffer))) > 0;) {
    if (control_.size() + n > 64 * 1024) return h3_fail(kGeneralProtocolError, Http3Error::Protocol);
    control_.append(ConstByteView(buffer, n));
  }
  for (;;) {
    ByteReader r(control_.view().subview(control_pos_));
    const uint64_t type = quic::read_varint(r);
    const uint64_t length = quic::read_varint(r);
    if (!r.ok() || length > r.remaining()) break;   // not all there yet
    const ConstByteView payload = r.take(length);
    control_pos_ += r.position();
    if (!settings_seen_ && type != kFrameSettings) return h3_fail(kMissingSettings, Http3Error::Protocol);
    switch (type) {
      case kFrameSettings: {
        if (settings_seen_) return h3_fail(kFrameUnexpected, Http3Error::Protocol);
        settings_seen_ = true;
        ByteReader s(payload);
        while (s.remaining() > 0) {
          quic::read_varint(s);   // identifier
          quic::read_varint(s);   // value
          if (!s.ok()) return h3_fail(kFrameError, Http3Error::Protocol);
        }
        break;
      }
      case kFrameGoaway: {
        ByteReader g(payload);
        const uint64_t last = quic::read_varint(g);
        if (!g.ok()) return h3_fail(kFrameError, Http3Error::Protocol);
        goaway_ = true;
        goaway_id_ = last;
        if (last <= request_stream_ && !head_done_) return fail(Http3Error::Refused);
        break;
      }
      case kFrameData:
      case kFrameHeaders:
      case kFramePushPromise: return h3_fail(kFrameUnexpected, Http3Error::Protocol);
      default: break;   // CANCEL_PUSH, MAX_PUSH_ID (from a server: an error, but harmless here) and unknown frames
    }
  }
  if (control_pos_ > 0 && control_pos_ == control_.size()) {
    control_.clear();
    control_pos_ = 0;
  }
  if (connection_.stream_finished(id)) h3_fail(kClosedCriticalStream, Http3Error::Protocol);
}

// A complete field section is in section_.
bool Http3Exchange::on_section() {
  scratch_.clear();
  const h3::QpackResult result = h3::qpack_decode(section_.view(), &scratch_, kMaxHeaderBytes);
  if (result == h3::QpackResult::TooLarge) {
    fail(Http3Error::TooLarge);
    return false;
  }
  if (result == h3::QpackResult::OutOfMemory) {
    fail(Http3Error::Internal);
    return false;
  }
  if (result != h3::QpackResult::Ok) {
    h3_fail(kQpackDecompressionFailed, Http3Error::Protocol);
    return false;
  }
  if (head_done_) return true;   // trailers: not kept
  // pseudo-header fields come first; a response has exactly :status
  bool pseudo_done = false;
  StringView status;
  for (uint64_t i = 0; i < scratch_.count(); ++i) {
    const StringView name = scratch_.name(i);
    for (uint64_t k = 0; k < name.size(); ++k)
      if (name[k] >= 'A' && name[k] <= 'Z') {
        h3_fail(kMessageError, Http3Error::Protocol);
        return false;
      }
    if (!name.empty() && name[0] == ':') {
      if (pseudo_done || !name.equals(":status") || !status.empty()) {
        h3_fail(kMessageError, Http3Error::Protocol);
        return false;
      }
      status = scratch_.value(i);
    } else {
      pseudo_done = true;
    }
  }
  if (status.size() != 3 || !is_digit(status[0]) || !is_digit(status[1]) || !is_digit(status[2])) {
    h3_fail(kMessageError, Http3Error::Protocol);
    return false;
  }
  const uint32_t code = static_cast<uint32_t>((status[0] - '0') * 100 + (status[1] - '0') * 10 + (status[2] - '0'));
  if (code < 200) {   // interim (101 is not allowed over HTTP/3)
    if (code == 101) {
      h3_fail(kMessageError, Http3Error::Protocol);
      return false;
    }
    return true;
  }
  status_ = code;
  response_.clear();
  for (uint64_t i = 0; i < scratch_.count(); ++i)
    if (scratch_.name(i)[0] != ':' && !response_.add(scratch_.name(i), scratch_.value(i))) {
      fail(Http3Error::Internal);
      return false;
    }
  head_done_ = true;
  return true;
}

void Http3Exchange::end_frame() {
  if (frame_type_ == kFrameHeaders) {
    const bool ok = on_section();
    section_.clear();
    if (!ok) return;
  }
  frame_ = Frame::Type;
}

void Http3Exchange::read_response() {
  while (phase_ == Http3Phase::Requesting) {
    if (frame_ == Frame::Type) {
      if (!read_varint(request_stream_, &vtype_, &frame_type_)) break;
      frame_ = Frame::Length;
    }
    if (frame_ == Frame::Length) {
      if (!read_varint(request_stream_, &vlength_, &frame_left_)) break;
      switch (frame_type_) {
        case kFrameData:
          if (!head_done_) return h3_fail(kFrameUnexpected, Http3Error::Protocol);
          break;
        case kFrameHeaders:
          if (frame_left_ > kMaxHeaderBytes) return fail(Http3Error::TooLarge);
          section_.clear();
          break;
        case kFrameCancelPush:
        case kFrameSettings:
        case kFramePushPromise:
        case kFrameGoaway:
        case kFrameMaxPushId: return h3_fail(kFrameUnexpected, Http3Error::Protocol);
        default: break;   // unknown frames are skipped
      }
      frame_ = Frame::Payload;
      if (frame_left_ == 0) {
        end_frame();
        continue;
      }
    }
    // Payload
    if (frame_type_ == kFrameData && body_.size() - body_pos_ >= kBodyBuffer) break;   // the caller has to take some first
    uint8_t buffer[4096];
    const uint64_t want = frame_left_ < sizeof buffer ? frame_left_ : sizeof buffer;
    const uint64_t n = connection_.read_stream(request_stream_, ByteView(buffer, want));
    if (n == 0) break;
    frame_left_ -= n;
    if (frame_type_ == kFrameData) {
      if (!body_.append(ConstByteView(buffer, n))) return fail(Http3Error::Internal);
      body_total_ += n;
    } else if (frame_type_ == kFrameHeaders) {
      if (!section_.append(ConstByteView(buffer, n))) return fail(Http3Error::Internal);
    }
    if (frame_left_ == 0) end_frame();
  }
}

void Http3Exchange::poll() {
  if (phase_ != Http3Phase::Connecting && phase_ != Http3Phase::Requesting) return;
  connection_.poll();
  // The connection may be gone (the server closed it right after the answer, say) while the answer is still waiting to be read: that is read first.
  const bool down = connection_.failed() || (connection_.closed() && !closed_);
  if (phase_ == Http3Phase::Connecting) {
    if (down) return fail(Http3Error::Connection);
    if (!connection_.established()) return;
    phase_ = Http3Phase::Requesting;
  }
  if (!set_up_) {
    set_up();
    if (phase_ != Http3Phase::Requesting) return;
  }
  if (!down) {
    if (!request_done_) flush_request();
    service_peer_streams();
    if (phase_ != Http3Phase::Requesting) return;
  }
  uint64_t code = 0;
  if (connection_.stream_reset_by_peer(request_stream_, &code)) {
    h3_code_ = code;
    return fail(code == kRequestRejected ? Http3Error::Refused : Http3Error::Reset);
  }
  read_response();
  if (phase_ != Http3Phase::Requesting) return;
  // finished: the stream ended exactly at a frame boundary
  if (connection_.stream_finished(request_stream_)) {
    if (frame_ != Frame::Type || vtype_.got != 0 || !head_done_) return h3_fail(kFrameError, Http3Error::Protocol);
    return complete();
  }
  if (down && connection_.readable(request_stream_) == 0) {   // nothing more will come
    if (connection_.peer_close_was_application() && connection_.peer_close_code() == kRequestRejected) return fail(Http3Error::Refused);
    if (goaway_ && !head_done_) return fail(Http3Error::Refused);
    return fail(Http3Error::Connection);
  }
}

}  // namespace network
