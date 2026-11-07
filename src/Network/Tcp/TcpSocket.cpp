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
#include "Utility/AllocatedView.hpp"
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

}  // namespace

TcpSocket::TcpSocket(IIpLayer& ip, TcpManager& manager, allocators::IAllocator& allocator)
    : ip_(ip), manager_(manager), allocator_(allocator), send_buffer_(allocator), receive_buffer_(allocator), accept_queue_(allocator) {}

TcpSocket::~TcpSocket() {
  release_view(allocator_, burst_);
  release_view(allocator_, ooo_storage_);
}

// ------------------------------------------------------------------ user API

bool TcpSocket::connect_ip(const IpAddress& address, uint16_t port) {
  StackLock guard(ip_);
  if (state_ != TcpState::Closed || failed_ || port == 0 || ip_.source_for_ip(address).is_unspecified()) return false;
  remote_ = address;
  remote_port_ = port;
  prepare_to_connect(manager_.allocate_port(), manager_.next_initial_sequence());
  if (failed_) return false;
  enter(TcpState::SynSent);
  send_segment(tcp_flags::kSyn | tcp_flags::kEce | tcp_flags::kCwr, iss_, ConstByteView(), true);   // (offers ECN, RFC 3168 6.1.1, and SACK)
  arm_timer();
  return true;
}

void TcpSocket::prepare_to_connect(uint16_t local_port, uint32_t initial_sequence) {
  local_port_ = local_port;
  iss_ = initial_sequence;
  snd_una_ = iss_;
  snd_nxt_ = iss_ + 1;  // the SYN occupies one sequence number
  snd_max_ = snd_nxt_;
  rto_ms_ = kInitialRtoMs;
  if (!send_buffer_.reserve(kBufferSize) || !receive_buffer_.reserve(kBufferSize)) failed_ = true;
}

bool TcpSocket::listen_families(uint16_t port, IpFamilies families) {
  StackLock guard(ip_);
  if (state_ != TcpState::Closed || failed_ || port == 0 || manager_.listening_on(port, families)) return false;
  local_port_ = port;
  listen_families_ = families;
  enter(TcpState::Listen);
  return true;
}

SharedPointer<ITcpSocket> TcpSocket::accept() {
  StackLock guard(ip_);
  if (state_ != TcpState::Listen || accept_queue_.empty()) return SharedPointer<ITcpSocket>();
  SharedPointer<TcpSocket> child = accept_queue_[0];
  accept_queue_.remove_at(0);
  return SharedPointer<ITcpSocket>(child);
}

uint32_t TcpSocket::send(ConstByteView data) {
  StackLock guard(ip_);
  const bool can_send = state_ == TcpState::Established || state_ == TcpState::CloseWait ||
                        state_ == TcpState::SynSent || state_ == TcpState::SynReceived;
  if (!can_send || close_requested_) return 0;
  const uint32_t queued = send_buffer_.write(data);
  output();
  return queued;
}

uint32_t TcpSocket::receive(ByteView buffer) {
  StackLock guard(ip_);
  const uint32_t n = receive_buffer_.read(buffer);
  // The sender may be waiting on a closed window: tell it once there is room again.
  if (n && last_advertised_window_ < our_mss() && advertised_window() >= our_mss() &&
      (state_ == TcpState::Established || state_ == TcpState::FinWait1 || state_ == TcpState::FinWait2))
    send_ack();
  return n;
}

void TcpSocket::close() {
  StackLock guard(ip_);
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
  StackLock guard(ip_);
  if (state_ == TcpState::Listen) {
    close();
    return;
  }
  if (state_ != TcpState::Closed && state_ != TcpState::SynSent && state_ != TcpState::TimeWait)
    send_segment(tcp_flags::kRst | tcp_flags::kAck, snd_nxt_, ConstByteView(), false);
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

void TcpSocket::send_segment(uint8_t flags, uint32_t sequence, ConstByteView data, bool with_mss) {
  using namespace tcp_flags;
  // Options, a multiple of four bytes: the MSS on a SYN (with SACK-permitted when this side offers it or answers the peer's offer), else
  // SACK blocks when this side holds data beyond a gap - as many as the packet has room for, the latest segment's range first.
  uint8_t options[40];
  ByteWriter opt{ByteView(options)};
  if (with_mss) {
    opt.u8(2);
    opt.u8(4);
    opt.be16(our_mss());
    if (!(flags & kAck) || sack_ok_) {
      opt.u8(1);
      opt.u8(1);
      opt.u8(4);
      opt.u8(2);
    }
  } else if (sack_ok_ && !held_.empty() && (flags & kAck)) {
    const uint32_t room = ip_.max_payload_for(remote_) - 20 - static_cast<uint32_t>(data.size());
    uint32_t blocks = room >= 4 ? (room - 4) / 8 : 0;
    if (blocks > 4) blocks = 4;
    if (blocks > held_.count()) blocks = held_.count();
    if (blocks) {
      opt.u8(1);
      opt.u8(1);
      opt.u8(5);
      opt.u8(static_cast<uint8_t>(2 + 8 * blocks));
      const SequenceRanges::Range* latest = held_.find(latest_held_);
      uint32_t written = 0;
      if (latest) {
        opt.be32(latest->start);
        opt.be32(latest->end);
        ++written;
      }
      for (uint32_t r = 0; r < held_.count() && written < blocks; ++r) {
        if (&held_.at(r) == latest) continue;
        opt.be32(held_.at(r).start);
        opt.be32(held_.at(r).end);
        ++written;
      }
    }
  }
  if (!opt.ok()) return;
  const ConstByteView option_bytes = opt.written();
  const uint32_t header = 20 + static_cast<uint32_t>(option_bytes.size());
  // ECN: new data is ECN-capable (ECT(0)); retransmissions, pure ACKs and control segments are not (RFC 3168 6.1.4, 6.1.5).
  const bool fresh_data = !data.empty() && !seq_lt(sequence, snd_max_);
  if (ece_pending_ && (flags & kAck) && !(flags & kSyn)) flags |= kEce;
  if (cwr_pending_ && fresh_data) {
    flags |= kCwr;
    cwr_pending_ = false;
  }
  const uint8_t ecn = ecn_ok_ && fresh_data ? kEcnEct0 : 0;

  uint8_t storage[kMaxIpPayload];
  ByteWriter out{ByteView(storage)};
  last_advertised_window_ = advertised_window();
  out.be16(local_port_);
  out.be16(remote_port_);
  out.be32(sequence);
  out.be32((flags & kAck) ? rcv_nxt_ : 0);
  out.u8(static_cast<uint8_t>((header / 4) << 4));
  out.u8(flags);
  out.be16(static_cast<uint16_t>(last_advertised_window_));
  out.be16(0);  // checksum: the IP layer fills it in
  out.be16(0);  // urgent pointer (unused)
  out.bytes(option_bytes);
  out.bytes(data);
  if (!out.ok()) return;  // payload larger than a packet: cannot happen, the sender chunks to the MSS (less the room options take)

  ip_.send_ip_to(remote_, kProtocolTcp, out.written(), ecn);  // a lost segment is retransmitted
}

bool TcpSocket::send_burst(uint8_t flags, uint32_t sequence, uint32_t offset, uint32_t length, uint16_t mss) {
  if (burst_.empty()) burst_ = allocate_view<uint8_t>(allocator_, uint64_t{kBufferSize} + 20);
  if (burst_.size() < uint64_t{length} + 20) return false;
  last_advertised_window_ = advertised_window();
  ByteWriter out{burst_};
  out.be16(local_port_);
  out.be16(remote_port_);
  out.be32(sequence);
  out.be32(rcv_nxt_);
  const bool fresh_data = !seq_lt(sequence, snd_max_);
  if (ece_pending_) flags |= tcp_flags::kEce;
  if (cwr_pending_ && fresh_data) {
    flags |= tcp_flags::kCwr;   // (the interface copies the header to every frame: harmless, the peer acts on the first)
    cwr_pending_ = false;
  }
  out.u8(5 << 4);
  out.u8(flags);
  out.be16(static_cast<uint16_t>(last_advertised_window_));
  out.be16(0);  // checksum: the IP layer fills it in
  out.be16(0);
  if (!out.ok()) return false;
  send_buffer_.peek(offset, burst_.subview(20, length));
  return ip_.send_tcp_segmented(remote_, burst_.first(20 + length), mss, ecn_ok_ && fresh_data ? kEcnEct0 : 0);
}

// Sends whatever the peer's window and our send buffer allow, then the FIN if
// the application has closed and everything is out. Where the interface cuts segments into frames (segmentation offload), what fits goes
// in one large segment instead of one per MSS.
uint32_t TcpSocket::initial_window() const {
  const uint32_t mss = send_mss();
  const uint32_t ten = 10 * mss, floor = 2 * mss > 14600 ? 2 * mss : 14600;   // RFC 6928: min(10*MSS, max(2*MSS, 14600))
  return ten < floor ? ten : floor;
}

// Loss: a fast retransmit (three duplicate ACKs) halves the window; a retransmission timeout starts again from one segment.
void TcpSocket::on_congestion(bool timeout) {
  const uint32_t mss = send_mss();
  const uint32_t flight = snd_nxt_ - snd_una_;
  ssthresh_ = flight / 2 > 2 * mss ? flight / 2 : 2 * mss;   // RFC 5681 (4)
  cwnd_ = timeout ? mss : ssthresh_;
  bytes_acked_ = 0;
  in_recovery_ = !timeout;
  recover_ = snd_nxt_;
}

// New data acknowledged: slow start below ssthresh_ (at most two segments per ACK, RFC 3465), one segment per window's worth above it.
void TcpSocket::grow_window(uint32_t acked) {
  const uint32_t mss = send_mss();
  if (cwnd_ == 0) return;
  if (in_recovery_) {
    if (!seq_ge(snd_una_, recover_)) return;   // a partial ACK: still recovering (go-back-N already resends the rest)
    in_recovery_ = false;
    retransmitted_ = 0;
    cwnd_ = ssthresh_;                          // RFC 6582: the window after recovery
    return;
  }
  if (cwnd_ < ssthresh_) {
    cwnd_ += acked < 2u * mss ? acked : 2u * mss;
  } else {
    bytes_acked_ += acked;
    if (bytes_acked_ >= cwnd_) {
      bytes_acked_ -= cwnd_;
      cwnd_ += mss;
    }
  }
  if (cwnd_ > kMaxCongestionWindow) cwnd_ = kMaxCongestionWindow;
}

// The bytes in flight as the network sees them: in SACK recovery, those sent less those the peer holds and those taken as lost (below the
// highest SACKed byte and not SACKed), plus what was retransmitted (RFC 6675's "pipe", by bytes); otherwise everything unacknowledged.
uint32_t TcpSocket::pipe() const {
  const uint32_t in_flight = snd_nxt_ - snd_una_ - (fin_sent_ ? 1 : 0);
  if (!sack_recovery() || sacked_.empty()) return in_flight;
  const uint32_t sacked = sacked_.bytes();
  const uint32_t below_highest = sacked_.highest(snd_una_) - snd_una_;
  const uint32_t lost = below_highest > sacked ? below_highest - sacked : 0;
  const uint64_t out = uint64_t{in_flight} + retransmitted_;
  const uint64_t gone = uint64_t{sacked} + lost;
  return out > gone ? static_cast<uint32_t>(out - gone) : 0;
}

// Retransmits the holes the scoreboard shows, a segment at a time, while the congestion window has room. True if any went.
bool TcpSocket::retransmit_holes(uint32_t mss) {
  bool any = false;
  const uint32_t highest = sacked_.highest(snd_una_);
  for (uint32_t guard = 0; guard < 64 && pipe() < cwnd_; ++guard) {
    uint32_t from = seq_lt(retransmit_next_, snd_una_) ? snd_una_ : retransmit_next_;
    uint32_t gap_start = 0, gap_end = 0;
    if (!sacked_.next_gap(from, highest, &gap_start, &gap_end)) break;
    uint32_t n = gap_end - gap_start;
    if (n > mss) n = mss;
    uint8_t chunk[kMaxIpPayload];
    const ByteView payload = ByteView(chunk).first(n);
    send_buffer_.peek(gap_start - snd_una_, payload);
    send_segment(tcp_flags::kAck, gap_start, payload, false);
    retransmit_next_ = gap_start + n;
    retransmitted_ += n;
    timing_ = false;   // (Karn)
    any = true;
  }
  if (any && !timer_deadline_) arm_timer();
  return any;
}

// What pacing lets out now (~0: no pacing - the round trip is shorter than the clock's millisecond, so there is nothing to spread). The rate
// is twice the congestion window per smoothed round trip in slow start and 1.2 times it after; up to two milliseconds' worth (at least four
// segments) may build up for a burst.
uint32_t TcpSocket::pacing_allowance(uint64_t now) {
  if (!have_rtt_ || srtt_ms_ == 0) return ~0u;
  const uint64_t per_ms = (uint64_t{cwnd_} * (cwnd_ < ssthresh_ ? 10 : 6)) / (5 * uint64_t{srtt_ms_});
  uint64_t cap = per_ms * 2;
  if (cap < 4u * send_mss()) cap = 4u * send_mss();
  if (now > paced_at_ms_) pacing_credit_ += per_ms * (now - paced_at_ms_);
  paced_at_ms_ = now;
  if (pacing_credit_ > cap) pacing_credit_ = cap;
  return pacing_credit_ > 0xFFFFFFFEu ? 0xFFFFFFFEu : static_cast<uint32_t>(pacing_credit_);
}

void TcpSocket::output() {
  const bool may_send_data = state_ == TcpState::Established || state_ == TcpState::CloseWait ||
                             state_ == TcpState::FinWait1 || state_ == TcpState::LastAck;
  if (!may_send_data) return;

  const uint16_t mss = send_mss();
  const uint32_t burst = ip_.max_segmented_payload(remote_);
  const uint64_t now = ip_.now_ms();
  if (cwnd_ == 0) {
    cwnd_ = initial_window();
  } else if (snd_nxt_ == snd_una_ && last_send_ms_ && now - last_send_ms_ > rto_ms_) {   // idle for longer than an RTO: RFC 5681 4.1
    const uint32_t restart = initial_window();
    if (cwnd_ > restart) cwnd_ = restart;
  }
  paced_out_ = false;
  // In loss recovery with SACK the holes go first (only they: what the peer holds is not sent again).
  if (sack_recovery()) retransmit_holes(mss);
  uint32_t allowance = pacing_allowance(now);
  for (;;) {
    const uint32_t in_flight = snd_nxt_ - snd_una_ - (fin_sent_ ? 1 : 0);  // data only
    if (fin_sent_) break;  // nothing follows a FIN
    const uint32_t unsent = send_buffer_.size() > in_flight ? send_buffer_.size() - in_flight : 0;
    // The receiver's window counts from snd_una_; the network's from what is really in flight (in SACK recovery: less what arrived or was lost).
    const uint32_t pipe_now = pipe();
    const uint32_t receiver_room = snd_wnd_ > in_flight ? snd_wnd_ - in_flight : 0;
    const uint32_t network_room = cwnd_ > pipe_now ? cwnd_ - pipe_now : 0;
    // (a zero receive window is probed by on_timer; a congestion window smaller than a segment still lets one segment out)
    uint32_t window_room = receiver_room < network_room ? receiver_room : network_room;
    if (window_room == 0 && in_flight == 0 && snd_wnd_ > 0) window_room = mss;
    uint32_t n = unsent < window_room ? unsent : window_room;
    if (n == 0) break;
    if (ip_.queued_for(remote_, local_port_, remote_port_) >= kMaxQueuedFrames) break;   // its frames wait for the interface: hold back
    if (n > allowance) {   // the pacing rate allows less now: whole segments of it, or wait (on_timer goes on)
      if (allowance < mss && allowance < unsent) {
        paced_out_ = true;
        break;
      }
      n = allowance > mss ? allowance - allowance % mss : allowance;
    }
    if (n > mss && burst > 20u + mss) {  // several segments' worth, cut by the interface
      if (n > burst - 20) n = burst - 20;
      if (n > kBufferSize) n = kBufferSize;
      const bool last = n == unsent;
      send_burst(tcp_flags::kAck | (last ? tcp_flags::kPsh : 0), snd_nxt_, in_flight, n, mss);  // (a lost one is retransmitted)
    } else {
      if (n > mss) n = mss;
      uint8_t chunk[kMaxIpPayload];
      const ByteView payload = ByteView(chunk).first(n);  // n <= mss <= kMaxIpPayload by construction; checked here
      send_buffer_.peek(in_flight, payload);
      const bool last = n == unsent;
      send_segment(tcp_flags::kAck | (last ? tcp_flags::kPsh : 0), snd_nxt_, payload, false);
    }
    if (!timing_ && snd_nxt_ == snd_max_) {  // time this segment (Karn: never a retransmission - after a go-back-N, data below snd_max_ is one)
      timing_ = true;
      timed_sequence_ = snd_nxt_ + n;
      timed_at_ = ip_.now_ms();
    }
    if (allowance != ~0u) {
      allowance -= n;
      pacing_credit_ -= n;
    }
    advance_next(n);
    last_send_ms_ = now;
    if (!timer_deadline_) arm_timer();
  }

  // The FIN goes out once every queued byte has been sent.
  const uint32_t in_flight = snd_nxt_ - snd_una_;
  if (close_requested_ && !fin_sent_ && send_buffer_.size() == in_flight) {
    send_segment(tcp_flags::kFin | tcp_flags::kAck, snd_nxt_, ConstByteView(), false);
    fin_sent_ = fin_ever_sent_ = true;
    advance_next(1);
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

void TcpSocket::accept_syn(TcpSocket* listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port,
                           const TcpSegment& syn, uint32_t initial_sequence) {
  listener_ = listener;
  remote_ = remote_address;
  remote_port_ = remote_port;
  prepare_to_connect(local_port, initial_sequence);
  if (failed_) {
    listener_ = nullptr;
    return;
  }
  irs_ = syn.sequence;
  rcv_nxt_ = irs_ + 1;
  if (syn.mss) peer_mss_ = syn.mss;
  sack_ok_ = syn.sack_permitted;
  ecn_ok_ = (syn.flags & tcp_flags::kEce) && (syn.flags & tcp_flags::kCwr);   // an ECN-setup SYN: answered with ECE (RFC 3168 6.1.1)
  snd_wnd_ = syn.window;
  snd_wl1_ = syn.sequence;
  snd_wl2_ = 0;
  enter(TcpState::SynReceived);
  send_segment(tcp_flags::kSyn | tcp_flags::kAck | (ecn_ok_ ? tcp_flags::kEce : 0), iss_, ConstByteView(), true);
  arm_timer();
}

void TcpSocket::on_segment(const TcpSegment& s) {
  using namespace tcp_flags;
  if (state_ == TcpState::Closed || state_ == TcpState::Listen) return;

  if (state_ == TcpState::SynSent) {
    if (s.flags & kAck) {
      if (seq_le(s.acknowledgement, iss_) || seq_gt(s.acknowledgement, snd_nxt_)) {
        if (!(s.flags & kRst)) send_segment(kRst, s.acknowledgement, ConstByteView(), false);
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
    sack_ok_ = s.sack_permitted;                                  // (this side offered it on its SYN)
    ecn_ok_ = (s.flags & kEce) && !(s.flags & kCwr);              // RFC 3168 6.1.1: an ECN-setup SYN-ACK
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
      ecn_ok_ = false;
      send_segment(kSyn | kAck, iss_, ConstByteView(), true);
    }
    return;
  }

  // From here on the connection is synchronized.
  if (s.flags & kRst) {
    // In TIME-WAIT the connection is over (both FINs exchanged and acknowledged): a reset there is the answer of a peer that already forgot
    // it - say to a duplicated last ACK - and is ignored (RFC 1337), not reported as a failure.
    if (state_ == TcpState::TimeWait) return;
    // Only a reset whose sequence number falls in our receive window counts.
    if (seq_ge(s.sequence, rcv_nxt_) && seq_lt(s.sequence, rcv_nxt_ + advertised_window() + 1)) fail();
    return;
  }
  if (s.flags & kSyn) {  // a retransmitted SYN: answer again; anything else is ignored
    if (state_ == TcpState::SynReceived && s.sequence == irs_) send_segment(kSyn | kAck | (ecn_ok_ ? kEce : 0), iss_, ConstByteView(), true);
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
      send_segment(kRst, s.acknowledgement, ConstByteView(), false);
      return;
    }
  }

  process_ack(s);
  if (ecn_ok_) {   // (RFC 3168 6.1.3: CWR ends the echo, a CE mark on data starts it)
    if (s.flags & kCwr) ece_pending_ = false;
    if (s.congestion_experienced && !s.data.empty()) ece_pending_ = true;
  }
  accept_data(s);
  output();
}

void TcpSocket::process_ack(const TcpSegment& s) {
  using namespace tcp_flags;
  if (seq_gt(s.acknowledgement, snd_max_)) {  // acknowledges something we have not sent
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
    if (fin_ever_sent_ && s.acknowledgement == snd_max_) {  // the FIN is acknowledged too
      fin_acked_ = fin_sent_ = true;
      --acked;
    }
    send_buffer_.discard(acked);
    snd_una_ = s.acknowledgement;
    if (seq_gt(snd_una_, snd_nxt_)) snd_nxt_ = snd_una_;   // (data sent before a go-back-N had arrived after all)
    note_sack(s);
    grow_window(acked);
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
  } else if (s.acknowledgement == snd_una_ && snd_una_ != snd_nxt_ &&
             ((s.data.empty() && s.window == old_window && !(s.flags & kFin)) || adds_sack(s))) {
    // A duplicate ACK: RFC 5681's (nothing new, the same window), or - with SACK - one that says the peer holds more than it did (RFC 6675's
    // definition: a receiver whose application reads meanwhile changes its window on every ACK, and would never send the first kind).
    note_sack(s);
    ++duplicate_acks_;
    // Fast retransmit: the receiver kept asking for the same byte three times - or, with SACK, holds three segments' worth beyond it (RFC 6675's
    // loss rule: what the receiver coalesced into one segment, and so acknowledged once, still counts by what it holds).
    const bool lost = duplicate_acks_ == 3 || (sack_ok_ && !in_recovery_ && duplicate_acks_ < 3 && sacked_.bytes() >= 3u * send_mss());
    if (lost) {
      duplicate_acks_ = 3;
      const bool was_recovering = in_recovery_;
      if (!in_recovery_) on_congestion(false);
      if (sack_ok_) {   // only the holes (output() does them first), from snd_una_ on
        if (!was_recovering) {
          retransmit_next_ = snd_una_;
          retransmitted_ = 0;
        }
      } else {
        rollback_for_retransmission();
      }
      output();
    } else if (sack_recovery()) {
      output();   // (each further duplicate ACK can make room: more holes, or new data)
    }
  } else {
    note_sack(s);
  }
  // ECN-Echo: congestion on the way - cut the window as for a loss, once per window of data, without retransmitting anything (RFC 3168 6.1.2).
  // (strictly past ecn_recover_: an ECE on the ACK of the window the cut was for is the same congestion, echoed until the peer saw CWR)
  if (ecn_ok_ && (s.flags & kEce) && !in_recovery_ && (!ecn_cut_ || seq_gt(snd_una_, ecn_recover_))) {
    const uint32_t mss = send_mss();
    const uint32_t flight = snd_nxt_ - snd_una_;
    ssthresh_ = flight / 2 > 2 * mss ? flight / 2 : 2 * mss;
    if (cwnd_ > ssthresh_) cwnd_ = ssthresh_;
    bytes_acked_ = 0;
    ecn_recover_ = snd_max_;
    ecn_cut_ = true;
    cwr_pending_ = true;
  }
}

void TcpSocket::accept_data(const TcpSegment& s) {
  using namespace tcp_flags;
  const bool can_receive = state_ == TcpState::Established || state_ == TcpState::FinWait1 || state_ == TcpState::FinWait2;
  const bool fin = s.flags & kFin;
  if (s.data.empty() && !fin) return;

  if (!can_receive) {
    // A FIN or data we have already accepted, retransmitted: repeat the ACK.
    if (fin || !s.data.empty()) {
      send_ack();
      if (state_ == TcpState::TimeWait) time_wait_deadline_ = ip_.now_ms() + kTimeWaitMs;
    }
    return;
  }
  if (seq_gt(s.sequence, rcv_nxt_)) {  // a gap: the missing segment is on its way or lost
    if (!s.data.empty()) accept_out_of_order(s.sequence, s.data);   // kept until the gap fills (a FIN beyond a gap waits for its retransmission)
    send_ack();                        // a duplicate ACK (with SACK blocks) tells the sender what we are waiting for, and what we have
    return;
  }

  uint32_t skip = rcv_nxt_ - s.sequence;  // bytes of this segment we already have
  const uint32_t end_sequence = s.sequence + static_cast<uint32_t>(s.data.size());
  if (skip > s.data.size()) skip = static_cast<uint32_t>(s.data.size());
  const ConstByteView fresh = s.data.dropped(skip);  // the part we do not have yet
  const uint32_t length = static_cast<uint32_t>(fresh.size());
  const uint32_t stored = receive_buffer_.write(fresh);
  rcv_nxt_ += stored;

  if (fin && stored == length && end_sequence == rcv_nxt_) {
    ++rcv_nxt_;
    fin_received_ = true;
    held_.clear();   // (nothing comes after a FIN)
    if (state_ == TcpState::Established) enter(TcpState::CloseWait);
    else if (state_ == TcpState::FinWait1) enter(fin_acked_ ? TcpState::TimeWait : TcpState::Closing);
    else if (state_ == TcpState::FinWait2) enter(TcpState::TimeWait);
  } else {
    take_reassembled();
  }
  send_ack();
}

// Data past a gap: kept (within the receive window, while there are ranges to spare) in ooo_storage_, at its sequence number modulo the
// buffer size.
void TcpSocket::accept_out_of_order(uint32_t sequence, ConstByteView data) {
  const uint32_t window_end = rcv_nxt_ + receive_buffer_.free_space();
  uint32_t end = sequence + static_cast<uint32_t>(data.size());
  if (!seq_lt(sequence, window_end)) return;
  if (seq_gt(end, window_end)) end = window_end;
  if (ooo_storage_.empty()) ooo_storage_ = allocate_view<uint8_t>(allocator_, kBufferSize);
  if (ooo_storage_.empty() || !held_.add(sequence, end)) return;
  const uint32_t n = end - sequence;
  for (uint32_t i = 0; i < n; ++i) ooo_storage_[(sequence + i) % kBufferSize] = data[i];
  latest_held_ = sequence;
}

// After rcv_nxt_ moved: what was held from there on goes into the receive buffer (as much as fits) and is acknowledged with it.
void TcpSocket::take_reassembled() {
  held_.remove_below(rcv_nxt_);
  while (!held_.empty() && held_.at(0).start == rcv_nxt_ && receive_buffer_.free_space() > 0) {
    const uint32_t available = held_.at(0).end - rcv_nxt_;
    uint8_t chunk[512];
    uint32_t n = available < sizeof chunk ? available : static_cast<uint32_t>(sizeof chunk);
    for (uint32_t i = 0; i < n; ++i) chunk[i] = ooo_storage_[(rcv_nxt_ + i) % kBufferSize];
    n = receive_buffer_.write(ConstByteView(chunk).first(n));
    if (n == 0) break;
    rcv_nxt_ += n;
    held_.remove_below(rcv_nxt_);
  }
}

// The ACK brings SACK blocks that cover something the scoreboard does not have yet.
bool TcpSocket::adds_sack(const TcpSegment& s) const {
  if (!sack_ok_) return false;
  for (uint32_t b = 0; b < s.sack_blocks; ++b) {
    const uint32_t left = s.sack[b][0], right = s.sack[b][1];
    if (seq_le(left, snd_una_) || seq_gt(right, snd_max_) || !seq_lt(left, right)) continue;
    uint32_t gap_start, gap_end;
    if (sacked_.next_gap(left, right, &gap_start, &gap_end)) return true;
  }
  return false;
}

// The peer's SACK blocks (those within what was sent and not yet acknowledged) go on the scoreboard.
void TcpSocket::note_sack(const TcpSegment& s) {
  if (!sack_ok_) return;
  for (uint32_t b = 0; b < s.sack_blocks; ++b) {
    const uint32_t left = s.sack[b][0], right = s.sack[b][1];
    if (seq_le(left, snd_una_) || seq_gt(right, snd_max_) || !seq_lt(left, right)) continue;   // (stale, or claims what was never sent)
    sacked_.add(left, right);
  }
  sacked_.remove_below(snd_una_);
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
      send_segment(tcp_flags::kRst | tcp_flags::kAck, snd_nxt_, ConstByteView(), false);
      fail();
      return;
    }
    rto_ms_ = rto_ms_ * 2 > kMaxRtoMs ? kMaxRtoMs : rto_ms_ * 2;
    if (state_ == TcpState::SynSent) {
      send_segment(tcp_flags::kSyn | tcp_flags::kEce | tcp_flags::kCwr, iss_, ConstByteView(), true);
    } else if (state_ == TcpState::SynReceived) {
      send_segment(tcp_flags::kSyn | tcp_flags::kAck | (ecn_ok_ ? tcp_flags::kEce : 0), iss_, ConstByteView(), true);
    } else {
      if (cwnd_) on_congestion(true);
      sacked_.clear();   // (after a timeout the scoreboard is not trusted: go-back-N, RFC 2018 8)
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
    send_buffer_.peek(0, ByteView(&probe, 1));
    send_segment(tcp_flags::kAck, snd_nxt_, ConstByteView(&probe, 1), false);
    advance_next(1);
    arm_timer();
    persist_deadline_ = 0;
  }

  // Data that held back while its frames waited in the stack's transmit queue (see output()) goes once they have gone out.
  const uint32_t in_flight = snd_nxt_ - snd_una_ - (fin_sent_ ? 1 : 0);
  if (!fin_sent_ && send_buffer_.size() > in_flight && snd_wnd_ > 0) output();
}

}  // namespace network
