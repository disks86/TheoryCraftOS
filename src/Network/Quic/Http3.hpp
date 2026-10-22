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

// One HTTP/3 request and its answer (RFC 9114) over a QUIC connection of its own (see QuicConnection.hpp), with QPACK (see Qpack.hpp) for the headers. The
// connection carries a single request: the exchange sets up the control and QPACK streams, sends the request on a bidirectional stream, reads the answer,
// and closes the connection with H3_NO_ERROR when it is over. Polled like the connection under it.
//
//   Http3Exchange h3(stack, allocator, quic_config);
//   h3::HeaderList request(allocator);   // :method :scheme :authority :path, then the other fields
//   h3.start(address, 443, "example.com", request, body);
//   while (h3.phase() == Http3Phase::Connecting || h3.phase() == Http3Phase::Requesting) { stack.poll(); h3.poll(); take h3.pending_body(); }
//
// Server push is never enabled (no MAX_PUSH_ID is sent). The dynamic table of QPACK is not used (this side announces capacity 0). A GOAWAY that does not
// cover the request, a refused stream and the like end as Http3Error::Refused: nothing was processed, so the request may be repeated elsewhere.
#pragma once
#include "Network/Quic/Qpack.hpp"
#include "Network/Quic/QuicConnection.hpp"

namespace network {

enum class Http3Phase : uint8_t { Idle, Connecting, Requesting, Done, Failed };

enum class Http3Error : uint8_t {
  None,
  Connection,   // the QUIC connection could not be made, or ended (connection().error())
  Protocol,     // the server broke a rule of HTTP/3 or QPACK (h3_code())
  Reset,        // the server reset the request stream (h3_code())
  Refused,      // the server did not take the request (GOAWAY, H3_REQUEST_REJECTED): it can be tried again
  TooLarge,     // the headers are over the limit
  Internal,     // out of memory
  Cancelled,
};
const char* http3_error_name(Http3Error error);

class Http3Exchange {
 public:
  static constexpr uint64_t kMaxHeaderBytes = 256 * 1024;   // a field section (decoded size, as RFC 9114 4.2.2 counts it)
  static constexpr uint64_t kBodyBuffer = 256 * 1024;       // how much answer is held before the connection is left to wait

  Http3Exchange(INetworkStack& stack, allocators::IAllocator& allocator, const QuicConfig& config);
  Http3Exchange(const Http3Exchange&) = delete;
  Http3Exchange& operator=(const Http3Exchange&) = delete;

  // Starts connecting to the server; the request (and `body`, which must stay valid until the end) is sent as soon as the connection is up. False if the
  // socket could not be opened or memory ran out.
  bool start(Ipv4Address address, uint16_t port, StringView server_name, const h3::HeaderList& request, ConstByteView body);
  void poll();
  // Gives up: the connection is closed.
  void cancel();

  Http3Phase phase() const { return phase_; }
  Http3Error error() const { return error_; }
  uint64_t h3_code() const { return h3_code_; }
  QuicConnection& connection() { return connection_; }
  const QuicConnection& connection() const { return connection_; }

  // The answer's head has arrived (the final one; 1xx are skipped).
  bool head_ready() const { return head_done_; }
  uint32_t status() const { return status_; }
  const h3::HeaderList& response() const { return response_; }
  // The body as far as it has arrived, and taking it.
  ConstByteView pending_body() const { return body_.view().subview(body_pos_); }
  void consume_body(uint64_t bytes);
  uint64_t body_received() const { return body_total_; }

 private:
  struct Varint {
    uint8_t got = 0, length = 0;
    uint64_t value = 0;
  };
  struct PeerStream {
    uint64_t id = 0;
    int64_t type = -1;      // -1: the type byte has not arrived
    Varint partial;
  };
  enum class Frame : uint8_t { Type, Length, Payload };

  bool read_varint(uint64_t stream, Varint* state, uint64_t* out);
  void set_up();
  bool flush_request();
  void service_peer_streams();
  void read_control();
  void read_response();
  void end_frame();
  bool on_section();
  void h3_fail(uint64_t code, Http3Error error);
  void fail(Http3Error error);
  void complete();

  QuicConfig config_;
  QuicConnection connection_;
  allocators::IAllocator& allocator_;
  Http3Phase phase_ = Http3Phase::Idle;
  Http3Error error_ = Http3Error::None;
  uint64_t h3_code_ = 0;
  bool set_up_ = false, closed_ = false;
  // the request
  ByteBuffer request_head_;       // the HEADERS frame and the header of the DATA frame
  ConstByteView request_body_;
  uint64_t request_sent_ = 0, request_total_ = 0, request_stream_ = 0;
  bool request_done_ = false;
  // the peer's unidirectional streams
  static constexpr uint32_t kPeerStreams = 8;
  PeerStream peers_[kPeerStreams];
  uint32_t peer_count_ = 0;
  ByteBuffer control_;
  uint64_t control_pos_ = 0;
  bool control_seen_ = false, settings_seen_ = false, goaway_ = false;
  uint64_t goaway_id_ = 0;
  // the answer
  Frame frame_ = Frame::Type;
  Varint vtype_, vlength_;
  uint64_t frame_type_ = 0, frame_left_ = 0;
  ByteBuffer section_;
  h3::HeaderList response_, scratch_;
  bool head_done_ = false, saw_data_ = false;
  uint32_t status_ = 0;
  ByteBuffer body_;
  uint64_t body_pos_ = 0, body_total_ = 0;
};

}  // namespace network
