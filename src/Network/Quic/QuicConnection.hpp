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

// A QUIC client connection (RFC 9000, 9001, 9002) over a UDP socket of the network stack: the handshake (TLS 1.3 carried in CRYPTO frames, see
// Network/Tls/Tls13Client.hpp), packet protection, acknowledgements, loss recovery with NewReno congestion control, flow control, and streams. It is polled like
// the sockets underneath: call poll() while the stack is polled; nothing happens in between.
//
//   QuicConnection quic(stack, allocator, config);
//   quic.connect(address, 443, "example.com");
//   while (!quic.established() && !quic.failed()) { stack.poll(); quic.poll(); }
//   uint64_t id;  quic.open_stream(true, &id);  quic.write_stream(id, request, true);
//   ... stack.poll(); quic.poll(); quic.read_stream(id, buffer) ...
//
// Supported: QUIC version 1 as a client; Initial, Handshake and 1-RTT packets (no 0-RTT); Retry; ACKs with ranges; PTO and the time and packet thresholds of
// RFC 9002; connection and stream flow control; peer-initiated key updates; the peer's connection IDs; stateless resets; closing. Not supported: version
// negotiation (another version is a failure), migration and path validation beyond answering PATH_CHALLENGE, ECN, DATAGRAM frames, 0-RTT, pacing.
#pragma once
#include "Network/INetworkStack.hpp"
#include "Network/Quic/QuicCommon.hpp"
#include "Network/Quic/QuicCrypto.hpp"
#include "Network/Quic/QuicStreams.hpp"
#include "Network/Tls/Tls13Client.hpp"

namespace network {

enum class QuicState : uint8_t { Idle, Connecting, Established, Closed, Failed };

enum class QuicError : uint8_t {
  None,
  Connect,        // the UDP socket could not be opened
  Timeout,        // the handshake did not finish in time, or the connection went idle
  Version,        // the server does not speak QUIC version 1
  Tls,            // the TLS handshake failed (tls_error(), certificate_status(), tls_alert())
  Protocol,       // the server broke a rule of the protocol (transport_error() says which)
  PeerClosed,     // the server closed the connection (peer_close_code(), peer_close_reason())
  StatelessReset, // the server forgot the connection
  Internal,       // out of memory, or no random numbers
  Closed,         // closed by this side with close()
};
const char* quic_error_name(QuicError error);

struct QuicConfig {
  TlsConfig tls;                        // trust store, random source, clock, certificate rules; `alpn` names the application protocol(s)
  uint32_t idle_timeout_ms = 30000;
  uint32_t handshake_timeout_ms = 10000;
  uint64_t connection_window = 8u << 20;       // how much the peer may send on the connection before we raise the limit
  uint64_t stream_window = 4u << 20;           // ... on one stream
  uint32_t max_incoming_streams = 64;          // unidirectional streams the peer may open (it needs 3 for HTTP/3)
  uint32_t max_datagram = 1350;                // the largest UDP payload sent
};

class QuicConnection {
 public:
  static constexpr uint64_t kNoStream = ~uint64_t{0};

  QuicConnection(INetworkStack& stack, allocators::IAllocator& allocator, const QuicConfig& config);
  ~QuicConnection();
  QuicConnection(const QuicConnection&) = delete;
  QuicConnection& operator=(const QuicConnection&) = delete;

  // Opens the UDP socket and sends the first packets. `server_name` is the TLS server name (SNI, certificate check). False if that could not be done.
  bool connect(Ipv4Address address, uint16_t port, StringView server_name);
  // Receives datagrams, runs the timers, sends what is due.
  void poll();

  QuicState state() const { return state_; }
  bool established() const { return state_ == QuicState::Established; }     // streams can be used
  bool failed() const { return state_ == QuicState::Failed; }
  bool closed() const { return state_ == QuicState::Closed || state_ == QuicState::Failed; }
  QuicError error() const { return error_; }
  uint64_t transport_error() const { return transport_error_; }             // the QUIC error code behind QuicError::Protocol (RFC 9000 20.1)
  uint64_t peer_close_code() const { return peer_close_code_; }
  StringView peer_close_reason() const { return peer_close_reason_.view(); }
  bool peer_close_was_application() const { return peer_close_application_; }
  TlsError tls_error() const { return tls_ ? tls_->error() : TlsError::None; }
  crypto::CertStatus certificate_status() const { return tls_ ? tls_->certificate_status() : crypto::CertStatus::Ok; }
  uint8_t tls_alert() const { return tls_ ? tls_->alert() : 0; }
  const char* cipher_suite_name() const { return tls_ && tls_->suite() ? tls_->suite()->name : ""; }
  StringView alpn() const { return tls_ ? tls_->alpn_selected() : StringView(); }
  bool tls_resumed() const { return tls_ && tls_->resumed(); }

  // --- Streams (ids as in RFC 9000 2.1: client-initiated bidirectional 0, 4, 8 ..., unidirectional 2, 6 ...; the server's 1, 5 ... and 3, 7 ...)
  // Opens a stream of ours. False if the peer's limit on streams is reached (try again later) or the connection is not established.
  bool open_stream(bool bidirectional, uint64_t* id);
  // Queues bytes on a stream (`fin`: and ends it). Returns how many were taken: less than all when the stream's send buffer is full - write the rest later.
  uint64_t write_stream(uint64_t id, ConstByteView data, bool fin);
  // Bytes waiting to be read on a stream, reading them, and whether the stream has ended (everything read).
  uint64_t readable(uint64_t id) const;
  uint64_t read_stream(uint64_t id, ByteView out);
  bool stream_finished(uint64_t id) const;
  // The peer reset the stream (RESET_STREAM): its error code.
  bool stream_reset_by_peer(uint64_t id, uint64_t* code) const;
  // The peer asked us to stop sending on the stream (STOP_SENDING).
  bool stream_stop_requested(uint64_t id, uint64_t* code) const;
  // A stream the peer opened that has not been taken yet (in the order of ids); false if none.
  bool accept_stream(uint64_t* id);
  // Gives up sending on a stream (RESET_STREAM), and asks the peer to stop sending on it (STOP_SENDING).
  void reset_stream(uint64_t id, uint64_t error_code);
  void stop_sending(uint64_t id, uint64_t error_code);
  // How many bytes more can be queued on a stream right now.
  uint64_t writable(uint64_t id) const;

  // Closes the connection with an application error code (a CONNECTION_CLOSE frame goes out) .
  void close(uint64_t error_code = 0, StringView reason = StringView());

  // --- Numbers for tests and diagnostics
  struct Stats {
    uint64_t packets_sent = 0, packets_received = 0, packets_lost = 0, bytes_sent = 0, bytes_received = 0, retransmissions = 0, ptos = 0, datagrams_dropped = 0;
    uint32_t smoothed_rtt_ms = 0, congestion_window = 0, bytes_in_flight = 0;
    uint32_t key_updates = 0;
    bool retry_seen = false;
  };
  Stats stats() const;

 private:
  static constexpr uint32_t kSpaceCount = 3;
  static constexpr uint32_t kMaxFramesPerPacket = 16;
  static constexpr uint32_t kPacketNumberLength = 4;

  struct FrameRef {
    enum Kind : uint8_t { Crypto, Stream, MaxData, MaxStreamData, MaxStreams, RetireConnectionId, ResetStream, StopSending, PathResponse, HandshakeDone, NewConnectionId };
    Kind kind = Crypto;
    bool fin = false, flag = false;
    uint64_t id = 0, offset = 0, length = 0, code = 0;
  };
  struct SentPacket {
    uint64_t number = 0, time_ms = 0;
    uint32_t size = 0, frame_count = 0;
    bool ack_eliciting = false, in_flight = false;
    FrameRef frames[kMaxFramesPerPacket];
  };
  struct Space {
    explicit Space(allocators::IAllocator& a) : received(a), sent(a), crypto_send(a), crypto_recv(a) {}
    quic::PacketKeys send_keys, receive_keys;
    bool keys_ready = false, discarded = false;
    uint64_t next_number = 0;
    int64_t largest_acked = -1, largest_received = -1;
    uint64_t largest_received_ms = 0;
    quic::RangeSet received;
    bool ack_pending = false;
    uint32_t unacked_ack_eliciting = 0;
    uint64_t ack_deadline_ms = 0;
    List<SentPacket> sent;
    uint64_t loss_time_ms = 0, last_ack_eliciting_ms = 0;
    quic::SendBuffer crypto_send;
    quic::RecvBuffer crypto_recv;
    uint32_t probes = 0;   // ack-eliciting packets that must be sent now whatever the congestion window says
    // The CRYPTO ranges of the last probe packet: the next probe repeats them, so that each probe packet of the handshake stands on its own (a server may refuse
    // an Initial packet without CRYPTO data as the first it gets, which it is if the earlier probe was lost or overtaken).
    FrameRef last_probe_crypto[2];
    uint32_t last_probe_crypto_count = 0;
  };
  struct Stream {
    Stream(allocators::IAllocator& a, uint64_t stream_id) : id(stream_id), send(a), recv(a) {}
    uint64_t id;
    quic::SendBuffer send;
    quic::RecvBuffer recv;
    uint64_t send_limit = 0;          // the peer's limit on the offsets we may send (MAX_STREAM_DATA)
    uint64_t recv_limit = 0;          // the limit we told the peer
    bool max_stream_data_pending = false;
    bool reset_received = false, stop_requested = false, accepted = false;
    uint64_t reset_code = 0, stop_code = 0, reset_final_size = 0;
    bool reset_to_send = false, reset_sent = false, stop_to_send = false, stop_sent = false;
    uint64_t reset_to_send_code = 0, stop_to_send_code = 0;
    bool counted_for_credit = false;
  };
  struct ConnectionIdEntry {
    uint64_t sequence = 0;
    quic::ConnectionId id;
    uint8_t reset_token[16] = {};
  };

  // setup and the receive side (QuicConnection.cpp)
  void fail(QuicError error, uint64_t transport_error = 0);
  void drain_socket();
  void on_datagram(ByteView datagram);
  bool on_packet(ByteView packet, uint32_t* consumed);
  bool on_long_packet(ByteView packet, uint32_t* packet_length);
  bool on_short_packet(ByteView packet);
  void on_retry(ConstByteView packet);
  bool process_payload(uint32_t space, ConstByteView payload, bool* ack_eliciting);
  static bool equal_constant_time_bytes(ConstByteView a, ConstByteView b) { return a.size() == b.size() && crypto::equal_constant_time(a.data(), b.data(), static_cast<uint32_t>(a.size())); }
  bool on_ack(uint32_t space, ByteReader& r, bool with_ecn);
  bool on_crypto(uint32_t space, ByteReader& r);
  bool on_stream(uint8_t type, ByteReader& r);
  bool on_new_connection_id(ByteReader& r);
  void on_stream_credit(Stream& s);
  void advance_tls();
  bool apply_peer_parameters();
  void install_keys(TlsLevel level);
  void discard_space(uint32_t space);
  Stream* find_stream(uint64_t id) const;
  Stream* get_stream(uint64_t id, bool create);
  bool stream_is_ours(uint64_t id) const { return (id & 1) == 0; }

  // loss recovery (QuicRecovery.cpp)
  void on_packet_sent(uint32_t space, SentPacket& packet);
  void process_acked(uint32_t space, SentPacket& packet, bool* congestion_credit);
  void on_packet_lost(uint32_t space, SentPacket& packet, bool congestion_event);
  void detect_losses(uint32_t space);
  void update_rtt(uint64_t latest_ms, uint64_t ack_delay_ms, bool application);
  void set_loss_timer();
  void on_loss_timer();
  void requeue_frames(uint32_t space, const SentPacket& packet);
  uint64_t pto_interval_ms(uint32_t space) const;

  // the send side (QuicSend.cpp)
  void send_pending();
  struct PacketOut {
    uint32_t space = 0, start = 0, pn_offset = 0, header_length = 0, length_field_at = 0, end = 0;   // [start, end) holds the packet without its tag
    uint64_t number = 0;
    SentPacket record;
  };
  bool build_datagram(uint8_t* buffer, uint32_t capacity, uint32_t* length);
  bool build_packet(uint32_t space, uint8_t* buffer, uint32_t capacity, PacketOut* out);
  bool add_ack_frame(uint32_t space, ByteWriter& w, uint32_t limit);
  bool want_to_send(uint32_t space) const;
  bool send_datagram(ConstByteView datagram);
  void queue_connection_close();

  INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  QuicConfig config_;
  QuicState state_ = QuicState::Idle;
  QuicError error_ = QuicError::None;
  uint64_t transport_error_ = 0, peer_close_code_ = 0;
  bool peer_close_application_ = false;
  String peer_close_reason_;
  SharedPointer<IUdpSocket> socket_;
  Ipv4Address peer_address_;
  uint16_t peer_port_ = 0;
  UniquePointer<Tls13Client> tls_;
  Space spaces_[kSpaceCount];
  quic::ConnectionId original_dcid_, dcid_, scid_;
  bool dcid_known_ = false, retry_seen_ = false, handshake_confirmed_ = false, handshake_done_received_ = false, transport_parameters_applied_ = false;
  bool initial_keys_dropped_ = false;
  ByteBuffer retry_token_;
  quic::ConnectionId retry_source_cid_;
  uint64_t started_ms_ = 0, last_activity_ms_ = 0;
  uint32_t idle_timeout_ms_ = 30000;
  // keys of the 1-RTT space for key updates
  quic::PacketKeys next_receive_keys_, old_receive_keys_;
  bool key_phase_ = false, have_next_keys_ = false, have_old_keys_ = false;
  uint64_t old_keys_until_ = 0;
  uint32_t next_stream_round_ = 0;
  // peer's transport parameters
  uint64_t peer_max_data_ = 0, peer_initial_bidi_local_ = 0, peer_initial_bidi_remote_ = 0, peer_initial_uni_ = 0;
  uint64_t peer_max_streams_bidi_ = 0, peer_max_streams_uni_ = 0;
  uint32_t peer_max_udp_payload_ = 1200, peer_ack_delay_exponent_ = 3, peer_max_ack_delay_ms_ = 25, peer_idle_timeout_ms_ = 0, peer_active_cid_limit_ = 2;
  uint8_t peer_reset_token_[16] = {};
  bool have_peer_reset_token_ = false;
  // connection ids offered by the peer
  List<ConnectionIdEntry> peer_cids_;
  List<uint64_t> retire_queue_;
  // connection flow control
  uint64_t data_sent_ = 0;                 // new stream bytes sent (offsets counted once)
  uint64_t recv_max_data_ = 0, data_received_ = 0, data_consumed_ = 0;
  bool max_data_pending_ = false;
  uint64_t streams_opened_bidi_ = 0, streams_opened_uni_ = 0;
  uint64_t local_max_streams_uni_ = 0, peer_streams_uni_opened_ = 0;
  bool max_streams_pending_ = false;
  List<UniquePointer<Stream>> streams_;
  // loss recovery and congestion control
  uint64_t smoothed_rtt_ms_ = 333, rtt_var_ms_ = 166, min_rtt_ms_ = 0, latest_rtt_ms_ = 0;
  bool have_rtt_ = false;
  uint32_t pto_count_ = 0;
  uint64_t loss_timer_ms_ = 0;
  uint32_t bytes_in_flight_ = 0;
  uint64_t congestion_window_ = 0, ssthresh_ = ~uint64_t{0}, recovery_start_ms_ = 0;
  // sending
  ByteBuffer stash_;                        // a datagram the socket would not take, to send again
  ByteBuffer path_response_;                // 8-byte payloads of PATH_CHALLENGEs to answer
  bool close_pending_ = false, close_sent_ = false, handshake_done_ack_ = false;
  uint64_t close_code_ = 0;
  String close_reason_;
  bool close_application_ = true;
  Stats stats_;
};

}  // namespace network
