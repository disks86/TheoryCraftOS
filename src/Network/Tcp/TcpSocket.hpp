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
// window probing. Loss recovery is go-back-N, out-of-order segments are dropped
// (the sender retransmits them), and there is no congestion control: it is meant
// for local networks and modest transfers.
class TcpSocket final : public ITcpSocket {
 public:
  static constexpr uint32_t kBufferSize = 16384;  // each of the send and receive buffers
  static constexpr uint32_t kBacklog = 8;
  static constexpr uint32_t kMaxRetries = 8;
  static constexpr uint32_t kTimeWaitMs = 2000;

  TcpSocket(IIpLayer& ip, TcpManager& manager, allocators::IAllocator& allocator);

  // --- ITcpSocket
  TcpState state() const override {
    StackLock guard(ip_);
    return state_;
  }
  bool connect(Ipv4Address address, uint16_t port) override;
  bool listen(uint16_t port) override;
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
    return remote_address_;
  }
  uint16_t remote_port() const override {
    StackLock guard(ip_);
    return remote_port_;
  }

  // --- used by TcpManager
  bool matches(Ipv4Address remote_address, uint16_t remote_port, uint16_t local_port) const {
    return state_ != TcpState::Listen && state_ != TcpState::Closed && local_port_ == local_port &&
           remote_port_ == remote_port && remote_address_ == remote_address;
  }
  void on_segment(const TcpSegment& segment);
  void on_timer(uint64_t now);
  // Becomes a connection in SynReceived for a SYN that arrived at `listener`.
  void accept_syn(TcpSocket* listener, Ipv4Address remote_address, uint16_t remote_port, uint16_t local_port,
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
  void send_ack() { send_segment(tcp_flags::kAck, snd_nxt_, ConstByteView(), false); }
  void output();
  void process_ack(const TcpSegment& segment);
  void accept_data(const TcpSegment& segment);
  void enter(TcpState state);
  void rollback_for_retransmission();
  void arm_timer();
  void sample_rtt(uint32_t acknowledged_up_to);
  void fail();

  IIpLayer& ip_;
  TcpManager& manager_;
  TcpState state_ = TcpState::Closed;
  uint16_t local_port_ = 0;
  Ipv4Address remote_address_ = kAnyAddress;
  uint16_t remote_port_ = 0;

  // Send side.
  uint32_t iss_ = 0, snd_una_ = 0, snd_nxt_ = 0, snd_wnd_ = 0, snd_wl1_ = 0, snd_wl2_ = 0;
  ByteRing send_buffer_;      // bytes from snd_una_ on: unacknowledged first, then unsent
  bool close_requested_ = false;
  bool fin_sent_ = false;
  bool fin_acked_ = false;
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

  bool failed_ = false;
  bool established_event_ = false;
  // Listening sockets.
  TcpSocket* listener_ = nullptr;  // for a connection spawned by a listener, until accepted
  List<SharedPointer<TcpSocket>> accept_queue_;
  uint32_t embryos_ = 0;           // children still in SynReceived
};

}  // namespace network
