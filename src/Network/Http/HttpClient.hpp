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

// An HTTP/1.1 client for http:// and https:// URLs, on top of the network stack's TCP sockets and TlsSocket. One request at a time; polled like
// the sockets underneath (call poll() while the stack is polled, or run() to wait for the answer):
//
//   HttpClient http(stack, allocator);
//   http.set_tls(&tls_config);                       // trust store, random source and clock; needed for https:// only
//   http.start(HttpMethod::Get, "https://example.com/index.html");
//   http.run(20000);                                 // or: while (http.busy()) { stack.poll(); http.poll(); }
//   if (http.state() == HttpState::Done) use(http.status(), http.header("content-type"), http.body());
//
//   - The host name is looked up through the stack's DNS resolver (a dotted-quad address is used as it is), the connection is opened, wrapped
//     in TLS for https (the certificate must be valid for the host name and chain to the trust store), and the request goes out with
//     "Connection: close": every request has its own connection.
//   - Answers with Content-Length, "Transfer-Encoding: chunked" (extensions and trailers are skipped) and ones that simply end with the
//     connection are all understood. 1xx interim answers are skipped. HEAD answers, 204 and 304 have no body.
//   - Redirects (301, 302, 303, 307, 308) are followed, up to set_max_redirects() (default 5): a 303, and a 301 or 302 to a POST, becomes
//     a GET; credentials in headers you added (Authorization, Cookie) are not sent to another origin; a redirect from https to http is refused.
//   - The body is collected in memory (at most set_max_body(), default 16 MiB), after "gzip" and "deflate" encodings are undone; or, with
//     set_body_sink(), handed to a function piece by piece as it arrives and not kept (then it is neither limited nor decoded).
//   - start_websocket() opens a WebSocket (RFC 6455) over ws:// or wss:// (or http:// and https://): the client sends the upgrade request and checks the
//     server's answer; the state is then Upgraded, and ws_send_text() / ws_send_binary() / ws_ping() / ws_close() and ws_receive() (see WebSocket.hpp for
//     the framing) carry the messages. Pings are answered, a Close is answered, and a message that breaks the rules ends the connection with the right code.
//       http.start_websocket("wss://echo.example.com/socket") && http.run(10000);
//       http.ws_send_text("hello");  ... http.poll(); WebSocketMessage m(allocator); if (http.ws_receive(&m)) use(m);
//   - HTTP/3 (RFC 9114 over QUIC, see Http3.hpp) is used for https:// requests when set_http3() allows it: with Http3Mode::Auto when the server has said
//     so in an Alt-Svc header the client has seen (the AltSvcCache given to set_alt_svc_cache() remembers that between requests), with Http3Mode::Prefer
//     for every https request. A QUIC attempt that fails before the server has taken the request (no UDP route, no answer, a refusal) is dropped and the
//     request goes on over TCP, and the server is not tried over QUIC again for a while. used_http3() says which was used.
// Not supported: keeping connections alive, proxies, IPv6, HTTP/2, cookies, authentication, compressed request bodies, WebSockets over HTTP/3.
#pragma once
#include "Network/Http/AltSvc.hpp"
#include "Network/Http/HttpUrl.hpp"
#include "Network/Http/WebSocket.hpp"
#include "Network/INetworkStack.hpp"
#include "Network/Quic/Http3.hpp"
#include "Network/Tls/TlsSession.hpp"
#include "Network/Tls/TlsSocket.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

enum class HttpMethod : uint8_t { Get, Head, Post, Put, Delete, Patch };
const char* http_method_name(HttpMethod method);

// Upgraded: a WebSocket is open (or closing); it is not "busy", the program drives it with the ws_ functions and poll().
enum class HttpState : uint8_t { Idle, Resolving, Connecting, Sending, ReceivingHead, ReceivingBody, Done, Failed, Upgraded };

// When https requests go over HTTP/3 (QUIC): never, when an Alt-Svc header has announced it (needs set_alt_svc_cache()), or always (falling back to TCP).
enum class Http3Mode : uint8_t { Off, Auto, Prefer };

enum class HttpError : uint8_t {
  None,
  BadUrl,            // the URL, or a Location header, is not a usable http(s) URL
  NoTlsConfiguration,// https:// without set_tls()
  Resolve,           // the host name could not be looked up
  Connect,           // the TCP connection failed or was refused
  Tls,               // the TLS handshake or the connection's encryption failed (tls_error(), certificate_status())
  Timeout,           // no connection in time, or the exchange stalled
  Protocol,          // the answer is not valid HTTP (or a gzip body is damaged)
  TooLarge,          // the body is longer than set_max_body()
  TooManyRedirects,
  InsecureRedirect,  // a redirect from https to http
  Refused,           // the body sink said stop
  Cancelled,
  OutOfMemory,
  WebSocketHandshake, // the server did not accept the WebSocket upgrade, or its answer broke the rules (the status, ws_handshake_status())
};
const char* http_error_name(HttpError error);

// Receives the body in pieces as it arrives; returns false to abandon the download (the request then fails with Refused).
using HttpBodySink = bool (*)(void* context, ConstByteView data);

// One message of a WebSocket.
struct WebSocketMessage {
  explicit WebSocketMessage(allocators::IAllocator& allocator) : data(allocator) {}
  bool text = false;   // text (UTF-8) or binary
  ByteBuffer data;
};

class HttpClient {
 public:
  static constexpr uint32_t kMaxHeadBytes = 64 * 1024;

  HttpClient(INetworkStack& stack, allocators::IAllocator& allocator);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // --- Settings (they apply to the next start())
  // Required for https://. Copied; what it points to (trust store, random source, clock) must outlive the client. The server name and the
  // ALPN protocol are filled in per request.
  void set_tls(const TlsConfig* config) {
    has_tls_ = config != nullptr;
    if (config) tls_ = *config;
  }
  void set_max_redirects(uint32_t count) { max_redirects_ = count; }
  void set_max_body(uint64_t bytes) { max_body_ = bytes; }
  void set_body_sink(HttpBodySink sink, void* context) {
    sink_ = sink;
    sink_context_ = context;
  }
  void set_timeouts(uint32_t connect_ms, uint32_t stall_ms) {
    connect_timeout_ms_ = connect_ms;
    stall_timeout_ms_ = stall_ms;
  }
  // A header sent with every request (replacing the default of the same name: User-Agent, Accept, Accept-Encoding). False for a name or
  // value that is not valid (control characters, a colon in the name), or memory shortage.
  bool add_header(StringView name, StringView value);
  void clear_headers() { extra_headers_.clear(); }
  // Where unpredictable bytes come from, for the keys and masks of WebSockets (the TLS configuration's random source is used when this is not set; with
  // neither, a generator seeded from the clock is - fine against accidents, not against an attacker who can watch the connection).
  void set_random(crypto::IRandom* random) { random_ = random; }
  // Offer permessage-deflate (RFC 7692, without context takeover) to the next WebSocket server; messages are then compressed in both directions if it agrees.
  void set_websocket_compression(bool on) { ws_offer_deflate_ = on; }

  // HTTP/3 policy (default Auto) and the memory of what servers announced; the cache must outlive the client and may be shared by several.
  void set_http3(Http3Mode mode) { http3_mode_ = mode; }
  void set_alt_svc_cache(AltSvcCache* cache) { alt_svc_ = cache; }
  // Where TLS 1.3 session tickets are kept so that a later request to the same server resumes the session (over TCP and QUIC alike; see TlsSession.hpp). The cache
  // must outlive the client. (A cache in the TlsConfig given to set_tls() does the same; this one wins.)
  void set_session_cache(TlsSessionCache* cache) { session_cache_ = cache; }

  // --- A request
  // Starts a request for `url`. `body` (sent with Content-Length; it must stay valid until the request is over) and `content_type` are for
  // POST, PUT and PATCH. False if `url` is not a valid http(s) URL or memory ran out (error() says which). Cancels a request in progress.
  bool start(HttpMethod method, StringView url, ConstByteView body = ConstByteView(), StringView content_type = StringView());
  void poll();
  // Waits (polling the stack) until the request is over or `timeout_ms` has passed (then it is cancelled with Timeout). True if it ended Done.
  bool run(uint32_t timeout_ms);
  void cancel();

  // --- WebSockets
  // Starts a WebSocket to `url` (ws://, wss://, http:// or https://), offering the comma separated `subprotocols` if any. False if the URL is not valid
  // (error() says). The upgrade is over when state() is Upgraded (run() returns true then) or Failed.
  bool start_websocket(StringView url, StringView subprotocols = StringView());
  bool websocket_open() const { return state_ == HttpState::Upgraded && !ws_closed_; }
  // The subprotocol the server picked ("" if none).
  StringView ws_protocol() const { return ws_protocol_.view(); }
  bool ws_compressed() const { return ws_deflate_; }
  // Messages out (queued and sent as the connection allows). False if the WebSocket is not open, is closing, or the queue is full.
  bool ws_send_text(StringView text);
  bool ws_send_binary(ConstByteView data);
  bool ws_ping(ConstByteView payload = ConstByteView());
  // Starts the closing handshake (code 1000 is "normal"); the WebSocket is closed when the server has answered or after a few seconds.
  bool ws_close(uint16_t code = kWsNormal, StringView reason = StringView());
  // The next message, if one has arrived (call poll() or run something that polls first). Ping, Pong and Close frames are dealt with on the way.
  bool ws_receive(WebSocketMessage* message);
  // The WebSocket has ended (the close handshake is over, or the connection broke or the server broke the rules); ws_close_code() says how.
  bool ws_closed() const { return ws_closed_; }
  uint16_t ws_close_code() const { return ws_close_code_; }          // the server's code, or 1006 if the connection just ended
  StringView ws_close_reason() const { return ws_close_reason_.view(); }
  // The status of the answer to the upgrade request (101 when it was accepted).
  uint32_t ws_handshake_status() const { return status_; }

  HttpState state() const { return state_; }
  bool busy() const { return state_ != HttpState::Idle && state_ != HttpState::Done && state_ != HttpState::Failed && state_ != HttpState::Upgraded; }
  HttpError error() const { return error_; }

  // --- The answer
  uint32_t status() const { return status_; }                 // 200, 404, ...; 0 until the head has arrived
  StringView reason() const { return reason_.view(); }
  // The first header called `name` (any case), without surrounding blanks; empty if there is none.
  StringView header(StringView name) const;
  ConstByteView body() const { return body_.view(); }
  uint64_t bytes_received() const { return received_; }       // body bytes (before decompression)
  // The body length the server announced, or ~0 when it did not (chunked, or until the connection closes).
  uint64_t content_length() const { return content_length_; }
  bool body_was_decoded() const { return decoded_; }          // a gzip / deflate body was inflated into body()
  uint32_t redirects() const { return redirect_count_; }
  const HttpUrl& url() const { return url_; }                 // where the answer came from, after redirects
  bool used_http3() const { return h3_used_; }                // the answer came over HTTP/3
  bool tls_resumed() const { return tls_resumed_; }           // the TLS session was resumed from a ticket (no certificate exchange)

  // --- TLS details of a failed https request
  TlsError tls_error() const { return tls_error_; }
  crypto::CertStatus certificate_status() const { return cert_status_; }
  uint8_t tls_alert() const { return tls_alert_; }
  const char* tls_cipher_suite() const { return cipher_suite_; }

 private:
  enum class Body : uint8_t { None, Length, Chunked, UntilClose };
  enum class Chunk : uint8_t { Size, Data, DataEnd, Trailers };

  bool begin_connection();
  void fail(HttpError error);
  void finish();
  void release_socket(bool graceful);
  void capture_tls();
  bool build_request();
  bool pump_receive();
  bool feed(ConstByteView data);
  bool feed_head(ConstByteView data);
  bool parse_head();
  bool feed_body(ConstByteView data);
  bool feed_chunked(ConstByteView data);
  bool emit(ConstByteView data);
  bool follow_redirect();
  bool decode_body();
  bool begin_websocket_connection();
  bool open_tcp();
  bool try_http3();
  void poll_http3(uint64_t now);
  bool h3_head();
  void h3_drop(bool learn_failure);
  void learn_alt_svc();
  bool accept_upgrade(ConstByteView rest);
  void ws_service();
  void ws_flush();
  void ws_finish(uint16_t code);
  bool ws_queue(WsOpcode opcode, ConstByteView payload);
  void fill_random(ByteView out);
  StringView find_header(StringView block, StringView name) const;
  bool has_extra_header(StringView name) const;

  INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  // settings
  TlsConfig tls_;
  bool has_tls_ = false;
  uint32_t max_redirects_ = 5;
  uint64_t max_body_ = 16ull << 20;
  HttpBodySink sink_ = nullptr;
  void* sink_context_ = nullptr;
  uint32_t connect_timeout_ms_ = 15000, stall_timeout_ms_ = 30000;
  String extra_headers_;          // "Name: value\r\n" lines
  // the request
  HttpMethod method_ = HttpMethod::Get;
  HttpUrl url_;
  ConstByteView request_body_;
  String content_type_;
  uint32_t redirect_count_ = 0;
  // progress
  HttpState state_ = HttpState::Idle;
  HttpError error_ = HttpError::None;
  SharedPointer<IDnsQuery> dns_;
  Ipv4Address address_;
  bool literal_address_ = false;
  SharedPointer<ITcpSocket> socket_;
  TlsSocket* tls_socket_ = nullptr;   // socket_ when it is TLS
  ByteBuffer request_head_;
  uint64_t sent_ = 0, request_total_ = 0;
  uint64_t deadline_ms_ = 0, last_progress_ms_ = 0;
  // the answer
  ByteBuffer head_;               // the status line and headers, as received
  bool head_done_ = false;
  uint32_t status_ = 0;
  String reason_, headers_;       // headers_: the header lines after the status line
  Body body_mode_ = Body::None;
  Chunk chunk_ = Chunk::Size;
  uint64_t content_length_ = ~uint64_t{0}, remaining_ = 0, received_ = 0, emitted_ = 0;
  char line_[96] = {};
  uint32_t line_length_ = 0;
  ByteBuffer body_;
  bool decoded_ = false;
  TlsError tls_error_ = TlsError::None;
  crypto::CertStatus cert_status_ = crypto::CertStatus::Ok;
  uint8_t tls_alert_ = 0;
  const char* cipher_suite_ = "";
  // HTTP/3
  Http3Mode http3_mode_ = Http3Mode::Auto;
  AltSvcCache* alt_svc_ = nullptr;
  TlsSessionCache* session_cache_ = nullptr;
  bool tls_resumed_ = false;
  UniquePointer<Http3Exchange> h3_;
  bool h3_failed_ = false, h3_used_ = false, address_ready_ = false;
  uint16_t h3_port_ = 0;
  // WebSockets
  crypto::IRandom* random_ = nullptr;
  uint64_t fallback_state_ = 0;
  bool ws_offer_deflate_ = false;
  bool websocket_ = false, ws_deflate_ = false, ws_sent_close_ = false, ws_closed_ = false, ws_have_message_ = false, ws_message_text_ = false;
  String ws_key_, ws_offered_, ws_protocol_, ws_close_reason_;
  UniquePointer<WsReader> ws_reader_;
  ByteBuffer ws_out_, ws_message_;
  uint64_t ws_out_sent_ = 0, ws_close_deadline_ = 0;
  uint16_t ws_close_code_ = 0;
  uint32_t ws_deflate_window_ = 15;   // the most the server lets this side's compressor reach back (client_max_window_bits)
};

}  // namespace network
