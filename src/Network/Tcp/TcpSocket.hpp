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
#include "Network/RcuSocketTable.hpp"
#include "Network/Tcp/SequenceRanges.hpp"
#include "Network/Tcp/ITcpSocket.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

class TcpManager;

namespace tcp_flags {
constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08, kAck = 0x10, kEce = 0x40, kCwr = 0x80;
}

// A received TCP segment, parsed.
struct TcpSegment {
  uint32_t sequence;
  uint32_t acknowledgement;
  uint8_t flags;
  uint16_t window;
  ConstByteView data;  // the payload, a view into the received packet
  uint16_t mss;  // from the MSS option, 0 if absent
  bool sack_permitted;   // the SACK-permitted option (RFC 2018), on a SYN
  uint8_t sack_blocks;   // SACK blocks carried (at most 4)
  uint32_t sack[4][2];   // their [left edge, right edge)
  bool congestion_experienced;   // the IP header said CE (RFC 3168): a router (or this machine's queue) was congested
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
// an idle period), so that connections sharing a bottleneck share it.
//
// Segments that arrive out of order are kept (up to SequenceRanges::kMax pieces
// within the receive window) and the receiver says what it holds with SACK blocks
// (RFC 2018); a sender whose peer does too retransmits only the holes during loss
// recovery (in the spirit of RFC 6675: the bytes in flight counted without what was
// SACKed or lost), and falls back to go-back-N after a timeout or without SACK.
// ECN (RFC 3168) is negotiated on the SYN: new data then goes out ECN-capable, a
// congestion mark on the way (or in this machine's own transmit queue, which marks
// instead of dropping) is echoed back, and the sender cuts its window as for a loss,
// once per window, without a retransmission. Data is paced at twice the congestion
// window per round trip in slow start and 1.2 times it after (as Linux does), so a
// window does not leave as one burst on a long path.
// A connection also holds back while kMaxQueuedFrames of its frames wait in the
// stack's transmit queue (IIpLayer::queued_for), so a bulk sender cannot fill that
// queue ahead of the others.
//
// Concurrency within one connection (after Linux's socket ownership): the protocol
// state is guarded by lock_, but nobody waits for it on the hot paths. The
// application's send() and receive() only take the buffer's own leaf lock and copy;
// the protocol work they cause (output, a window update) is done at once if lock_ is
// free, else flagged in pending_ for whoever holds it. A segment that arrives while
// lock_ is held (by the application, a timer or another receive queue's thread) is
// copied to the backlog and processed by the holder before it lets go, so a receive
// thread never waits for a connection; timers that find it busy are flagged the same
// way. Every hold of lock_ goes through Owner, whose release runs that deferred work,
// and status reads (state, failed, ports, buffer levels) take no lock_ at all.
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
  // Bytes of segments the backlog keeps while the connection is busy (one segment is always taken, however large: a coalesced one may be).
  static constexpr uint32_t kBacklogBytes = 4 * kBufferSize;

  // Holds the connection's protocol lock (lock_); letting go runs the work others left for the holder (see the class comment). Taken before
  // the manager's table lock, a listener's before its connections'.
  class Owner {
   public:
    explicit Owner(TcpSocket& socket) : socket_(socket) { socket_.lock_.lock(); }
    ~Owner() { socket_.release(); }
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

   private:
    TcpSocket& socket_;
  };

  TcpSocket(IIpLayer& ip, TcpManager& manager, allocators::IAllocator& allocator);
  ~TcpSocket();

  // --- ITcpSocket
  TcpState state() const override { return __atomic_load_n(&state_, __ATOMIC_ACQUIRE); }
  bool connect(Ipv4Address address, uint16_t port) override { return connect_ip(IpAddress(address), port); }
  bool connect_ip(const IpAddress& address, uint16_t port) override;
  bool listen(uint16_t port) override { return listen_families(port, IpFamilies::Both); }
  bool listen_families(uint16_t port, IpFamilies families) override;
  SharedPointer<ITcpSocket> accept() override;
  uint32_t send(ConstByteView data) override;
  uint32_t receive(ByteView buffer) override;
  uint32_t available() const override {
    kernel::ScopedMutex guard(receive_lock_);
    return receive_buffer_.size();
  }
  uint32_t unsent_or_unacknowledged() const override {
    kernel::ScopedMutex guard(send_lock_);
    return send_buffer_.size();
  }
  bool is_connected() const override {
    const TcpState now = state();
    return now == TcpState::Established || now == TcpState::CloseWait;
  }
  bool peer_closed() const override {
    kernel::ScopedMutex guard(receive_lock_);
    return fin_received_ && receive_buffer_.size() == 0;
  }
  bool failed() const override { return __atomic_load_n(&failed_, __ATOMIC_ACQUIRE); }
  void close() override;
  void abort() override;
  uint16_t local_port() const override { return __atomic_load_n(&local_port_, __ATOMIC_ACQUIRE); }
  Ipv4Address remote_address() const override {
    const IpAddress remote = remote_ip();
    return remote.v6 ? kAnyAddress : remote.ipv4;
  }
  IpAddress remote_ip() const override {
    kernel::ScopedMutex guard(remote_lock_);
    return remote_;
  }
  uint16_t remote_port() const override { return __atomic_load_n(&remote_port_, __ATOMIC_ACQUIRE); }

  // Congestion control state, for tests and statistics.
  // Data waits only for the pacing rate (the stack polls again soon then, whether or not frames come).
  bool pacing_paused() const { return __atomic_load_n(&paced_out_, __ATOMIC_RELAXED); }   // (read by the manager without this socket's lock)
  bool sack_permitted() {
    Owner owner(*this);
    return sack_ok_;
  }
  bool ecn_capable() {
    Owner owner(*this);
    return ecn_ok_;
  }
  uint32_t congestion_window() {
    Owner owner(*this);
    return cwnd_;
  }
  uint32_t slow_start_threshold() {
    Owner owner(*this);
    return ssthresh_;
  }
  // Segments that found the connection busy and went to the backlog, and those dropped because it was full (for tests and statistics).
  uint64_t backlogged_segments() const { return __atomic_load_n(&backlogged_, __ATOMIC_RELAXED); }
  uint64_t backlog_drops() const { return __atomic_load_n(&backlog_drops_, __ATOMIC_RELAXED); }

  // --- used by TcpManager (which takes no socket's lock while it holds its table's)
  // A segment for this connection: processed now, or kept in the backlog for the thread that holds the connection. `self` is the table's
  // reference to this socket (handed to the listener when a connection it spawned is established). Never waits for lock_.
  void on_segment(const TcpSegment& segment, const SharedPointer<TcpSocket>& self);
  // The timers' turn (never waits either: a busy connection runs them when its holder lets go).
  void on_timer(uint64_t now);
  // Becomes a connection in SynReceived for a SYN that arrived at `listener` (whose lock the caller holds). False if out of memory.
  bool accept_syn(TcpSocket* listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port,
                  const TcpSegment& syn, uint32_t initial_sequence);
  // `listener` is closing: a connection it spawned and has not handed over yet is reset.
  void leave_listener(TcpSocket* listener);
  // The application let go of it while it was still open (or listening): close it as close() would.
  void close_if_abandoned();
  // A listener's count of connections still in their handshake and of those waiting to be accepted (its accept lock: taken by its connections
  // with their own locks held).
  uint32_t embryo_count() const;
  uint32_t accepted_waiting() const;

 private:
  enum class Mode : uint8_t { Open, Closing };

  uint32_t advertised_window() const;
  void send_segment(uint8_t flags, uint32_t sequence, ConstByteView data, bool with_mss);
  // Data from the send buffer (`offset` bytes past snd_una_) as one large segment the interface cuts into frames of `mss` bytes.
  bool send_burst(uint8_t flags, uint32_t sequence, uint32_t offset, uint32_t length, uint16_t mss);
  void send_ack() { send_segment(tcp_flags::kAck, snd_nxt_, ConstByteView(), false); }
  void output();
  void handle_segment(const TcpSegment& segment);
  void run_timers(uint64_t now);
  // A connection its listener spawned finished its handshake in the segment just handled: hand it over (`self` if the caller has it).
  void hand_over_if_established(const SharedPointer<TcpSocket>* self);
  // Deferred work (pending_): flags it, then runs it here if lock_ can be had without waiting (else the holder does, see release()).
  void request(uint32_t work);
  bool keep_in_backlog(const TcpSegment& segment);
  void run_deferred();
  // Lets go of lock_ (one Owner's hold); the outermost hold first runs the deferred work, and takes lock_ back for work flagged meanwhile.
  void release();
  // The buffers' side for the protocol (each under its leaf lock).
  uint32_t send_size() const {
    kernel::ScopedMutex guard(send_lock_);
    return send_buffer_.size();
  }
  void send_peek(uint32_t offset, ByteView into) const {
    kernel::ScopedMutex guard(send_lock_);
    send_buffer_.peek(offset, into);
  }
  uint32_t receive_room() const {
    kernel::ScopedMutex guard(receive_lock_);
    return receive_buffer_.free_space();
  }
  void process_ack(const TcpSegment& segment);
  void accept_data(const TcpSegment& segment);
  void enter(TcpState state);
  void rollback_for_retransmission();
  void arm_timer();
  void sample_rtt(uint32_t acknowledged_up_to);
  void accept_out_of_order(uint32_t sequence, ConstByteView data);
  void take_reassembled();
  void note_sack(const TcpSegment& segment);
  bool adds_sack(const TcpSegment& segment) const;
  uint32_t pipe() const;
  bool sack_recovery() const { return in_recovery_ && sack_ok_; }
  bool retransmit_holes(uint32_t mss);
  uint32_t pacing_allowance(uint64_t now);
  uint16_t send_mss() const { return peer_mss_ < our_mss() ? peer_mss_ : our_mss(); }
  uint32_t initial_window() const;
  void on_congestion(bool timeout);
  void grow_window(uint32_t acked);
  void fail();
  void prepare_to_connect(uint16_t local_port, uint32_t initial_sequence);
  void set_remote(const IpAddress& address, uint16_t port);
  // The listener's side of its connections (under accept_lock_).
  void add_embryo();
  void child_finished_handshake();
  void child_established(const SharedPointer<TcpSocket>& child);
  void drop_pending_connections();

  IIpLayer& ip_;
  TcpManager& manager_;
  allocators::IAllocator& allocator_;
  ByteView burst_;  // where a large segment is built (allocated when the first one is sent)
  TcpState state_ = TcpState::Closed;   // (written by the owner, read by anyone: atomically - see state())
  uint16_t local_port_ = 0;            // (likewise, with remote_port_ and failed_)
  IpFamilies listen_families_ = IpFamilies::Both;
  IpAddress remote_;   // (see remote_lock_)
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
  ByteRing send_buffer_;      // bytes from snd_una_ on: unacknowledged first, then unsent (send_lock_)
  bool close_requested_ = false;   // (send_lock_: no byte is queued after the close)
  bool fin_sent_ = false;
  bool fin_acked_ = false;
  bool fin_ever_sent_ = false;   // (fin_sent_ is cleared by go-back-N; this is not: the FIN's sequence number is snd_max_ - 1)
  uint16_t peer_mss_ = 536;
  // Receive side.
  uint32_t irs_ = 0, rcv_nxt_ = 0;
  ByteRing receive_buffer_;   // (receive_lock_, with fin_received_)
  bool fin_received_ = false;
  uint32_t last_advertised_window_ = 0;   // (read by receive() without lock_: atomically)
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
  // SACK (RFC 2018). What this side holds beyond rcv_nxt_ (its bytes in ooo_storage_, indexed by sequence modulo kBufferSize: the window
  // is at most kBufferSize, so no two held bytes share a place), and which range the latest segment went to (reported first).
  bool sack_ok_ = false;
  SequenceRanges held_;
  uint32_t latest_held_ = 0;
  ByteView ooo_storage_;
  // What the peer said it holds beyond snd_una_ (the scoreboard), and where hole retransmission has got to in recovery.
  SequenceRanges sacked_;
  uint32_t retransmit_next_ = 0, retransmitted_ = 0;
  // ECN (RFC 3168).
  bool ecn_ok_ = false;
  bool ece_pending_ = false;      // a CE mark arrived: ACKs say ECE until the peer sends CWR
  bool cwr_pending_ = false;      // the window was cut for an ECE: the next new data says CWR
  uint32_t ecn_recover_ = 0;      // no second cut for an ECE until snd_una_ passes this
  bool ecn_cut_ = false;          // (whether ecn_recover_ is set yet)
  // Pacing: bytes that may go now (refilled at the pacing rate, at most a burst's worth), and when it was last refilled.
  uint64_t pacing_credit_ = 0, paced_at_ms_ = 0;
  bool paced_out_ = false;

  bool failed_ = false;
  bool established_event_ = false;
  // Listening sockets.
  TcpSocket* listener_ = nullptr;  // for a connection spawned by a listener, until accepted (it outlives this pointer: see leave_listener)
  mutable kernel::Mutex accept_lock_;   // guards the two below (a leaf: nothing else is taken while it is held)
  List<SharedPointer<TcpSocket>> accept_queue_;
  uint32_t embryos_ = 0;           // children still in SynReceived

  // Guards everything in this socket but what the leaf locks below and accept_lock_ guard (see TcpManager for the order it is taken in).
  mutable kernel::RecursiveMutex lock_;
  // Leaf locks (nothing is taken while one is held), each guarding one buffer for the application and the protocol to share.
  mutable kernel::Mutex send_lock_, receive_lock_;
  mutable kernel::Mutex remote_lock_;   // remote_, for readers that do not hold lock_ (it is written with both held)

  // Deferred work for lock_'s holder: bits of pending_ (set by anyone, atomically; taken by the holder).
  static constexpr uint32_t kWorkBacklog = 1, kWorkOutput = 2, kWorkWindow = 4, kWorkTimers = 8;
  uint32_t pending_ = 0;
  // The backlog: segments copied while the connection was busy. Two halves: arrivals fill one (under backlog_lock_, a leaf) while the holder
  // works through the other, which nobody else touches.
  struct BacklogEntry {
    TcpSegment segment;   // (its data view is empty: the bytes are at `offset` in the half's data)
    uint64_t offset = 0;
    uint32_t length = 0;
  };
  struct Backlog {
    explicit Backlog(allocators::IAllocator& allocator) : entries(allocator), data(allocator) {}
    List<BacklogEntry> entries;
    ByteBuffer data;
  };
  mutable kernel::Mutex backlog_lock_;
  Backlog backlog_[2];
  uint32_t filling_ = 0;   // the half arrivals go to (backlog_lock_)
  uint64_t backlogged_ = 0, backlog_drops_ = 0;

  // Which of the manager's tables this socket is in, under what key (the manager's, under its table lock).
  friend class TcpManager;
  enum class Table : uint8_t { None, Listeners, Connections };
  Table table_ = Table::None;
  SocketKey table_key_;
};

}  // namespace network
