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

namespace network {

using namespace quic;

namespace {
constexpr uint64_t kSendBufferLimit = 2u << 20;   // bytes queued on a stream and not acknowledged
constexpr uint64_t kServerUniWindow = 256 * 1024;

// Milliseconds from `then` to `now`; 0 if `then` is later (a clock read after `now` was taken).
uint64_t since(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }
}  // namespace

void QuicConnection::poll() {
  if (state_ == QuicState::Idle || state_ == QuicState::Failed || state_ == QuicState::Closed) return;
  drain_socket();
  if (state_ == QuicState::Failed || state_ == QuicState::Closed) return;
  const uint64_t now = stack_.now_ms();
  if (have_old_keys_ && now > old_keys_until_) {
    have_old_keys_ = false;
    old_receive_keys_.wipe();
  }
  if (state_ == QuicState::Connecting && since(now, started_ms_) > config_.handshake_timeout_ms) {
    error_ = QuicError::Timeout;
    state_ = QuicState::Failed;
    return;
  }
  if (idle_timeout_ms_ != 0 && since(now, last_activity_ms_) > idle_timeout_ms_) {
    error_ = QuicError::Timeout;
    state_ = QuicState::Failed;
    return;
  }
  if (loss_timer_ms_ != 0 && now >= loss_timer_ms_) on_loss_timer();
  send_pending();
}

// ---- Streams

bool QuicConnection::open_stream(bool bidirectional, uint64_t* id) {
  if (state_ != QuicState::Established || !transport_parameters_applied_) return false;
  const uint64_t limit = bidirectional ? peer_max_streams_bidi_ : peer_max_streams_uni_;
  uint64_t& opened = bidirectional ? streams_opened_bidi_ : streams_opened_uni_;
  if (opened >= limit) return false;
  const uint64_t new_id = (opened << 2) | (bidirectional ? 0 : 2);
  auto s = UniquePointer<Stream>::make(allocator_, allocator_, new_id);
  if (!s) return false;
  s->send_limit = bidirectional ? peer_initial_bidi_remote_ : peer_initial_uni_;
  s->recv_limit = bidirectional ? config_.stream_window : 0;
  if (!streams_.push_back(util::move(s))) return false;
  ++opened;
  *id = new_id;
  return true;
}

uint64_t QuicConnection::write_stream(uint64_t id, ConstByteView data, bool fin) {
  Stream* s = find_stream(id);
  if (!s || s->send.finishing() || s->reset_sent || s->reset_to_send) return 0;
  if (!stream_is_ours(id) && (id & 2)) return 0;   // (a stream the peer opened to send on)
  const uint64_t taken = s->send.write(data, kSendBufferLimit);
  if (fin && taken == data.size()) s->send.finish();
  return taken;
}

uint64_t QuicConnection::writable(uint64_t id) const {
  const Stream* s = find_stream(id);
  if (!s || s->send.finishing()) return 0;
  return s->send.unacknowledged() < kSendBufferLimit ? kSendBufferLimit - s->send.unacknowledged() : 0;
}

uint64_t QuicConnection::readable(uint64_t id) const {
  const Stream* s = find_stream(id);
  return s ? s->recv.available() : 0;
}

uint64_t QuicConnection::read_stream(uint64_t id, ByteView out) {
  Stream* s = find_stream(id);
  if (!s) return 0;
  const uint64_t n = s->recv.read(out);
  if (n == 0 && !(s->recv.finished() && !s->counted_for_credit)) return 0;
  data_consumed_ += n;
  // the peer is given more room as the data is taken
  if (recv_max_data_ - data_consumed_ < config_.connection_window / 2) {
    recv_max_data_ = data_consumed_ + config_.connection_window;
    max_data_pending_ = true;
  }
  const uint64_t window = (s->id & 2) ? kServerUniWindow : config_.stream_window;
  if (s->recv_limit - s->recv.read_offset() < window / 2 && !s->recv.fin_known()) {
    s->recv_limit = s->recv.read_offset() + window;
    s->max_stream_data_pending = true;
  }
  if (s->recv.finished() && !s->counted_for_credit && !stream_is_ours(s->id)) {   // a stream of the server's is over: it may open another
    s->counted_for_credit = true;
    ++local_max_streams_uni_;
    max_streams_pending_ = true;
  }
  return n;
}

bool QuicConnection::stream_finished(uint64_t id) const {
  const Stream* s = find_stream(id);
  return s && s->recv.finished();
}

bool QuicConnection::stream_reset_by_peer(uint64_t id, uint64_t* code) const {
  const Stream* s = find_stream(id);
  if (!s || !s->reset_received) return false;
  *code = s->reset_code;
  return true;
}

bool QuicConnection::stream_stop_requested(uint64_t id, uint64_t* code) const {
  const Stream* s = find_stream(id);
  if (!s || !s->stop_requested) return false;
  *code = s->stop_code;
  return true;
}

bool QuicConnection::accept_stream(uint64_t* id) {
  Stream* best = nullptr;
  for (uint64_t i = 0; i < streams_.count(); ++i) {
    Stream* s = streams_[i].get();
    if (stream_is_ours(s->id) || s->accepted) continue;
    if (!best || s->id < best->id) best = s;
  }
  if (!best) return false;
  best->accepted = true;
  *id = best->id;
  return true;
}

void QuicConnection::reset_stream(uint64_t id, uint64_t error_code) {
  Stream* s = find_stream(id);
  if (!s || s->reset_sent || s->reset_to_send || (!stream_is_ours(id) && (id & 2))) return;
  s->reset_to_send = true;
  s->reset_to_send_code = error_code;
}

void QuicConnection::stop_sending(uint64_t id, uint64_t error_code) {
  Stream* s = find_stream(id);
  if (!s || s->stop_sent || s->stop_to_send || (stream_is_ours(id) && (id & 2))) return;
  s->stop_to_send = true;
  s->stop_to_send_code = error_code;
}

void QuicConnection::close(uint64_t error_code, StringView reason) {
  if (state_ == QuicState::Idle || state_ == QuicState::Closed || state_ == QuicState::Failed) return;
  close_pending_ = true;
  close_application_ = spaces_[2].keys_ready;   // (before 1-RTT keys exist the transport form of the frame is used)
  close_code_ = error_code;
  close_reason_.assign(reason);
  send_pending();
  state_ = QuicState::Closed;
  if (error_ == QuicError::None) error_ = QuicError::Closed;
}

}  // namespace network
