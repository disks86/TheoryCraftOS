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

// Loss detection and congestion control (RFC 9002).
namespace network {

using namespace quic;

namespace {
constexpr uint64_t kGranularityMs = 1;
constexpr uint32_t kPacketThreshold = 3;
constexpr uint64_t kInitialRttMs = 333;
}  // namespace

uint64_t QuicConnection::pto_interval_ms(uint32_t space) const {
  uint64_t interval = smoothed_rtt_ms_ + (4 * rtt_var_ms_ > kGranularityMs ? 4 * rtt_var_ms_ : kGranularityMs);
  if (space == 2) interval += peer_max_ack_delay_ms_;
  return interval;
}

void QuicConnection::on_packet_sent(uint32_t space_index, SentPacket& packet) {
  Space& space = spaces_[space_index];
  if (packet.ack_eliciting) space.last_ack_eliciting_ms = packet.time_ms;
  if (packet.in_flight) bytes_in_flight_ += packet.size;
  ++stats_.packets_sent;
  set_loss_timer();
}

void QuicConnection::update_rtt(uint64_t latest_ms, uint64_t ack_delay_ms, bool application) {
  latest_rtt_ms_ = latest_ms;
  if (!have_rtt_) {
    have_rtt_ = true;
    min_rtt_ms_ = latest_ms;
    smoothed_rtt_ms_ = latest_ms;
    rtt_var_ms_ = latest_ms / 2;
    return;
  }
  if (latest_ms < min_rtt_ms_) min_rtt_ms_ = latest_ms;
  if (application) {
    if (ack_delay_ms > peer_max_ack_delay_ms_) ack_delay_ms = peer_max_ack_delay_ms_;
  } else {
    ack_delay_ms = 0;
  }
  uint64_t adjusted = latest_ms;
  if (latest_ms >= min_rtt_ms_ + ack_delay_ms) adjusted -= ack_delay_ms;
  const uint64_t deviation = smoothed_rtt_ms_ > adjusted ? smoothed_rtt_ms_ - adjusted : adjusted - smoothed_rtt_ms_;
  rtt_var_ms_ = (3 * rtt_var_ms_ + deviation) / 4;
  smoothed_rtt_ms_ = (7 * smoothed_rtt_ms_ + adjusted) / 8;
}

// Takes the retransmittable content of a packet that did not arrive back into the send queues.
void QuicConnection::requeue_frames(uint32_t space_index, const SentPacket& packet) {
  Space& space = spaces_[space_index];
  for (uint32_t i = 0; i < packet.frame_count; ++i) {
    const FrameRef& f = packet.frames[i];
    ++stats_.retransmissions;
    switch (f.kind) {
      case FrameRef::Crypto: space.crypto_send.on_lost(f.offset, f.length, false); break;
      case FrameRef::Stream: {
        Stream* s = find_stream(f.id);
        if (s) s->send.on_lost(f.offset, f.length, f.fin);
        break;
      }
      case FrameRef::MaxData: max_data_pending_ = true; break;
      case FrameRef::MaxStreamData: {
        Stream* s = find_stream(f.id);
        if (s) s->max_stream_data_pending = true;
        break;
      }
      case FrameRef::MaxStreams: max_streams_pending_ = true; break;
      case FrameRef::RetireConnectionId: retire_queue_.push_back(f.id); break;
      case FrameRef::ResetStream: {
        Stream* s = find_stream(f.id);
        if (s) s->reset_to_send = true;
        break;
      }
      case FrameRef::StopSending: {
        Stream* s = find_stream(f.id);
        if (s) s->stop_to_send = true;
        break;
      }
      case FrameRef::PathResponse:
      case FrameRef::HandshakeDone:
      case FrameRef::NewConnectionId: break;
    }
  }
}

void QuicConnection::on_packet_lost(uint32_t space_index, SentPacket& packet, bool congestion_event) {
  if (packet.in_flight && bytes_in_flight_ >= packet.size) bytes_in_flight_ -= packet.size;
  ++stats_.packets_lost;
  requeue_frames(space_index, packet);
  if (congestion_event && packet.in_flight) {
    // a loss event: the window halves, once per round trip
    if (packet.time_ms > recovery_start_ms_ || recovery_start_ms_ == 0) {
      recovery_start_ms_ = stack_.now_ms();
      const uint64_t minimum = 2 * uint64_t{config_.max_datagram};
      congestion_window_ = congestion_window_ / 2 > minimum ? congestion_window_ / 2 : minimum;
      ssthresh_ = congestion_window_;
    }
  }
}

void QuicConnection::process_acked(uint32_t space_index, SentPacket& packet, bool* grew) {
  Space& space = spaces_[space_index];
  if (packet.in_flight && bytes_in_flight_ >= packet.size) bytes_in_flight_ -= packet.size;
  for (uint32_t i = 0; i < packet.frame_count; ++i) {
    const FrameRef& f = packet.frames[i];
    if (f.kind == FrameRef::Crypto) {
      space.crypto_send.on_acked(f.offset, f.length, false);
    } else if (f.kind == FrameRef::Stream) {
      Stream* s = find_stream(f.id);
      if (s) s->send.on_acked(f.offset, f.length, f.fin);
    }
  }
  if (packet.in_flight) {
    // congestion window growth (slow start, then congestion avoidance), unless the packet was sent before the current recovery period began
    if (!(recovery_start_ms_ != 0 && packet.time_ms <= recovery_start_ms_)) {
      if (congestion_window_ < ssthresh_) congestion_window_ += packet.size;
      else congestion_window_ += uint64_t{config_.max_datagram} * packet.size / congestion_window_;
      *grew = true;
    }
  }
}

void QuicConnection::detect_losses(uint32_t space_index) {
  Space& space = spaces_[space_index];
  space.loss_time_ms = 0;
  if (space.largest_acked < 0) return;
  const uint64_t now = stack_.now_ms();
  uint64_t base = smoothed_rtt_ms_ > latest_rtt_ms_ ? smoothed_rtt_ms_ : latest_rtt_ms_;
  uint64_t loss_delay = base + base / 8;
  if (loss_delay < kGranularityMs) loss_delay = kGranularityMs;
  const uint64_t lost_send_time = now > loss_delay ? now - loss_delay : 0;
  bool any_lost = false;
  for (uint64_t i = 0; i < space.sent.count();) {
    SentPacket& p = space.sent[i];
    if (static_cast<int64_t>(p.number) > space.largest_acked) break;
    if (p.time_ms <= lost_send_time || static_cast<uint64_t>(space.largest_acked) >= p.number + kPacketThreshold) {
      on_packet_lost(space_index, p, true);
      space.sent.remove_at(i);
      any_lost = true;
    } else {
      const uint64_t t = p.time_ms + loss_delay;
      if (space.loss_time_ms == 0 || t < space.loss_time_ms) space.loss_time_ms = t;
      ++i;
    }
  }
  (void)any_lost;
}

bool QuicConnection::on_ack(uint32_t space_index, ByteReader& r, bool with_ecn) {
  Space& space = spaces_[space_index];
  const uint64_t largest = read_varint(r), delay = read_varint(r), count = read_varint(r), first_range = read_varint(r);
  if (!r.ok()) return true;
  if (first_range > largest || count > 256) {
    fail(QuicError::Protocol, 7);
    return false;
  }
  if (largest >= space.next_number) {   // an acknowledgement of a packet that was never sent
    fail(QuicError::Protocol, 10);
    return false;
  }
  RangeSet acked(allocator_);
  uint64_t high = largest, low = largest - first_range;
  acked.add(low, high);
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t gap = read_varint(r), length = read_varint(r);
    if (!r.ok()) return true;
    if (low < gap + 2) {
      fail(QuicError::Protocol, 7);
      return false;
    }
    high = low - gap - 2;
    if (length > high) {
      fail(QuicError::Protocol, 7);
      return false;
    }
    low = high - length;
    acked.add(low, high);
  }
  if (with_ecn) {
    read_varint(r);
    read_varint(r);
    read_varint(r);
  }
  if (!r.ok()) return true;
  const uint64_t now = stack_.now_ms();
  bool largest_newly_acked = false, ack_eliciting_acked = false, grew = false;
  uint64_t largest_time = 0;
  for (uint64_t i = 0; i < space.sent.count();) {
    SentPacket& p = space.sent[i];
    if (!acked.contains(p.number)) {
      ++i;
      continue;
    }
    if (p.number == largest) {
      largest_newly_acked = true;
      largest_time = p.time_ms;
    }
    if (p.ack_eliciting) ack_eliciting_acked = true;
    process_acked(space_index, p, &grew);
    space.sent.remove_at(i);
  }
  (void)grew;
  if (static_cast<int64_t>(largest) > space.largest_acked) space.largest_acked = static_cast<int64_t>(largest);
  if (largest_newly_acked && ack_eliciting_acked) {
    const uint64_t ack_delay_ms = (delay << peer_ack_delay_exponent_) / 1000;
    update_rtt(now > largest_time ? now - largest_time : 0, ack_delay_ms, space_index == 2);
  }
  if (ack_eliciting_acked || largest_newly_acked) {
    detect_losses(space_index);
    pto_count_ = 0;
    set_loss_timer();
  }
  return true;
}

void QuicConnection::set_loss_timer() {
  uint64_t earliest_loss = 0;
  for (uint32_t i = 0; i < kSpaceCount; ++i) {
    if (spaces_[i].discarded) continue;
    const uint64_t t = spaces_[i].loss_time_ms;
    if (t != 0 && (earliest_loss == 0 || t < earliest_loss)) earliest_loss = t;
  }
  if (earliest_loss != 0) {
    loss_timer_ms_ = earliest_loss;
    return;
  }
  // nothing to detect: a probe timeout, if anything we sent can still be answered
  bool in_flight = false;
  for (uint32_t i = 0; i < kSpaceCount && !in_flight; ++i) {
    if (spaces_[i].discarded) continue;
    for (uint64_t k = 0; k < spaces_[i].sent.count(); ++k)
      if (spaces_[i].sent[k].ack_eliciting) {
        in_flight = true;
        break;
      }
  }
  // a client whose handshake is not confirmed keeps probing so that the server is not stuck waiting for it
  if (!in_flight && handshake_confirmed_) {
    loss_timer_ms_ = 0;
    return;
  }
  // the earliest space with something in flight (Application only once the handshake is confirmed)
  uint64_t timeout = 0;
  uint32_t chosen = kSpaceCount;
  for (uint32_t i = 0; i < kSpaceCount; ++i) {
    if (spaces_[i].discarded || !spaces_[i].keys_ready) continue;
    if (i == 2 && !handshake_confirmed_) continue;
    bool has = false;
    for (uint64_t k = 0; k < spaces_[i].sent.count(); ++k)
      if (spaces_[i].sent[k].ack_eliciting) has = true;
    if (!has && in_flight) continue;
    const uint64_t t = (spaces_[i].last_ack_eliciting_ms ? spaces_[i].last_ack_eliciting_ms : stack_.now_ms()) + (pto_interval_ms(i) << (pto_count_ > 10 ? 10 : pto_count_));
    if (chosen == kSpaceCount || t < timeout) {
      timeout = t;
      chosen = i;
    }
  }
  loss_timer_ms_ = chosen == kSpaceCount ? 0 : timeout;
}

void QuicConnection::on_loss_timer() {
  uint64_t earliest_loss = 0;
  uint32_t loss_space = kSpaceCount;
  for (uint32_t i = 0; i < kSpaceCount; ++i) {
    if (spaces_[i].discarded) continue;
    const uint64_t t = spaces_[i].loss_time_ms;
    if (t != 0 && (earliest_loss == 0 || t < earliest_loss)) {
      earliest_loss = t;
      loss_space = i;
    }
  }
  if (loss_space != kSpaceCount) {
    detect_losses(loss_space);
    set_loss_timer();
    return;
  }
  // a probe timeout: send something that gets an answer, in the space that has been waiting longest
  ++stats_.ptos;
  uint32_t chosen = kSpaceCount;
  uint64_t oldest = 0;
  for (uint32_t i = 0; i < kSpaceCount; ++i) {
    if (spaces_[i].discarded || !spaces_[i].keys_ready) continue;
    if (i == 2 && !handshake_confirmed_) continue;
    bool has = spaces_[i].sent.count() > 0;
    if (!has && i == 0 && !spaces_[1].keys_ready) has = true;   // (nothing outstanding but the handshake is not done: send a new Initial)
    if (!has) continue;
    const uint64_t t = spaces_[i].last_ack_eliciting_ms;
    if (chosen == kSpaceCount || t < oldest) {
      oldest = t;
      chosen = i;
    }
  }
  if (chosen == kSpaceCount) chosen = spaces_[1].keys_ready && !spaces_[1].discarded ? 1 : 0;
  Space& space = spaces_[chosen];
  // what is outstanding in it is declared lost so that it is sent again in the probe
  for (uint64_t i = 0; i < space.sent.count(); ++i) {
    SentPacket& p = space.sent[i];
    if (p.in_flight && bytes_in_flight_ >= p.size) bytes_in_flight_ -= p.size;
    if (p.ack_eliciting) requeue_frames(chosen, p);
  }
  space.sent.clear();
  space.probes = 2;
  ++pto_count_;
  set_loss_timer();
}

}  // namespace network
