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

// SCTP (RFC 4960) as WebRTC data channels use it (RFC 8831 / 8261): one association over DTLS, user messages on numbered streams with a payload protocol identifier,
// ordered or not, reliable or partially reliable (FORWARD-TSN, RFC 3758), stream resets (RFC 6525). Packets in and out; the owner moves them through DTLS and calls
// tick() for the timers. No multi-homing, no IP addresses (the "ports" are only labels, as RFC 8261 has it).
#pragma once
#include "Crypto/Random.hpp"
#include "ObjectContainer/List.hpp"
#include "Rtc/RtcBytes.hpp"
#include "Utility/UniquePointer.hpp"

namespace rtc {

uint32_t crc32c(ConstByteView data);

struct SctpMessage {
  explicit SctpMessage(allocators::IAllocator& a) : data(a) {}
  uint16_t stream = 0;
  uint32_t ppid = 0;
  ByteBuffer data;
};

struct SctpOptions {
  bool ordered = true;
  int32_t max_retransmits = -1;   // -1: reliable
  int32_t max_lifetime_ms = -1;
};

class SctpAssociation {
 public:
  enum class State : uint8_t { Closed, CookieWait, CookieEchoed, Established, ShutdownPending, ShutdownSent, ShutdownReceived, ShutdownAckSent };
  static constexpr uint32_t kMaxPacket = 1100, kMaxMessage = 256 * 1024, kMaxStreams = 1024;

  SctpAssociation(allocators::IAllocator& allocator, crypto::IRandom& random);

  void set_ports(uint16_t local, uint16_t remote) { local_port_ = local; remote_port_ = remote; }
  void connect(uint64_t now_ms);                                    // sends INIT
  void receive(ConstByteView packet, uint64_t now_ms);
  bool next_packet(ByteBuffer* out);
  void tick(uint64_t now_ms);
  State state() const { return state_; }
  bool established() const { return state_ == State::Established; }

  // Queues a message. False if the association is not up, the message is too large, or too much is queued already.
  bool send(uint16_t stream, uint32_t ppid, ConstByteView data, const SctpOptions& options);
  bool receive_message(UniquePointer<SctpMessage>* out);
  // Bytes of the stream not yet acknowledged (queued or in flight).
  uint64_t buffered(uint16_t stream) const;
  // Asks the peer to reset the stream (after what is queued on it has been delivered). take_reset() tells of resets the peer asked for.
  bool reset_stream(uint16_t stream);
  bool take_reset(uint16_t* stream);
  void shutdown();
  void abort();

  uint32_t retransmissions() const { return retransmissions_; }

 private:
  struct Out {
    explicit Out(allocators::IAllocator& a) : data(a) {}
    uint32_t tsn = 0, ppid = 0, message = 0;
    uint16_t stream = 0, ssn = 0;
    uint8_t flags = 0;                 // B 2, E 1, U 4
    int32_t max_rtx = -1;
    uint64_t expires_ms = 0;           // 0: never
    uint64_t sent_ms = 0;
    uint32_t transmissions = 0, misses = 0;
    bool sent = false, acked = false, abandoned = false, retransmit = false;
    ByteBuffer data;
  };
  struct In {
    explicit In(allocators::IAllocator& a) : data(a) {}
    uint32_t tsn = 0, ppid = 0;
    uint16_t stream = 0, ssn = 0;
    uint8_t flags = 0;
    ByteBuffer data;
  };
  struct StreamState {
    uint16_t id = 0;
    uint16_t next_out_ssn = 0, next_in_ssn = 0;
  };

  void queue_control(uint8_t chunk_type, uint8_t flags, ConstByteView value, uint32_t tag);
  void queue_packet(uint32_t tag, const ByteBuffer& chunks);
  void send_init();
  void send_cookie_echo();
  void handle_chunk(uint8_t type, uint8_t flags, ConstByteView value, uint32_t tag, bool* got_data);
  void handle_init(ConstByteView value);
  void handle_init_ack(ConstByteView value);
  void handle_cookie_echo(ConstByteView value);
  void handle_data(uint8_t flags, ConstByteView value);
  void handle_sack(ConstByteView value);
  void handle_forward_tsn(ConstByteView value);
  void handle_reconfig(ConstByteView value);
  void send_sack();
  void deliver();
  void abandon(const Out& chunk);
  void process_acks(uint32_t cum, uint32_t rwnd, ConstByteView gaps, uint32_t gap_count);
  StreamState& stream_state(uint16_t id);
  uint32_t flight_size() const;
  bool build_data_packet(ByteBuffer* out);
  uint32_t random32();
  void establish(uint32_t peer_tag, uint32_t peer_tsn, uint32_t peer_rwnd);
  void enter_closed();

  allocators::IAllocator& allocator_;
  crypto::IRandom& random_;
  State state_ = State::Closed;
  uint16_t local_port_ = 5000, remote_port_ = 5000;
  uint32_t my_tag_ = 0, peer_tag_ = 0;
  uint64_t now_ms_ = 0;
  // handshake
  uint32_t init_tsn_ = 0;
  ByteBuffer cookie_, init_value_;
  uint8_t secret_[20] = {};
  uint64_t t1_at_ = 0;
  uint32_t t1_attempts_ = 0;
  // sending
  uint32_t next_tsn_ = 0, cum_acked_ = 0, peer_rwnd_ = 0, cwnd_ = 0, ssthresh_ = 0, partial_acked_ = 0, next_message_ = 1;
  uint64_t rto_ms_ = 1000, srtt_ms_ = 0, rttvar_ms_ = 0, t3_at_ = 0;
  uint32_t t3_failures_ = 0, retransmissions_ = 0, queued_bytes_ = 0;
  bool have_rtt_ = false;
  List<UniquePointer<Out>> out_;
  List<StreamState> streams_;
  uint32_t forward_tsn_sent_ = 0;
  bool forward_pending_ = false;
  // receiving
  uint32_t cum_tsn_ = 0, buffered_in_ = 0;
  bool dup_pending_ = false, sack_pending_ = false;
  List<uint32_t> gaps_, dups_;     // TSNs above cum_tsn_ received (sorted); duplicates seen since the last SACK
  List<UniquePointer<In>> in_;
  List<UniquePointer<SctpMessage>> inbox_;
  // control
  List<UniquePointer<ByteBuffer>> control_;
  uint32_t reset_seq_ = 0, peer_reset_seq_ = 0, pending_reset_seq_ = 0;
  List<uint16_t> reset_wanted_, reset_inflight_, resets_taken_;
  uint64_t reset_sent_ms_ = 0;
  bool shutdown_wanted_ = false;
  uint64_t shutdown_sent_ms_ = 0;
};

}  // namespace rtc
