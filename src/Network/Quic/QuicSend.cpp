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

#include "Network/Quic/QuicConnection.hpp"
#include "Utility/Move.hpp"

// Building and sending packets.
namespace network {

using namespace quic;

namespace {
constexpr uint32_t kTagLength = 16;
constexpr uint32_t kMinInitialDatagram = 1200;
}  // namespace

bool QuicConnection::want_to_send(uint32_t space_index) const {
  const Space& space = spaces_[space_index];
  if (!space.keys_ready || space.discarded) return false;
  if (close_pending_ && !close_sent_) {
    // the connection is closed in the highest space that has keys
    for (uint32_t i = kSpaceCount; i > 0; --i)
      if (spaces_[i - 1].keys_ready && !spaces_[i - 1].discarded) return i - 1 == space_index;
    return false;
  }
  if (space.ack_pending || space.crypto_send.has_pending() || space.probes > 0) return true;
  if (space_index != 2) return false;
  if (state_ != QuicState::Established && !(tls_ && tls_->handshake_complete())) return false;
  if (path_response_.size() > 0 || retire_queue_.count() > 0 || max_data_pending_ || max_streams_pending_) return true;
  for (uint64_t i = 0; i < streams_.count(); ++i) {
    const Stream& s = *streams_[i];
    if (s.max_stream_data_pending || s.reset_to_send || s.stop_to_send) return true;
    if (s.send.has_pending()) return true;
  }
  return false;
}

bool QuicConnection::add_ack_frame(uint32_t space_index, ByteWriter& w, uint32_t limit) {
  Space& space = spaces_[space_index];
  if (space.received.empty()) return true;
  const uint64_t n = space.received.count();
  const uint64_t now = stack_.now_ms();
  const Range& top = space.received.at(n - 1);
  const uint64_t delay_us = (now > space.largest_received_ms ? now - space.largest_received_ms : 0) * 1000;
  const uint64_t delay = delay_us >> 3;   // (our ack_delay_exponent is the default, 3)
  // how many older ranges fit
  uint64_t fit = 0;
  uint64_t used = 1 + varint_size(top.last) + varint_size(delay) + 2 + varint_size(top.last - top.first);
  if (w.position() + used > limit) return true;
  for (uint64_t i = n - 1; i > 0; --i) {
    const Range& hi = space.received.at(i);
    const Range& lo = space.received.at(i - 1);
    const uint64_t gap = hi.first - lo.last - 2, len = lo.last - lo.first;
    const uint64_t add = varint_size(gap) + varint_size(len);
    if (w.position() + used + add > limit || fit >= 60) break;
    used += add;
    ++fit;
  }
  w.u8(0x02);
  write_varint(w, top.last);
  write_varint(w, delay);
  write_varint(w, fit);
  write_varint(w, top.last - top.first);
  for (uint64_t k = 0; k < fit; ++k) {
    const Range& hi = space.received.at(n - 1 - k);
    const Range& lo = space.received.at(n - 2 - k);
    write_varint(w, hi.first - lo.last - 2);
    write_varint(w, lo.last - lo.first);
  }
  space.ack_pending = false;
  space.unacked_ack_eliciting = 0;
  return true;
}

// One packet of a space, written into `buffer[0, capacity)` (without its tag, which the caller makes room for after it): header and frames. False if there was
// nothing to send.
bool QuicConnection::build_packet(uint32_t space_index, uint8_t* buffer, uint32_t capacity, PacketOut* out) {
  Space& space = spaces_[space_index];
  if (capacity < 64 + kTagLength) return false;
  const bool long_header = space_index != 2;
  ByteWriter w{ByteView(buffer, capacity)};
  uint32_t length_field_at = 0;
  if (long_header) {
    w.u8(static_cast<uint8_t>(0xC0 | ((space_index == 0 ? 0 : 2) << 4) | (kPacketNumberLength - 1)));
    w.be32(kVersion1);
    w.u8(dcid_.length);
    w.bytes(dcid_.view());
    w.u8(scid_.length);
    w.bytes(scid_.view());
    if (space_index == 0) {
      write_varint(w, retry_token_.size());
      w.bytes(retry_token_.view());
    }
    length_field_at = static_cast<uint32_t>(w.position());
    w.be16(0x4000);
  } else {
    w.u8(static_cast<uint8_t>(0x40 | (key_phase_ ? 0x04 : 0) | (kPacketNumberLength - 1)));
    w.bytes(dcid_.view());
  }
  const uint32_t pn_offset = static_cast<uint32_t>(w.position());
  const uint64_t number = space.next_number;
  w.be32(static_cast<uint32_t>(number));
  const uint32_t header_length = static_cast<uint32_t>(w.position());
  const uint32_t limit = capacity - kTagLength;   // frames must end before this
  if (!w.ok() || header_length + 8 > limit) return false;

  SentPacket& rec = out->record;
  rec = SentPacket();
  rec.number = number;
  rec.time_ms = stack_.now_ms();
  bool ack_eliciting = false;
  auto room = [&]() { return limit > w.position() ? static_cast<uint32_t>(limit - w.position()) : 0u; };
  auto note = [&](const FrameRef& f) {
    if (rec.frame_count < kMaxFramesPerPacket) rec.frames[rec.frame_count++] = f;
  };
  const uint64_t cwnd_room = congestion_window_ > bytes_in_flight_ ? congestion_window_ - bytes_in_flight_ : 0;
  const bool may_send_data = space.probes > 0 || cwnd_room >= 128;

  if (close_pending_ && !close_sent_) {
    // CONNECTION_CLOSE alone
    w.u8(close_application_ ? 0x1d : 0x1c);
    write_varint(w, close_code_);
    if (!close_application_) write_varint(w, 0);
    uint64_t reason = close_reason_.size() < 100 ? close_reason_.size() : 100;
    write_varint(w, reason);
    w.bytes(close_reason_.view().first(reason).size() ? ConstByteView(reinterpret_cast<const uint8_t*>(close_reason_.data()), reason) : ConstByteView());
    if (!w.ok()) return false;
    close_sent_ = true;
    out->space = space_index;
    out->start = 0;
    out->pn_offset = pn_offset;
    out->header_length = header_length;
    out->length_field_at = length_field_at;
    out->number = number;
    out->end = static_cast<uint32_t>(w.position());
    ++space.next_number;
    return true;
  }

  if (space.ack_pending) add_ack_frame(space_index, w, limit);

  if (may_send_data) {
    // CRYPTO
    if (space.probes > 0 && space_index < 2 && !space.crypto_send.has_pending())
      for (uint32_t i = 0; i < space.last_probe_crypto_count; ++i) space.crypto_send.on_lost(space.last_probe_crypto[i].offset, space.last_probe_crypto[i].length, false);
    while (space.crypto_send.has_pending() && room() > 16 && rec.frame_count < kMaxFramesPerPacket) {
      const uint32_t overhead = 1 + 8 + 2;
      uint64_t offset = 0;
      ConstByteView data;
      bool fin = false;
      if (!space.crypto_send.next(room() - overhead, ~uint64_t{0}, &offset, &data, &fin)) break;
      w.u8(0x06);
      write_varint(w, offset);
      write_varint(w, data.size());
      w.bytes(data);
      FrameRef f;
      f.kind = FrameRef::Crypto;
      f.offset = offset;
      f.length = data.size();
      note(f);
      ack_eliciting = true;
    }
    if (space_index == 2 && (tls_ && tls_->handshake_complete())) {
      // PATH_RESPONSE
      while (path_response_.size() >= 8 && room() >= 9) {
        w.u8(0x1b);
        w.bytes(path_response_.view().first(8));
        ByteBuffer rest(allocator_);
        rest.append(path_response_.view().subview(8));
        path_response_.clear();
        path_response_.append(rest.view());
        ack_eliciting = true;
      }
      while (retire_queue_.count() > 0 && room() >= 10 && rec.frame_count < kMaxFramesPerPacket) {
        const uint64_t seq = retire_queue_[0];
        retire_queue_.remove_at(0);
        w.u8(0x19);
        write_varint(w, seq);
        FrameRef f;
        f.kind = FrameRef::RetireConnectionId;
        f.id = seq;
        note(f);
        ack_eliciting = true;
      }
      if (max_data_pending_ && room() >= 9 && rec.frame_count < kMaxFramesPerPacket) {
        w.u8(0x10);
        write_varint(w, recv_max_data_);
        max_data_pending_ = false;
        FrameRef f;
        f.kind = FrameRef::MaxData;
        note(f);
        ack_eliciting = true;
      }
      if (max_streams_pending_ && room() >= 9 && rec.frame_count < kMaxFramesPerPacket) {
        w.u8(0x13);
        write_varint(w, local_max_streams_uni_);
        max_streams_pending_ = false;
        FrameRef f;
        f.kind = FrameRef::MaxStreams;
        note(f);
        ack_eliciting = true;
      }
      for (uint64_t i = 0; i < streams_.count() && rec.frame_count < kMaxFramesPerPacket; ++i) {
        Stream& s = *streams_[i];
        if (s.max_stream_data_pending && room() >= 17) {
          w.u8(0x11);
          write_varint(w, s.id);
          write_varint(w, s.recv_limit);
          s.max_stream_data_pending = false;
          FrameRef f;
          f.kind = FrameRef::MaxStreamData;
          f.id = s.id;
          note(f);
          ack_eliciting = true;
        }
        if (s.reset_to_send && room() >= 25 && rec.frame_count < kMaxFramesPerPacket) {
          w.u8(0x04);
          write_varint(w, s.id);
          write_varint(w, s.reset_to_send_code);
          write_varint(w, s.send.next_offset());
          s.reset_to_send = false;
          s.reset_sent = true;
          FrameRef f;
          f.kind = FrameRef::ResetStream;
          f.id = s.id;
          note(f);
          ack_eliciting = true;
        }
        if (s.stop_to_send && room() >= 17 && rec.frame_count < kMaxFramesPerPacket) {
          w.u8(0x05);
          write_varint(w, s.id);
          write_varint(w, s.stop_to_send_code);
          s.stop_to_send = false;
          s.stop_sent = true;
          FrameRef f;
          f.kind = FrameRef::StopSending;
          f.id = s.id;
          note(f);
          ack_eliciting = true;
        }
      }
      // STREAM data, the streams taking turns
      const uint64_t count = streams_.count();
      for (uint64_t k = 0; k < count && rec.frame_count < kMaxFramesPerPacket; ++k) {
        Stream& s = *streams_[(next_stream_round_ + k) % count];
        if (s.reset_sent || s.reset_to_send || !s.send.has_pending()) continue;
        if (!stream_is_ours(s.id) && (s.id & 2) == 0) continue;
        const uint32_t overhead = 1 + varint_size(s.id) + 8 + 4;
        if (room() <= overhead) break;
        const uint64_t prior_next = s.send.next_offset();
        const uint64_t connection_credit = peer_max_data_ > data_sent_ ? peer_max_data_ - data_sent_ : 0;
        const uint64_t limit_offset = prior_next + connection_credit < s.send_limit ? prior_next + connection_credit : s.send_limit;
        uint64_t offset = 0;
        ConstByteView data;
        bool fin = false;
        if (!s.send.next(room() - overhead, limit_offset, &offset, &data, &fin)) continue;
        w.u8(static_cast<uint8_t>(0x08 | 0x04 | 0x02 | (fin ? 1 : 0)));
        write_varint(w, s.id);
        write_varint(w, offset);
        write_varint(w, data.size());
        w.bytes(data);
        if (offset + data.size() > prior_next) data_sent_ += offset + data.size() - prior_next;
        FrameRef f;
        f.kind = FrameRef::Stream;
        f.id = s.id;
        f.offset = offset;
        f.length = data.size();
        f.fin = fin;
        note(f);
        ack_eliciting = true;
      }
      if (count > 0) next_stream_round_ = static_cast<uint32_t>((next_stream_round_ + 1) % count);
    }
  }
  // a probe has to get an answer: if nothing else is in the packet, it is a PING
  if (space.probes > 0 && !ack_eliciting && room() >= 1) {
    w.u8(0x01);
    ack_eliciting = true;
  }
  if (!w.ok() || w.position() == header_length) return false;   // nothing in it
  if (ack_eliciting && space.probes > 0) {
    --space.probes;
    if (space_index < 2) {
      space.last_probe_crypto_count = 0;
      for (uint32_t i = 0; i < rec.frame_count && space.last_probe_crypto_count < 2; ++i)
        if (rec.frames[i].kind == FrameRef::Crypto) space.last_probe_crypto[space.last_probe_crypto_count++] = rec.frames[i];
    }
  }
  rec.ack_eliciting = ack_eliciting;
  rec.in_flight = ack_eliciting;   // (ACK-only packets do not count against the congestion window)
  out->space = space_index;
  out->start = 0;
  out->pn_offset = pn_offset;
  out->header_length = header_length;
  out->length_field_at = length_field_at;
  out->number = number;
  out->end = static_cast<uint32_t>(w.position());
  ++space.next_number;
  return true;
}

bool QuicConnection::build_datagram(uint8_t* buffer, uint32_t capacity, uint32_t* length) {
  PacketOut packets[kSpaceCount];
  uint32_t count = 0, position = 0;
  bool has_initial = false, has_handshake = false;
  for (uint32_t space = 0; space < kSpaceCount; ++space) {
    if (!want_to_send(space)) continue;
    if (capacity - position < 64 + kTagLength) break;
    PacketOut& po = packets[count];
    if (!build_packet(space, buffer + position, capacity - position, &po)) continue;
    po.start = position;
    // positions are relative to the datagram from here on
    po.pn_offset += position;
    po.header_length += position;
    if (space != 2) po.length_field_at += position;
    po.end += position;
    position = po.end + kTagLength;
    if (space == 0) has_initial = true;
    if (space == 1) has_handshake = true;
    ++count;
    if (space == 2) break;
  }
  if (count == 0) return false;
  // a datagram that holds an Initial packet is at least 1200 bytes: the last packet is padded
  PacketOut& last = packets[count - 1];
  if (has_initial && position < kMinInitialDatagram) {
    const uint32_t pad = kMinInitialDatagram - position;
    if (last.end + pad + kTagLength <= capacity) {
      for (uint32_t i = 0; i < pad; ++i) buffer[last.end + i] = 0;
      last.end += pad;
      position += pad;
    }
  }
  // lengths of the long header packets, then protection
  for (uint32_t i = 0; i < count; ++i) {
    PacketOut& po = packets[i];
    if (po.space != 2) {
      const uint32_t packet_length = po.end + kTagLength - po.pn_offset;   // packet number, payload and tag
      buffer[po.length_field_at] = static_cast<uint8_t>(0x40 | (packet_length >> 8));
      buffer[po.length_field_at + 1] = static_cast<uint8_t>(packet_length);
    }
    Space& space = spaces_[po.space];
    if (!protect_packet(space.send_keys, po.number, ByteView(buffer + po.start, po.end + kTagLength - po.start), po.pn_offset - po.start, kPacketNumberLength)) {
      fail(QuicError::Internal);
      return false;
    }
    po.record.size = po.end + kTagLength - po.start;
    space.sent.push_back(po.record);
    on_packet_sent(po.space, space.sent[space.sent.count() - 1]);
  }
  *length = position;
  // the client stops using the Initial keys when it first sends a Handshake packet
  if (has_handshake && !initial_keys_dropped_) {
    initial_keys_dropped_ = true;
    discard_space(0);
  }
  return true;
}

bool QuicConnection::send_datagram(ConstByteView datagram) {
  if (!socket_->send_to(peer_address_, peer_port_, datagram)) return false;
  stats_.bytes_sent += datagram.size();
  return true;
}

void QuicConnection::send_pending() {
  if (!socket_) return;
  if (stash_.size() > 0) {
    if (!send_datagram(stash_.view())) return;
    stash_.clear();
  }
  for (uint32_t i = 0; i < 24; ++i) {
    uint8_t buffer[1500];
    uint32_t length = 0;
    if (!build_datagram(buffer, config_.max_datagram, &length)) break;
    if (!send_datagram(ConstByteView(buffer, length))) {
      stash_.append(ConstByteView(buffer, length));
      break;
    }
    if (close_sent_) break;
  }
}

}  // namespace network
