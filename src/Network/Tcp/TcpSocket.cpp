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

#include "Network/Tcp/TcpSocket.hpp"
#include "Network/Checksum.hpp"
#include "Network/Tcp/TcpManager.hpp"

namespace network {

const char* tcp_state_name(TcpState state) {
  switch (state) {
    case TcpState::Closed:      return "CLOSED";
    case TcpState::Listen:      return "LISTEN";
    case TcpState::SynSent:     return "SYN_SENT";
    case TcpState::SynReceived: return "SYN_RECEIVED";
    case TcpState::Established: return "ESTABLISHED";
    case TcpState::FinWait1:    return "FIN_WAIT_1";
    case TcpState::FinWait2:    return "FIN_WAIT_2";
    case TcpState::CloseWait:   return "CLOSE_WAIT";
    case TcpState::Closing:     return "CLOSING";
    case TcpState::LastAck:     return "LAST_ACK";
    case TcpState::TimeWait:    return "TIME_WAIT";
  }
  return "?";
}

namespace {
constexpr uint32_t kMinRtoMs = 200, kMaxRtoMs = 8000, kInitialRtoMs = 1000;
constexpr uint16_t kOurMss = kMaxIpPayload - 20;  // 1460
}  // namespace

TcpSocket::TcpSocket(IIpLayer& ip, TcpManager& manager, allocators::IAllocator& allocator)
    : ip_(ip), manager_(manager), send_buffer_(allocator), receive_buffer_(allocator), accept_queue_(allocator) {}

// ------------------------------------------------------------------ user API

bool TcpSocket::connect(Ipv4Address address, uint16_t port) {
  if (state_ != TcpState::Closed || failed_ || port == 0 || !ip_.local_address().value()) return false;
  remote_address_ = address;
  remote_port_ = port;
  prepare_to_connect(manager_.allocate_port(), manager_.next_initial_sequence());
  if (failed_) return false;
  enter(TcpState::SynSent);
  send_segment(tcp_flags::kSyn, iss_, nullptr, 0, true);
  arm_timer();
  return true;
}

void TcpSocket::prepare_to_connect(uint16_t local_port, uint32_t initial_sequence) {
  local_port_ = local_port;
  iss_ = initial_sequence;
  snd_una_ = iss_;
  snd_nxt_ = iss_ + 1;  // the SYN occupies one sequence number
  rto_ms_ = kInitialRtoMs;
  if (!send_buffer_.reserve(kBufferSize) || !receive_buffer_.reserve(kBufferSize)) failed_ = true;
}

bool TcpSocket::listen(uint16_t port) {
  if (state_ != TcpState::Closed || failed_ || port == 0 || manager_.listening_on(port)) return false;
  local_port_ = port;
  enter(TcpState::Listen);
  return true;
}

SharedPointer<ITcpSocket> TcpSocket::accept() {
  if (state_ != TcpState::Listen || accept_queue_.empty()) return SharedPointer<ITcpSocket>();
  SharedPointer<TcpSocket> child = accept_queue_[0];
  accept_queue_.remove_at(0);
  return SharedPointer<ITcpSocket>(child);
}

uint32_t TcpSocket::send(const uint8_t* data, uint32_t length) {
  const bool can_send = state_ == TcpState::Established || state_ == TcpState::CloseWait ||
                        state_ == TcpState::SynSent || state_ == TcpState::SynReceived;
  if (!can_send || close_requested_ || !data) return 0;
  const uint32_t queued = send_buffer_.write(data, length);
  output();
  return queued;
}

uint32_t TcpSocket::receive(uint8_t* buffer, uint32_t capacity) {
  if (!buffer) return 0;
  const uint32_t n = receive_buffer_.read(buffer, capacity);
  // The sender may be waiting on a closed window: tell it once there is room again.
  if (n && last_advertised_window_ < kOurMss && advertised_window() >= kOurMss &&
      (state_ == TcpState::Established || state_ == TcpState::FinWait1 || state_ == TcpState::FinWait2))
    send_ack();
  return n;
}

void TcpSocket::close() {
  switch (state_) {
    case TcpState::Listen:
      enter(TcpState::Closed);
      manager_.detach_children(this);
      drop_pending_connections();
      break;
    case TcpState::SynSent:
      enter(TcpState::Closed);
      break;
    case TcpState::SynReceived:
    case TcpState::Established:
    case TcpState::CloseWait:
      close_requested_ = true;
      output();  // sends the FIN once the queued data is out
      break;
    default:
      break;
  }
}

void TcpSocket::abort() {
  if (state_ == TcpState::Listen) {
    close();
    return;
  }
  if (state_ != TcpState::Closed && state_ != TcpState::SynSent && state_ != TcpState::TimeWait)
    send_segment(tcp_flags::kRst | tcp_flags::kAck, snd_nxt_, nullptr, 0, false);
  if (listener_ && state_ == TcpState::SynReceived) {
    listener_->child_finished_handshake();
    listener_ = nullptr;
  }
  enter(TcpState::Closed);
}

void TcpSocket::drop_pending_connections() {
  while (!accept_queue_.empty()) {
    accept_queue_[0]->listener_ = nullptr;
    accept_queue_[0]->abort();
    accept_queue_.remove_at(0);
  }
  embryos_ = 0;
}

// ------------------------------------------------------------------ sending

uint32_t TcpSocket::advertised_window() const {
  const uint32_t w = receive_buffer_.free_space();
  return w > 65535 ? 65535 : w;
}

void TcpSocket::send_segment(uint8_t flags, uint32_t sequence, const uint8_t* data, uint32_t length, bool with_mss) {
  uint8_t packet[kMaxIpPayload];
  const uint32_t header = with_mss ? 24 : 20;
  if (header + length > sizeof(packet)) return;

  store16(packet, local_port_);
  store16(packet + 2, remote_port_);
  store32(packet + 4, sequence);
  store32(packet + 8, (flags & tcp_flags::kAck) ? rcv_nxt_ : 0);
  packet[12] = static_cast<uint8_t>((header / 4) << 4);
  packet[13] = flags;
  last_advertised_window_ = advertised_window();
  store16(packet + 14, static_cast<uint16_t>(last_advertised_window_));
  store16(packet + 16, 0);  // checksum, filled in below
  store16(packet + 18, 0);  // urgent pointer (unused)
  if (with_mss) {           // option: kind 2, length 4, our maximum segment size
    packet[20] = 2;
    packet[21] = 4;
    store16(packet + 22, kOurMss);
  }
  for (uint32_t i = 0; i < length; ++i) packet[header + i] = data[i];

  const uint16_t total = static_cast<uint16_t>(header + length);
  uint32_t sum = pseudo_header_sum(ip_.local_address(), remote_address_, kProtocolTcp, total);
  sum = checksum_add(sum, packet, total);
  store16(packet + 16, checksum_finish(sum));
  ip_.send_ip(remote_address_, kProtocolTcp, packet, total);  // a lost segment is retransmitted
}

// Sends whatever the peer's window and our send buffer allow, then the FIN if
// the application has closed and everything is out.
void TcpSocket::output() {
  const bool may_send_data = state_ == TcpState::Established || state_ == TcpState::CloseWait ||
                             state_ == TcpState::FinWait1 || state_ == TcpState::LastAck;
  if (!may_send_data) return;

  const uint16_t mss = peer_mss_ < kOurMss ? peer_mss_ : kOurMss;
  for (;;) {
    const uint32_t in_flight = snd_nxt_ - snd_una_ - (fin_sent_ ? 1 : 0);  // data only
    if (fin_sent_) break;  // nothing follows a FIN
    const uint32_t unsent = send_buffer_.size() > in_flight ? send_buffer_.size() - in_flight : 0;
    const uint32_t window_room = snd_wnd_ > in_flight ? snd_wnd_ - in_flight : 0;
    uint32_t n = unsent < window_room ? unsent : window_room;
    if (n > mss) n = mss;
    if (n == 0) break;

    uint8_t chunk[kMaxIpPayload];
    send_buffer_.peek(in_flight, chunk, n);
    const bool last = n == unsent;
    send_segment(tcp_flags::kAck | (last ? tcp_flags::kPsh : 0), snd_nxt_, chunk, n, false);
    if (!timing_) {  // time this segment (Karn: never a retransmission, see rollback)
      timing_ = true;
      timed_sequence_ = snd_nxt_ + n;
      timed_at_ = ip_.now_ms();
    }
    snd_nxt_ += n;
    if (!timer_deadline_) arm_timer();
  }

  // The FIN goes out once every queued byte has been sent.
  const uint32_t in_flight = snd_nxt_ - snd_una_;
  if (close_requested_ && !fin_sent_ && send_buffer_.size() == in_flight) {
    send_segment(tcp_flags::kFin | tcp_flags::kAck, snd_nxt_, nullptr, 0, false);
    fin_sent_ = true;
    ++snd_nxt_;
    if (state_ == TcpState::Established) enter(TcpState::FinWait1);
    else if (state_ == TcpState::CloseWait) enter(TcpState::LastAck);
    if (!timer_deadline_) arm_timer();
  }
}

void TcpSocket::arm_timer() { timer_deadline_ = ip_.now_ms() + rto_ms_; }

void TcpSocket::rollback_for_retransmission() {
  snd_nxt_ = snd_una_;  // go-back-N: send everything unacknowledged again
  fin_sent_ = false;    // ... including the FIN, if it was out
  timing_ = false;      // Karn's rule: an ACK after a retransmission says nothing about the RTT
}

// ------------------------------------------------------------------ receiving

void TcpSocket::enter(TcpState state) {
  state_ = state;
  if (state == TcpState::TimeWait) {
    time_wait_deadline_ = ip_.now_ms() + kTimeWaitMs;
    timer_deadline_ = 0;
  } else if (state == TcpState::Closed) {
    timer_deadline_ = 0;
  }
}

void TcpSocket::fail() {
  failed_ = true;
  if (listener_ && state_ == TcpState::SynReceived) {
    listener_->child_finished_handshake();
    listener_ = nullptr;
  }
  enter(TcpState::Closed);
}

void TcpSocket::accept_syn(TcpSocket* listener, Ipv4Address remote_address, uint16_t remote_port, uint16_t local_port,
                           const TcpSegment& syn, uint32_t initial_sequence) {
  listener_ = listener;
  remote_address_ = remote_address;
  remote_port_ = remote_port;
  prepare_to_connect(local_port, initial_sequence);
  if (failed_) {
    listener_ = nullptr;
    return;
  }
  irs_ = syn.sequence;
  rcv_nxt_ = irs_ + 1;
  if (syn.mss) peer_mss_ = syn.mss;
  snd_wnd_ = syn.window;
  snd_wl1_ = syn.sequence;
  snd_wl2_ = 0;
  enter(TcpState::SynReceived);
  send_segment(tcp_flags::kSyn | tcp_flags::kAck, iss_, nullptr, 0, true);
  arm_timer();
}

void TcpSocket::on_segment(const TcpSegment& s) {
  using namespace tcp_flags;
  if (state_ == TcpState::Closed || state_ == TcpState::Listen) return;

  if (state_ == TcpState::SynSent) {
    if (s.flags & kAck) {
      if (seq_le(s.acknowledgement, iss_) || seq_gt(s.acknowledgement, snd_nxt_)) {
        if (!(s.flags & kRst)) send_segment(kRst, s.acknowledgement, nullptr, 0, false);
        return;
      }
    }
    if (s.flags & kRst) {
      if (s.flags & kAck) fail();  // connection refused
      return;
    }
    if (!(s.flags & kSyn)) return;
    irs_ = s.sequence;
    rcv_nxt_ = irs_ + 1;
    if (s.mss) peer_mss_ = s.mss;
    if (s.flags & kAck) {
      snd_una_ = s.acknowledgement;
      snd_wnd_ = s.window;
      snd_wl1_ = s.sequence;
      snd_wl2_ = s.acknowledgement;
      retries_ = 0;
      timer_deadline_ = 0;
      enter(TcpState::Established);
      send_ack();
      output();
    } else {  // simultaneous open
      enter(TcpState::SynReceived);
      send_segment(kSyn | kAck, iss_, nullptr, 0, true);
    }
    return;
  }

  // From here on the connection is synchronized.
  if (s.flags & kRst) {
    // Only a reset whose sequence number falls in our receive window counts.
    if (seq_ge(s.sequence, rcv_nxt_) && seq_lt(s.sequence, rcv_nxt_ + advertised_window() + 1)) fail();
    return;
  }
  if (s.flags & kSyn) {  // a retransmitted SYN: answer again; anything else is ignored
    if (state_ == TcpState::SynReceived && s.sequence == irs_) send_segment(kSyn | kAck, iss_, nullptr, 0, true);
    return;
  }
  if (!(s.flags & kAck)) return;

  if (state_ == TcpState::SynReceived) {
    if (seq_lt(snd_una_, s.acknowledgement) && seq_le(s.acknowledgement, snd_nxt_)) {
      snd_una_ = s.acknowledgement;
      snd_wnd_ = s.window;
      snd_wl1_ = s.sequence;
      snd_wl2_ = s.acknowledgement;
      retries_ = 0;
      timer_deadline_ = 0;
      enter(TcpState::Established);
      established_event_ = true;
    } else {
      send_segment(kRst, s.acknowledgement, nullptr, 0, false);
      return;
    }
  }

  process_ack(s);
  accept_data(s);
  output();
}

void TcpSocket::process_ack(const TcpSegment& s) {
  using namespace tcp_flags;
  if (seq_gt(s.acknowledgement, snd_nxt_)) {  // acknowledges something we have not sent
    send_ack();
    return;
  }

  // Take the peer's window from the newest segment (RFC 793 SND.WL1/SND.WL2).
  const bool window_update = seq_lt(snd_wl1_, s.sequence) ||
                             (snd_wl1_ == s.sequence && seq_le(snd_wl2_, s.acknowledgement));
  const uint32_t old_window = snd_wnd_;
  if (window_update) {
    snd_wnd_ = s.window;
    snd_wl1_ = s.sequence;
    snd_wl2_ = s.acknowledgement;
  }

  if (seq_gt(s.acknowledgement, snd_una_)) {
    uint32_t acked = s.acknowledgement - snd_una_;
    if (fin_sent_ && s.acknowledgement == snd_nxt_) {  // the FIN is acknowledged too
      fin_acked_ = true;
      --acked;
    }
    send_buffer_.discard(acked);
    snd_una_ = s.acknowledgement;
    sample_rtt(s.acknowledgement);
    retries_ = 0;
    duplicate_acks_ = 0;
    if (snd_una_ == snd_nxt_) timer_deadline_ = 0;
    else arm_timer();

    if (fin_acked_) {
      if (state_ == TcpState::FinWait1) enter(TcpState::FinWait2);
      else if (state_ == TcpState::Closing) enter(TcpState::TimeWait);
      else if (state_ == TcpState::LastAck) enter(TcpState::Closed);
    }
  } else if (s.acknowledgement == snd_una_ && s.length == 0 && snd_una_ != snd_nxt_ && s.window == old_window &&
             !(s.flags & kFin)) {
    if (++duplicate_acks_ == 3) {  // fast retransmit: the receiver keeps asking for the same byte
      rollback_for_retransmission();
      output();
    }
  }
}

void TcpSocket::accept_data(const TcpSegment& s) {
  using namespace tcp_flags;
  const bool can_receive = state_ == TcpState::Established || state_ == TcpState::FinWait1 || state_ == TcpState::FinWait2;
  const bool fin = s.flags & kFin;
  if (s.length == 0 && !fin) return;

  if (!can_receive) {
    // A FIN or data we have already accepted, retransmitted: repeat the ACK.
    if (fin || s.length) {
      send_ack();
      if (state_ == TcpState::TimeWait) time_wait_deadline_ = ip_.now_ms() + kTimeWaitMs;
    }
    return;
  }
  if (seq_gt(s.sequence, rcv_nxt_)) {  // a gap: the missing segment is on its way or lost
    send_ack();                        // a duplicate ACK tells the sender what we are waiting for
    return;
  }

  uint32_t skip = rcv_nxt_ - s.sequence;  // bytes of this segment we already have
  const uint32_t end_sequence = s.sequence + s.length;
  if (skip > s.length) skip = s.length;
  const uint8_t* data = s.data + skip;
  const uint32_t length = s.length - skip;
  const uint32_t stored = receive_buffer_.write(data, length);
  rcv_nxt_ += stored;

  if (fin && stored == length && end_sequence == rcv_nxt_) {
    ++rcv_nxt_;
    fin_received_ = true;
    if (state_ == TcpState::Established) enter(TcpState::CloseWait);
    else if (state_ == TcpState::FinWait1) enter(fin_acked_ ? TcpState::TimeWait : TcpState::Closing);
    else if (state_ == TcpState::FinWait2) enter(TcpState::TimeWait);
  }
  send_ack();
}

// ------------------------------------------------------------------ timers

void TcpSocket::sample_rtt(uint32_t acknowledged_up_to) {
  if (!timing_ || !seq_ge(acknowledged_up_to, timed_sequence_)) return;
  timing_ = false;
  const uint32_t rtt = static_cast<uint32_t>(ip_.now_ms() - timed_at_);
  if (!have_rtt_) {  // RFC 6298 (2.2)
    srtt_ms_ = rtt;
    rttvar_ms_ = rtt / 2;
    have_rtt_ = true;
  } else {           // (2.3)
    const uint32_t delta = srtt_ms_ > rtt ? srtt_ms_ - rtt : rtt - srtt_ms_;
    rttvar_ms_ = (3 * rttvar_ms_ + delta) / 4;
    srtt_ms_ = (7 * srtt_ms_ + rtt) / 8;
  }
  uint32_t rto = srtt_ms_ + 4 * rttvar_ms_;
  if (rto < kMinRtoMs) rto = kMinRtoMs;
  if (rto > kMaxRtoMs) rto = kMaxRtoMs;
  rto_ms_ = rto;
}

void TcpSocket::on_timer(uint64_t now) {
  if (state_ == TcpState::TimeWait) {
    if (now >= time_wait_deadline_) enter(TcpState::Closed);
    return;
  }
  if (state_ == TcpState::Closed || state_ == TcpState::Listen) return;

  if (timer_deadline_ && now >= timer_deadline_) {
    if (++retries_ > kMaxRetries) {  // the peer is gone
      send_segment(tcp_flags::kRst | tcp_flags::kAck, snd_nxt_, nullptr, 0, false);
      fail();
      return;
    }
    rto_ms_ = rto_ms_ * 2 > kMaxRtoMs ? kMaxRtoMs : rto_ms_ * 2;
    if (state_ == TcpState::SynSent) {
      send_segment(tcp_flags::kSyn, iss_, nullptr, 0, true);
    } else if (state_ == TcpState::SynReceived) {
      send_segment(tcp_flags::kSyn | tcp_flags::kAck, iss_, nullptr, 0, true);
    } else {
      rollback_for_retransmission();
      output();
    }
    arm_timer();
  }

  // Zero window: the peer's ACK that reopens it could be lost, so ask now and then
  // by sending one byte (which also gets an ACK carrying the current window).
  const bool stalled = (state_ == TcpState::Established || state_ == TcpState::CloseWait) && snd_wnd_ == 0 &&
                       snd_una_ == snd_nxt_ && send_buffer_.size() > 0;
  if (!stalled) {
    persist_deadline_ = 0;
  } else if (persist_deadline_ == 0) {
    persist_deadline_ = now + rto_ms_;
  } else if (now >= persist_deadline_) {
    uint8_t probe;
    send_buffer_.peek(0, &probe, 1);
    send_segment(tcp_flags::kAck, snd_nxt_, &probe, 1, false);
    ++snd_nxt_;
    arm_timer();
    persist_deadline_ = 0;
  }
}

}  // namespace network
