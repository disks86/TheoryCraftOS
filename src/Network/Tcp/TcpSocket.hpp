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

#pragma once
#include "Network/ByteRing.hpp"
#include "Network/Ip/IIpLayer.hpp"
#include "Network/Tcp/ITcpSocket.hpp"
#include "ObjectContainer/List.hpp"

namespace network {

class TcpManager;

namespace tcp_flags {
constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08, kAck = 0x10;
}

// A received TCP segment, parsed.
struct TcpSegment {
  uint32_t sequence;
  uint32_t acknowledgement;
  uint8_t flags;
  uint16_t window;
  ConstByteView data;  // the payload, a view into the received packet
  uint16_t mss;  // from the MSS option, 0 if absent
};

// Sequence numbers wrap at 2^32; comparisons are done on the signed difference.
inline bool seq_lt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
inline bool seq_le(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }
inline bool seq_gt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
inline bool seq_ge(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

// The default ITcpSocket: the RFC 793 state machine with the retransmission
// timer of RFC 6298 (smoothed RTT, exponential back-off, Karn's rule), fast
// retransmit after three duplicate ACKs, receive-window flow control and zero
// window probing, and congestion control (RFC 5681 slow start and congestion
// avoidance with appropriate byte counting, RFC 6582's recovery after a fast
// retransmit, RFC 6928's initial window of ten segments, the restart window after
// an idle period), so that connections sharing a bottleneck share it. Loss recovery
// is go-back-N and out-of-order segments are dropped (the sender retransmits them).
// A connection also holds back while kMaxQueuedFrames of its frames wait in the
// stack's transmit queue (IIpLayer::queued_for), so a bulk sender cannot fill that
// queue ahead of the others.
class TcpSocket final : public ITcpSocket {
 public:
  static constexpr uint32_t kBufferSize = 16384;  // each of the send and receive buffers
  static constexpr uint32_t kBacklog = 8;
  static constexpr uint32_t kMaxRetries = 8;
  static constexpr uint32_t kTimeWaitMs = 2000;
  // (enough for the queue to hold each busy connection through a burst of room the interface makes at once: with 2, the connection whose ACKs
  // came first took what the queue left, and two bulk connections through a bottleneck split it 3:1; 4 splits it evenly)
  static constexpr uint32_t kMaxQueuedFrames = 4;
  static constexpr uint32_t kMaxCongestionWindow = 1u << 20;

  TcpSocket(IIpLayer& ip, TcpManager& manager, allocators::IAllocator& allocator);
  ~TcpSocket();

  // --- ITcpSocket
  TcpState state() const override {
    StackLock guard(ip_);
    return state_;
  }
  bool connect(Ipv4Address address, uint16_t port) override { return connect_ip(IpAddress(address), port); }
  bool connect_ip(const IpAddress& address, uint16_t port) override;
  bool listen(uint16_t port) override { return listen_families(port, IpFamilies::Both); }
  bool listen_families(uint16_t port, IpFamilies families) override;
  SharedPointer<ITcpSocket> accept() override;
  uint32_t send(ConstByteView data) override;
  uint32_t receive(ByteView buffer) override;
  uint32_t available() const override {
    StackLock guard(ip_);
    return receive_buffer_.size();
  }
  uint32_t unsent_or_unacknowledged() const override {
    StackLock guard(ip_);
    return send_buffer_.size();
  }
  bool is_connected() const override {
    StackLock guard(ip_);
    return state_ == TcpState::Established || state_ == TcpState::CloseWait;
  }
  bool peer_closed() const override {
    StackLock guard(ip_);
    return fin_received_ && receive_buffer_.size() == 0;
  }
  bool failed() const override {
    StackLock guard(ip_);
    return failed_;
  }
  void close() override;
  void abort() override;
  uint16_t local_port() const override {
    StackLock guard(ip_);
    return local_port_;
  }
  Ipv4Address remote_address() const override {
    StackLock guard(ip_);
    return remote_.v6 ? kAnyAddress : remote_.ipv4;
  }
  IpAddress remote_ip() const override {
    StackLock guard(ip_);
    return remote_;
  }
  uint16_t remote_port() const override {
    StackLock guard(ip_);
    return remote_port_;
  }

  // Congestion control state, for tests and statistics.
  uint32_t congestion_window() const {
    StackLock guard(ip_);
    return cwnd_;
  }
  uint32_t slow_start_threshold() const {
    StackLock guard(ip_);
    return ssthresh_;
  }

  // --- used by TcpManager
  bool matches(const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port) const {
    return state_ != TcpState::Listen && state_ != TcpState::Closed && local_port_ == local_port &&
           remote_port_ == remote_port && remote_ == remote_address;
  }
  IpFamilies listen_families() const { return listen_families_; }  // a listener's: whose SYNs it takes
  void on_segment(const TcpSegment& segment);
  void on_timer(uint64_t now);
  // Becomes a connection in SynReceived for a SYN that arrived at `listener`.
  void accept_syn(TcpSocket* listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port,
                  const TcpSegment& syn, uint32_t initial_sequence);
  void prepare_to_connect(uint16_t local_port, uint32_t initial_sequence);
  bool take_established_event() {  // true once, when a child connection completes its handshake
    const bool e = established_event_;
    established_event_ = false;
    return e;
  }
  TcpSocket* listener() const { return listener_; }
  void detach_from_listener() { listener_ = nullptr; }
  void enqueue_accepted(SharedPointer<TcpSocket> child) { accept_queue_.push_back(child); }
  uint32_t embryo_count() const { return embryos_; }
  uint32_t accepted_waiting() const { return static_cast<uint32_t>(accept_queue_.count()); }
  void child_finished_handshake() {
    if (embryos_) --embryos_;
  }
  void add_embryo() { ++embryos_; }
  void drop_pending_connections();
  bool is_dead() const { return state_ == TcpState::Closed; }

 private:
  enum class Mode : uint8_t { Open, Closing };

  uint32_t advertised_window() const;
  void send_segment(uint8_t flags, uint32_t sequence, ConstByteView data, bool with_mss);
  // Data from the send buffer (`offset` bytes past snd_una_) as one large segment the interface cuts into frames of `mss` bytes.
  bool send_burst(uint8_t flags, uint32_t sequence, uint32_t offset, uint32_t length, uint16_t mss);
  void send_ack() { send_segment(tcp_flags::kAck, snd_nxt_, ConstByteView(), false); }
  void output();
  void process_ack(const TcpSegment& segment);
  void accept_data(const TcpSegment& segment);
  void enter(TcpState state);
  void rollback_for_retransmission();
  void arm_timer();
  void sample_rtt(uint32_t acknowledged_up_to);
  uint16_t send_mss() const { return peer_mss_ < our_mss() ? peer_mss_ : our_mss(); }
  uint32_t initial_window() const;
  void on_congestion(bool timeout);
  void grow_window(uint32_t acked);
  void fail();

  IIpLayer& ip_;
  TcpManager& manager_;
  allocators::IAllocator& allocator_;
  ByteView burst_;  // where a large segment is built (allocated when the first one is sent)
  TcpState state_ = TcpState::Closed;
  uint16_t local_port_ = 0;
  IpFamilies listen_families_ = IpFamilies::Both;
  IpAddress remote_;
  // The largest segment our side takes and sends: what fits a 1500-byte packet after the IP header of the connection's version.
  uint16_t our_mss() const { return static_cast<uint16_t>((remote_.v6 ? kMaxIpv6Payload : kMaxIpPayload) - 20); }
  uint16_t remote_port_ = 0;

  // Send side.
  uint32_t iss_ = 0, snd_una_ = 0, snd_nxt_ = 0, snd_wnd_ = 0, snd_wl1_ = 0, snd_wl2_ = 0;
  // The highest sequence number sent so far: go-back-N moves snd_nxt_ back, and the peer may then acknowledge what it had already received,
  // past snd_nxt_ but not past this.
  uint32_t snd_max_ = 0;
  void advance_next(uint32_t n) {
    snd_nxt_ += n;
    if (seq_gt(snd_nxt_, snd_max_)) snd_max_ = snd_nxt_;
  }
  ByteRing send_buffer_;      // bytes from snd_una_ on: unacknowledged first, then unsent
  bool close_requested_ = false;
  bool fin_sent_ = false;
  bool fin_acked_ = false;
  bool fin_ever_sent_ = false;   // (fin_sent_ is cleared by go-back-N; this is not: the FIN's sequence number is snd_max_ - 1)
  uint16_t peer_mss_ = 536;
  // Receive side.
  uint32_t irs_ = 0, rcv_nxt_ = 0;
  ByteRing receive_buffer_;
  bool fin_received_ = false;
  uint32_t last_advertised_window_ = 0;
  // Timers and loss recovery.
  uint64_t timer_deadline_ = 0;   // 0 = retransmission timer not running
  uint64_t persist_deadline_ = 0;
  uint64_t time_wait_deadline_ = 0;
  uint32_t rto_ms_ = 1000, srtt_ms_ = 0, rttvar_ms_ = 0;
  bool have_rtt_ = false;
  uint32_t retries_ = 0;
  uint32_t duplicate_acks_ = 0;
  bool timing_ = false;           // an RTT measurement is in progress
  uint32_t timed_sequence_ = 0;
  uint64_t timed_at_ = 0;
  // Congestion control (bytes). cwnd_ 0: not started (the initial window is set when the first data goes out, once the peer's MSS is known).
  uint32_t cwnd_ = 0, ssthresh_ = kMaxCongestionWindow, bytes_acked_ = 0;
  bool in_recovery_ = false;
  uint32_t recover_ = 0;          // snd_nxt_ when loss was detected: recovery ends when everything up to it is acknowledged
  uint64_t last_send_ms_ = 0;

  bool failed_ = false;
  bool established_event_ = false;
  // Listening sockets.
  TcpSocket* listener_ = nullptr;  // for a connection spawned by a listener, until accepted
  List<SharedPointer<TcpSocket>> accept_queue_;
  uint32_t embryos_ = 0;           // children still in SynReceived
};

}  // namespace network
