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

#include "Network/Http/HttpClient.hpp"
#include "Kernel/Thread.hpp"
#include "Media/Zlib.hpp"
#include "Utility/Move.hpp"

namespace network {

const char* http_method_name(HttpMethod method) {
  switch (method) {
    case HttpMethod::Get: return "GET";
    case HttpMethod::Head: return "HEAD";
    case HttpMethod::Post: return "POST";
    case HttpMethod::Put: return "PUT";
    case HttpMethod::Delete: return "DELETE";
    case HttpMethod::Patch: return "PATCH";
  }
  return "GET";
}

const char* http_error_name(HttpError error) {
  switch (error) {
    case HttpError::None: return "none";
    case HttpError::BadUrl: return "bad URL";
    case HttpError::NoTlsConfiguration: return "no TLS configuration for https";
    case HttpError::Resolve: return "host name not found";
    case HttpError::Connect: return "connection failed";
    case HttpError::Tls: return "TLS failed";
    case HttpError::Timeout: return "timed out";
    case HttpError::Protocol: return "invalid HTTP answer";
    case HttpError::TooLarge: return "answer too large";
    case HttpError::TooManyRedirects: return "too many redirects";
    case HttpError::InsecureRedirect: return "redirect from https to http";
    case HttpError::Refused: return "stopped by the program";
    case HttpError::Cancelled: return "cancelled";
    case HttpError::OutOfMemory: return "out of memory";
    case HttpError::WebSocketHandshake: return "WebSocket upgrade refused";
  }
  return "";
}

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool is_token_char(char c) {
  if (c >= '0' && c <= '9') return true;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  for (const char* p = "!#$%&'*+-.^_`|~"; *p; ++p)
    if (c == *p) return true;
  return false;
}

StringView trim_blanks(StringView s) {
  while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s = s.without_prefix(1);
  while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t')) s = s.first(s.size() - 1);
  return s;
}

// Does the comma separated list `list` hold `token` (any case)?
bool list_has(StringView list, StringView token) {
  uint64_t at = 0;
  while (at <= list.size()) {
    uint64_t end = list.find(',', at);
    if (end == StringView::npos) end = list.size();
    if (trim_blanks(list.substr(at, end - at)).equals_ignore_case(token)) return true;
    at = end + 1;
  }
  return false;
}

// The last token of a comma separated list.
StringView last_token(StringView list) {
  const uint64_t comma = list.rfind(',');
  return trim_blanks(comma == StringView::npos ? list : list.substr(comma + 1));
}

bool parse_decimal(StringView s, uint64_t* out) {
  if (s.empty() || s.size() > 18) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *out = v;
  return true;
}

bool is_redirect(uint32_t status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// gzip (RFC 1952): header, deflate data, CRC-32 and length. The CRC is not checked (TLS or TCP already protect the bytes), the length is.
bool gunzip(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
  const uint8_t flags = in[3];
  uint64_t at = 10;
  if (flags & 0x04) {   // extra field
    if (at + 2 > in.size()) return false;
    at += 2 + (in[at] | (in[at + 1] << 8));
  }
  const uint8_t text_fields[2] = {0x08, 0x10};
  for (uint8_t bit : text_fields) {   // file name, comment: zero terminated
    if (flags & bit) {
      while (at < in.size() && in[at] != 0) ++at;
      ++at;
    }
  }
  if (flags & 0x02) at += 2;   // header CRC
  if (at + 8 > in.size()) return false;
  const uint64_t before = out->size();
  uint64_t consumed = 0;
  if (!media::inflate(in.subview(at, in.size() - at - 8), out, max_output, true, &consumed)) return false;
  const uint8_t* trailer = in.data() + in.size() - 4;
  const uint32_t size = trailer[0] | (trailer[1] << 8) | (trailer[2] << 16) | (uint32_t{trailer[3]} << 24);
  return static_cast<uint32_t>(out->size() - before) == size;
}

}  // namespace

HttpClient::HttpClient(INetworkStack& stack, allocators::IAllocator& allocator)
    : stack_(stack), allocator_(allocator), extra_headers_(allocator), url_(allocator), content_type_(allocator), request_head_(allocator), head_(allocator),
      reason_(allocator), headers_(allocator), body_(allocator), ws_key_(allocator), ws_offered_(allocator), ws_protocol_(allocator), ws_close_reason_(allocator),
      ws_out_(allocator), ws_message_(allocator) {}

HttpClient::~HttpClient() { release_socket(false); }

bool HttpClient::add_header(StringView name, StringView value) {
  if (name.empty()) return false;
  for (char c : name)
    if (!is_token_char(c)) return false;
  for (char c : value)
    if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7F) return false;
  return extra_headers_.append(name) && extra_headers_.append(": ") && extra_headers_.append(trim_blanks(value)) && extra_headers_.append("\r\n");
}

bool HttpClient::has_extra_header(StringView name) const { return !find_header(extra_headers_.view(), name).empty(); }

// The value of the first header called `name` in a block of "Name: value\r\n" lines.
StringView HttpClient::find_header(StringView block, StringView name) const {
  uint64_t at = 0;
  while (at < block.size()) {
    uint64_t end = block.find('\n', at);
    if (end == StringView::npos) end = block.size();
    StringView line = block.substr(at, end - at);
    if (!line.empty() && line[line.size() - 1] == '\r') line = line.first(line.size() - 1);
    const uint64_t colon = line.find(':');
    if (colon != StringView::npos && line.first(colon).equals_ignore_case(name)) return trim_blanks(line.substr(colon + 1));
    at = end + 1;
  }
  return StringView();
}

StringView HttpClient::header(StringView name) const { return find_header(headers_.view(), name); }

void HttpClient::capture_tls() {
  if (h3_) {
    const QuicConnection& c = h3_->connection();
    tls_error_ = c.tls_error();
    cert_status_ = c.certificate_status();
    tls_alert_ = c.tls_alert();
    cipher_suite_ = c.cipher_suite_name();
    tls_resumed_ = h3_ && h3_->connection().tls_resumed();
    return;
  }
  if (!tls_socket_) return;
  tls_error_ = tls_socket_->error();
  cert_status_ = tls_socket_->certificate_status();
  tls_alert_ = tls_socket_->alert_received();
  cipher_suite_ = tls_socket_->cipher_suite_name();
  tls_resumed_ = tls_socket_->resumed();
}

void HttpClient::release_socket(bool graceful) {
  capture_tls();
  if (socket_) {
    if (graceful) socket_->close();
    else socket_->abort();
  }
  socket_ = SharedPointer<ITcpSocket>();
  tls_socket_ = nullptr;
  h3_ = UniquePointer<Http3Exchange>();
}

void HttpClient::fail(HttpError error) {
  if (state_ == HttpState::Failed || state_ == HttpState::Done) return;
  release_socket(false);
  dns_ = SharedPointer<IDnsQuery>();
  error_ = error;
  state_ = HttpState::Failed;
}

void HttpClient::cancel() {
  if (busy()) fail(HttpError::Cancelled);
}

bool HttpClient::start(HttpMethod method, StringView url, ConstByteView body, StringView content_type) {
  cancel();
  release_socket(false);
  websocket_ = false;
  ws_reader_ = UniquePointer<WsReader>();
  ws_closed_ = ws_have_message_ = ws_sent_close_ = ws_deflate_ = false;
  error_ = HttpError::None;
  state_ = HttpState::Idle;
  redirect_count_ = 0;
  tls_error_ = TlsError::None;
  cert_status_ = crypto::CertStatus::Ok;
  tls_alert_ = 0;
  cipher_suite_ = "";
  tls_resumed_ = false;
  HttpUrl parsed(allocator_);
  if (!parse_http_url(url, &parsed)) {
    error_ = HttpError::BadUrl;
    state_ = HttpState::Failed;
    return false;
  }
  url_ = util::move(parsed);
  method_ = method;
  request_body_ = body;
  if (!content_type_.assign(content_type)) {
    error_ = HttpError::OutOfMemory;
    state_ = HttpState::Failed;
    return false;
  }
  return begin_connection();
}

void HttpClient::fill_random(ByteView out) {
  crypto::IRandom* source = random_ ? random_ : (has_tls_ ? tls_.random : nullptr);
  if (source && source->fill(out)) return;
  // no random source: xorshift seeded from the clock
  if (fallback_state_ == 0) fallback_state_ = stack_.now_ms() * 0x9E3779B97F4A7C15ull + 0x1234567ull;
  for (uint64_t i = 0; i < out.size(); ++i) {
    fallback_state_ ^= fallback_state_ << 13;
    fallback_state_ ^= fallback_state_ >> 7;
    fallback_state_ ^= fallback_state_ << 17;
    out[i] = static_cast<uint8_t>(fallback_state_ >> 24);
  }
}

bool HttpClient::start_websocket(StringView url, StringView subprotocols) {
  cancel();
  release_socket(false);
  error_ = HttpError::None;
  state_ = HttpState::Idle;
  redirect_count_ = 0;
  tls_error_ = TlsError::None;
  cert_status_ = crypto::CertStatus::Ok;
  tls_alert_ = 0;
  cipher_suite_ = "";
  ws_reader_ = UniquePointer<WsReader>();
  ws_closed_ = ws_have_message_ = ws_sent_close_ = ws_deflate_ = false;
  ws_close_code_ = 0;
  ws_close_reason_.clear();
  ws_protocol_.clear();
  ws_out_.clear();
  ws_out_sent_ = 0;
  ws_message_.clear();
  // ws:// and wss:// are http:// and https:// as far as connecting goes
  String converted(allocator_);
  bool ok = true;
  if (url.size() >= 5 && url.first(5).equals_ignore_case("ws://")) ok = converted.assign("http://") && converted.append(url.substr(5));
  else if (url.size() >= 6 && url.first(6).equals_ignore_case("wss://")) ok = converted.assign("https://") && converted.append(url.substr(6));
  else ok = converted.assign(url);
  HttpUrl parsed(allocator_);
  if (!ok || !parse_http_url(converted.view(), &parsed)) {
    error_ = ok ? HttpError::BadUrl : HttpError::OutOfMemory;
    state_ = HttpState::Failed;
    return false;
  }
  // the subprotocols must be tokens
  for (uint64_t i = 0; i < subprotocols.size(); ++i) {
    const char c = subprotocols[i];
    if (!is_token_char(c) && c != ',' && c != ' ') {
      error_ = HttpError::BadUrl;
      state_ = HttpState::Failed;
      return false;
    }
  }
  uint8_t key[16];
  fill_random(ByteView(key, sizeof key));
  ws_key_.clear();
  if (!ws_base64(ConstByteView(key, sizeof key), &ws_key_) || !ws_offered_.assign(subprotocols)) {
    error_ = HttpError::OutOfMemory;
    state_ = HttpState::Failed;
    return false;
  }
  url_ = util::move(parsed);
  method_ = HttpMethod::Get;
  request_body_ = ConstByteView();
  content_type_.clear();
  websocket_ = true;
  return begin_connection();
}

// Builds the request for url_ and starts finding the server.
bool HttpClient::begin_connection() {
  release_socket(false);
  head_.clear();
  head_done_ = false;
  status_ = 0;
  reason_.clear();
  headers_.clear();
  body_.clear();
  body_mode_ = Body::None;
  chunk_ = Chunk::Size;
  content_length_ = ~uint64_t{0};
  remaining_ = received_ = 0;
  line_length_ = 0;
  decoded_ = false;
  sent_ = 0;
  h3_failed_ = h3_used_ = address_ready_ = false;
  if (url_.https && !has_tls_) {
    error_ = HttpError::NoTlsConfiguration;
    state_ = HttpState::Failed;
    return false;
  }
  if (!build_request()) {
    error_ = HttpError::OutOfMemory;
    state_ = HttpState::Failed;
    return false;
  }
  const uint64_t now = stack_.now_ms();
  deadline_ms_ = now + connect_timeout_ms_;
  last_progress_ms_ = now;
  literal_address_ = parse_ipv4(url_.host.view(), &address_);
  if (literal_address_) {
    state_ = HttpState::Resolving;   // (poll() opens the connection at once)
    dns_ = SharedPointer<IDnsQuery>();
    return true;
  }
  dns_ = stack_.dns().query(url_.host.view());
  if (!dns_) {
    error_ = HttpError::Resolve;
    state_ = HttpState::Failed;
    return false;
  }
  state_ = HttpState::Resolving;
  return true;
}

bool HttpClient::build_request() {
  request_head_.clear();
  auto text = [&](const char* s) {
    uint64_t n = 0;
    while (s[n]) ++n;
    return request_head_.append(ConstByteView(reinterpret_cast<const uint8_t*>(s), n));
  };
  auto view = [&](StringView s) { return request_head_.append(ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size())); };
  const bool with_body = method_ == HttpMethod::Post || method_ == HttpMethod::Put || method_ == HttpMethod::Patch;
  bool ok = text(http_method_name(method_)) && text(" ") && view(url_.path.view()) && text(" HTTP/1.1\r\nHost: ") && view(url_.host.view());
  if (!url_.default_port()) {
    String port(allocator_);
    ok = ok && text(":") && port.append_uint(url_.port) && view(port.view());
  }
  ok = ok && text("\r\n");
  if (!has_extra_header("user-agent")) ok = ok && text("User-Agent: TheoryCraftOS\r\n");
  if (websocket_) {
    ok = ok && text("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ") && view(ws_key_.view()) && text("\r\nSec-WebSocket-Version: 13\r\n");
    if (!ws_offered_.empty()) ok = ok && text("Sec-WebSocket-Protocol: ") && view(ws_offered_.view()) && text("\r\n");
    if (ws_offer_deflate_) ok = ok && text("Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; server_no_context_takeover\r\n");
    ok = ok && view(extra_headers_.view()) && text("\r\n");
    request_total_ = request_head_.size();
    return ok;
  }
  if (!has_extra_header("accept")) ok = ok && text("Accept: */*\r\n");
  if (!has_extra_header("accept-encoding")) ok = ok && text(sink_ ? "Accept-Encoding: identity\r\n" : "Accept-Encoding: gzip, deflate\r\n");
  ok = ok && text("Connection: close\r\n");
  if (with_body) {
    if (!content_type_.empty()) ok = ok && text("Content-Type: ") && view(content_type_.view()) && text("\r\n");
    String length(allocator_);
    ok = ok && length.append_uint(request_body_.size()) && text("Content-Length: ") && view(length.view()) && text("\r\n");
  }
  ok = ok && view(extra_headers_.view()) && text("\r\n");
  request_total_ = request_head_.size() + (with_body ? request_body_.size() : 0);
  return ok;
}

// Opens the TCP connection (wrapped in TLS for https) to address_. False if the request failed.
bool HttpClient::open_tcp() {
  if (url_.https) {
    TlsConfig config = tls_;
    config.server_name = literal_address_ ? nullptr : url_.host.c_str();
    config.alpn = "http/1.1";
    if (session_cache_) config.sessions = session_cache_;
    SharedPointer<ITcpSocket> socket = SharedPointer<ITcpSocket>::make_as<TlsSocket>(allocator_, stack_, allocator_, config);
    tls_socket_ = static_cast<TlsSocket*>(socket.get());
    socket_ = util::move(socket);
  } else {
    socket_ = stack_.tcp_open();
  }
  if (!socket_) {
    fail(HttpError::OutOfMemory);
    return false;
  }
  if (!socket_->connect(address_, url_.port)) {
    fail(HttpError::Connect);
    return false;
  }
  state_ = HttpState::Connecting;
  return true;
}

// ---- HTTP/3

namespace {

void lower_into(String* out, StringView text) {
  for (uint64_t i = 0; i < text.size(); ++i) out->push_back(text[i] >= 'A' && text[i] <= 'Z' ? static_cast<char>(text[i] + 32) : text[i]);
}

bool connection_specific(StringView name) {
  return name == "connection" || name == "host" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding" || name == "upgrade" || name == "te";
}

}  // namespace

// Starts the request over HTTP/3 if the policy and the Alt-Svc cache allow it. True if it has been started (the exchange is in h3_).
bool HttpClient::try_http3() {
  if (!url_.https || !has_tls_ || websocket_ || literal_address_ || http3_mode_ == Http3Mode::Off) return false;
  uint16_t port = url_.port;
  const bool known = alt_svc_ && alt_svc_->find(url_.host.view(), url_.port, stack_.now_ms(), &port);
  if (!known) {
    if (http3_mode_ != Http3Mode::Prefer) return false;
    port = url_.port;
    if (alt_svc_ && alt_svc_->is_broken(url_.host.view(), url_.port, stack_.now_ms())) return false;   // (it failed lately)
  }
  h3::HeaderList request(allocator_);
  String authority(allocator_);
  bool ok = authority.assign(url_.host.view());
  if (!url_.default_port()) ok = ok && authority.push_back(':') && authority.append_uint(url_.port);
  const bool with_body = method_ == HttpMethod::Post || method_ == HttpMethod::Put || method_ == HttpMethod::Patch;
  ok = ok && request.add(":method", http_method_name(method_)) && request.add(":scheme", "https") && request.add(":authority", authority.view()) &&
       request.add(":path", url_.path.view());
  if (!has_extra_header("user-agent")) ok = ok && request.add("user-agent", "TheoryCraftOS");
  if (!has_extra_header("accept")) ok = ok && request.add("accept", "*/*");
  if (!has_extra_header("accept-encoding")) ok = ok && request.add("accept-encoding", sink_ ? "identity" : "gzip, deflate");
  if (with_body) {
    String length(allocator_);
    if (!content_type_.empty()) ok = ok && request.add("content-type", content_type_.view());
    ok = ok && length.append_uint(request_body_.size()) && request.add("content-length", length.view());
  }
  const StringView block = extra_headers_.view();
  for (uint64_t at = 0; ok && at < block.size();) {
    uint64_t end = block.find('\n', at);
    end = end == StringView::npos ? block.size() : end + 1;
    StringView line = block.substr(at, end - at);
    at = end;
    while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) line = line.first(line.size() - 1);
    const uint64_t colon = line.find(':');
    if (colon == StringView::npos) continue;
    String name(allocator_);
    lower_into(&name, line.first(colon));
    if (connection_specific(name.view())) continue;
    StringView value = line.substr(colon + 1);
    while (!value.empty() && value[0] == ' ') value = value.substr(1);
    ok = ok && request.add(name.view(), value);
  }
  QuicConfig config;
  config.tls = tls_;
  config.tls.server_name = url_.host.c_str();
  if (session_cache_) config.tls.sessions = session_cache_;
  config.handshake_timeout_ms = connect_timeout_ms_ < 5000 ? connect_timeout_ms_ : 5000;
  UniquePointer<Http3Exchange> exchange;
  if (ok) exchange = UniquePointer<Http3Exchange>::make(allocator_, stack_, allocator_, config);
  if (!ok || !exchange || !exchange->start(address_, port, url_.host.view(), request, with_body ? request_body_ : ConstByteView())) {
    h3_failed_ = true;
    return false;
  }
  h3_ = util::move(exchange);
  h3_port_ = port;
  state_ = HttpState::Connecting;
  const uint64_t now = stack_.now_ms();
  deadline_ms_ = now + connect_timeout_ms_;
  last_progress_ms_ = now;
  return true;
}

// The QUIC attempt is given up before the server has taken the request: the request goes over TCP instead.
void HttpClient::h3_drop(bool learn_failure) {
  if (learn_failure && alt_svc_) alt_svc_->mark_broken(url_.host.view(), url_.port, stack_.now_ms());
  h3_ = UniquePointer<Http3Exchange>();
  h3_failed_ = true;
  tls_error_ = TlsError::None;
  cert_status_ = crypto::CertStatus::Ok;
  tls_alert_ = 0;
  cipher_suite_ = "";
  state_ = HttpState::Resolving;
  const uint64_t now = stack_.now_ms();
  deadline_ms_ = now + connect_timeout_ms_;
  last_progress_ms_ = now;
}

void HttpClient::learn_alt_svc() {
  if (!alt_svc_ || !url_.https) return;
  const StringView value = header("alt-svc");
  if (!value.empty()) alt_svc_->learn(url_.host.view(), url_.port, value, stack_.now_ms());
}

// The final head of the answer has arrived: status and headers are taken over and the body is laid out. False once the request has ended (a redirect included).
bool HttpClient::h3_head() {
  const h3::HeaderList& fields = h3_->response();
  status_ = h3_->status();
  reason_.clear();
  headers_.clear();
  for (uint64_t i = 0; i < fields.count(); ++i) {
    if (!headers_.append(fields.name(i)) || !headers_.append(": ") || !headers_.append(fields.value(i)) || !headers_.append("\r\n")) {
      fail(HttpError::OutOfMemory);
      return false;
    }
  }
  body_mode_ = Body::UntilClose;
  content_length_ = ~uint64_t{0};
  if (method_ == HttpMethod::Head || status_ == 204 || status_ == 304) {
    body_mode_ = Body::None;
  } else if (!header("content-length").empty()) {
    uint64_t length = 0;
    if (!parse_decimal(header("content-length"), &length)) {
      fail(HttpError::Protocol);
      return false;
    }
    content_length_ = length;
    body_mode_ = length == 0 ? Body::None : Body::Length;
    if (!sink_ && length > max_body_) {
      fail(HttpError::TooLarge);
      return false;
    }
  }
  head_done_ = true;
  h3_used_ = true;
  state_ = HttpState::ReceivingBody;
  learn_alt_svc();
  if (is_redirect(status_) && max_redirects_ > 0 && !header("location").empty()) {
    follow_redirect();
    return false;
  }
  return true;
}

void HttpClient::poll_http3(uint64_t now) {
  Http3Exchange& x = *h3_;
  x.poll();
  const Http3Phase phase = x.phase();
  if (phase == Http3Phase::Failed) {
    const Http3Error e = x.error();
    const bool fall_back = !x.head_ready() && (e == Http3Error::Refused || (e == Http3Error::Connection && (!x.connection().established() || method_ == HttpMethod::Get || method_ == HttpMethod::Head)));
    if (fall_back) return h3_drop(true);
    capture_tls();
    switch (e) {
      case Http3Error::Connection:
        return fail(x.connection().error() == QuicError::Tls ? HttpError::Tls : x.connection().error() == QuicError::Timeout ? HttpError::Timeout : HttpError::Connect);
      case Http3Error::TooLarge: return fail(HttpError::TooLarge);
      case Http3Error::Internal: return fail(HttpError::OutOfMemory);
      case Http3Error::Cancelled: return fail(HttpError::Cancelled);
      default: return fail(HttpError::Protocol);
    }
  }
  if (state_ == HttpState::Connecting && phase != Http3Phase::Connecting) {
    state_ = HttpState::ReceivingHead;
    last_progress_ms_ = stack_.now_ms();
  }
  if (x.head_ready() && !head_done_ && !h3_head()) return;
  if (head_done_) {
    const ConstByteView pending = x.pending_body();
    if (!pending.empty()) {
      last_progress_ms_ = stack_.now_ms();
      if (body_mode_ != Body::None && !emit(pending)) return;
      x.consume_body(pending.size());
    }
    if (phase == Http3Phase::Done && x.pending_body().empty()) {
      if (body_mode_ == Body::Length && received_ != content_length_) return fail(HttpError::Protocol);
      return finish();
    }
  }
  // (`now` is older than a progress stamp taken above: see poll())
  if (busy() && now > last_progress_ms_ && now - last_progress_ms_ > stall_timeout_ms_) fail(HttpError::Timeout);
}

void HttpClient::poll() {
  if (state_ == HttpState::Upgraded) {
    ws_service();
    return;
  }
  if (!busy()) return;
  const uint64_t now = stack_.now_ms();
  if (state_ == HttpState::Resolving) {
    if (!literal_address_ && !address_ready_) {
      if (!dns_) return fail(HttpError::Resolve);
      const DnsStatus status = dns_->status();
      if (status == DnsStatus::Pending) {
        if (now > deadline_ms_) fail(HttpError::Timeout);
        return;
      }
      if (status != DnsStatus::Resolved) return fail(HttpError::Resolve);
      address_ = dns_->address();
      dns_ = SharedPointer<IDnsQuery>();
      address_ready_ = true;
    }
    if (!h3_failed_ && try_http3()) return;
    if (!open_tcp()) return;
  }
  if (h3_) return poll_http3(now);
  if (state_ == HttpState::Connecting) {
    if (socket_->failed()) return fail(tls_socket_ ? HttpError::Tls : HttpError::Connect);
    if (now > deadline_ms_) return fail(HttpError::Timeout);
    if (!socket_->is_connected()) return;
    last_progress_ms_ = now;
    state_ = HttpState::Sending;
  }
  if (socket_->failed() && state_ != HttpState::ReceivingBody) return fail(tls_socket_ ? HttpError::Tls : HttpError::Connect);
  if (state_ == HttpState::Sending) {
    const bool with_body = method_ == HttpMethod::Post || method_ == HttpMethod::Put || method_ == HttpMethod::Patch;
    while (sent_ < request_total_) {
      const ConstByteView part = sent_ < request_head_.size() ? request_head_.view().subview(sent_) : request_body_.subview(sent_ - request_head_.size());
      const uint32_t n = socket_->send(part);
      if (n == 0) break;
      sent_ += n;
      last_progress_ms_ = now;
    }
    (void)with_body;
    if (sent_ == request_total_) state_ = HttpState::ReceivingHead;
  }
  // (An answer can start before a long request body is out; read in both states.)
  if (state_ == HttpState::Sending || state_ == HttpState::ReceivingHead || state_ == HttpState::ReceivingBody) {
    if (!pump_receive()) return;
  }
  // (`now` was read before pump_receive() stamped last_progress_ms_ with a later time, so the difference can be negative: it must not wrap around)
  if (busy() && now > last_progress_ms_ && now - last_progress_ms_ > stall_timeout_ms_) fail(HttpError::Timeout);
}

// Reads what has arrived. False once the request has ended (done, failed, or moved on to a redirect).
bool HttpClient::pump_receive() {
  uint8_t buffer[2048];
  for (uint32_t n; (n = socket_->receive(ByteView(buffer, sizeof buffer))) > 0;) {
    last_progress_ms_ = stack_.now_ms();
    if (!feed(ConstByteView(buffer, n))) return false;
  }
  if (socket_->peer_closed() && socket_->available() == 0) {
    if (state_ == HttpState::ReceivingBody && body_mode_ == Body::UntilClose) {
      finish();
      return false;
    }
    fail(tls_socket_ && socket_->failed() ? HttpError::Tls : HttpError::Protocol);   // closed in the middle of the answer
    return false;
  }
  if (socket_->failed()) {
    fail(tls_socket_ ? HttpError::Tls : HttpError::Connect);
    return false;
  }
  return true;
}

bool HttpClient::feed(ConstByteView data) {
  if (state_ == HttpState::Sending) state_ = HttpState::ReceivingHead;   // (answered before the request was all sent)
  if (state_ == HttpState::ReceivingHead) return feed_head(data);
  return feed_body(data);
}

bool HttpClient::feed_head(ConstByteView data) {
  if (head_.size() + data.size() > kMaxHeadBytes) {
    fail(HttpError::Protocol);
    return false;
  }
  if (!head_.append(data)) {
    fail(HttpError::OutOfMemory);
    return false;
  }
  for (;;) {
    // The end of the head: an empty line.
    const ConstByteView all = head_.view();
    uint64_t end = 0;
    for (uint64_t i = 0; i + 3 < all.size(); ++i)
      if (all[i] == '\r' && all[i + 1] == '\n' && all[i + 2] == '\r' && all[i + 3] == '\n') {
        end = i + 4;
        break;
      }
    if (end == 0) return true;   // not all here yet
    ByteBuffer rest(allocator_);
    if (!rest.append(all.subview(end))) {
      fail(HttpError::OutOfMemory);
      return false;
    }
    head_.truncate(end);
    if (!parse_head()) return false;
    if (websocket_) {   // the answer to the upgrade request: the switch, or a refusal
      if (status_ == 101) {
        accept_upgrade(rest.view());
        return false;   // (reading goes on in poll(), in the Upgraded state)
      }
      if (status_ >= 200) {
        fail(HttpError::WebSocketHandshake);
        return false;
      }
    }
    if (status_ >= 100 && status_ < 200) {   // an interim answer: forget it, the real one follows
      head_.clear();
      if (rest.size() == 0) return true;
      head_.append(rest.view());
      continue;
    }
    head_done_ = true;
    state_ = HttpState::ReceivingBody;
    learn_alt_svc();
    if (is_redirect(status_) && max_redirects_ > 0 && !header("location").empty()) {
      follow_redirect();
      return false;
    }
    if (body_mode_ == Body::None) {
      finish();
      return false;
    }
    return rest.size() == 0 || feed_body(rest.view());
  }
}

// Reads the status line and headers in head_ (ending in an empty line); decides how the body is laid out. False (failed) if they are not valid.
bool HttpClient::parse_head() {
  const StringView text(reinterpret_cast<const char*>(head_.view().data()), head_.size());
  const uint64_t eol = text.find('\n');
  if (eol == StringView::npos) {
    fail(HttpError::Protocol);
    return false;
  }
  StringView line = text.first(eol);
  if (!line.empty() && line[line.size() - 1] == '\r') line = line.first(line.size() - 1);
  // "HTTP/1.1 200 OK"
  if (line.size() < 12 || !(line.starts_with(StringView("HTTP/1.1 ")) || line.starts_with(StringView("HTTP/1.0 ")))) {
    fail(HttpError::Protocol);
    return false;
  }
  uint32_t status = 0;
  for (uint64_t i = 9; i < 12; ++i) {
    if (line[i] < '0' || line[i] > '9') {
      fail(HttpError::Protocol);
      return false;
    }
    status = status * 10 + static_cast<uint32_t>(line[i] - '0');
  }
  if (line.size() > 12 && line[12] != ' ') {
    fail(HttpError::Protocol);
    return false;
  }
  status_ = status;
  const StringView reason = line.size() > 13 ? line.substr(13) : StringView();
  const StringView block = text.substr(eol + 1);
  if (!reason_.assign(reason.size() > 200 ? reason.first(200) : reason) || !headers_.assign(block)) {
    fail(HttpError::OutOfMemory);
    return false;
  }
  if (status == 101) {   // a protocol switch: only a WebSocket upgrade asks for one
    if (!websocket_) {
      fail(HttpError::Protocol);
      return false;
    }
    return true;
  }
  if (status < 200) return true;   // interim
  // How the body is laid out.
  body_mode_ = Body::UntilClose;
  const StringView transfer = header("transfer-encoding");
  uint64_t length = 0;
  if (method_ == HttpMethod::Head || status == 204 || status == 304) {
    body_mode_ = Body::None;
  } else if (!transfer.empty()) {
    if (last_token(transfer).equals_ignore_case("chunked")) body_mode_ = Body::Chunked;
    else body_mode_ = Body::UntilClose;   // (an unknown coding: the data cannot be framed, so read to the end)
  } else if (!header("content-length").empty()) {
    const StringView value = header("content-length");
    if (!parse_decimal(value, &length)) {
      fail(HttpError::Protocol);
      return false;
    }
    content_length_ = length;
    remaining_ = length;
    body_mode_ = length == 0 ? Body::None : Body::Length;
    if (!sink_ && length > max_body_) {
      fail(HttpError::TooLarge);
      return false;
    }
  }
  return true;
}

bool HttpClient::feed_body(ConstByteView data) {
  switch (body_mode_) {
    case Body::None: return true;
    case Body::Length: {
      const uint64_t n = data.size() < remaining_ ? data.size() : remaining_;
      if (!emit(data.first(n))) return false;
      remaining_ -= n;
      if (remaining_ == 0) {
        finish();
        return false;
      }
      return true;
    }
    case Body::UntilClose: return emit(data);
    case Body::Chunked: return feed_chunked(data);
  }
  return true;
}

bool HttpClient::feed_chunked(ConstByteView data) {
  uint64_t i = 0;
  while (i < data.size()) {
    if (chunk_ == Chunk::Data) {
      const uint64_t n = data.size() - i < remaining_ ? data.size() - i : remaining_;
      if (!emit(data.subview(i, n))) return false;
      remaining_ -= n;
      i += n;
      if (remaining_ == 0) {
        chunk_ = Chunk::DataEnd;
        line_length_ = 0;
      }
      continue;
    }
    // A line: the chunk size, the empty line after a chunk's data, or a trailer.
    const char c = static_cast<char>(data[i++]);
    if (c != '\n') {
      if (line_length_ < sizeof line_ - 1) line_[line_length_++] = c;
      else if (chunk_ != Chunk::Trailers) {
        fail(HttpError::Protocol);
        return false;
      }
      continue;
    }
    uint32_t length = line_length_;
    if (length && line_[length - 1] == '\r') --length;
    line_length_ = 0;
    if (chunk_ == Chunk::Size) {
      uint64_t size = 0;
      uint32_t digits = 0;
      for (uint32_t k = 0; k < length; ++k) {
        const char d = lower(line_[k]);
        uint32_t v;
        if (d >= '0' && d <= '9') v = static_cast<uint32_t>(d - '0');
        else if (d >= 'a' && d <= 'f') v = static_cast<uint32_t>(d - 'a' + 10);
        else break;   // an extension (";...") or blanks follow
        if (++digits > 15) {
          fail(HttpError::Protocol);
          return false;
        }
        size = size * 16 + v;
      }
      if (digits == 0) {
        fail(HttpError::Protocol);
        return false;
      }
      if (size == 0) {
        chunk_ = Chunk::Trailers;
      } else {
        remaining_ = size;
        chunk_ = Chunk::Data;
      }
    } else if (chunk_ == Chunk::DataEnd) {
      if (length != 0) {
        fail(HttpError::Protocol);
        return false;
      }
      chunk_ = Chunk::Size;
    } else {   // trailers: ignored; an empty line ends the answer
      if (length == 0) {
        finish();
        return false;
      }
    }
  }
  return true;
}

// Takes a piece of the body: to the sink, or into body_. False (failed) if that cannot be done.
bool HttpClient::emit(ConstByteView data) {
  if (data.empty()) return true;
  received_ += data.size();
  if (sink_) {
    if (!sink_(sink_context_, data)) {
      fail(HttpError::Refused);
      return false;
    }
    return true;
  }
  if (body_.size() + data.size() > max_body_) {
    fail(HttpError::TooLarge);
    return false;
  }
  if (!body_.append(data)) {
    fail(HttpError::OutOfMemory);
    return false;
  }
  return true;
}

bool HttpClient::follow_redirect() {
  const StringView location = header("location");
  if (redirect_count_ >= max_redirects_) {
    fail(HttpError::TooManyRedirects);
    return false;
  }
  HttpUrl next(allocator_);
  if (!resolve_http_reference(url_, location, &next)) {
    fail(HttpError::BadUrl);
    return false;
  }
  if (url_.https && !next.https) {
    fail(HttpError::InsecureRedirect);
    return false;
  }
  // 303 (and 301 / 302 after a POST) turn into a GET without a body; 307 and 308 repeat the request.
  if (status_ == 303 ? method_ != HttpMethod::Head : ((status_ == 301 || status_ == 302) && method_ == HttpMethod::Post)) {
    method_ = HttpMethod::Get;
    request_body_ = ConstByteView();
    content_type_.clear();
  }
  if (!url_.same_origin(next)) {   // credentials stay with the origin they were meant for
    String kept(allocator_);
    const StringView block = extra_headers_.view();
    for (uint64_t at = 0; at < block.size();) {
      uint64_t end = block.find('\n', at);
      end = end == StringView::npos ? block.size() : end + 1;
      const StringView line = block.substr(at, end - at);
      const uint64_t colon = line.find(':');
      const StringView name = colon == StringView::npos ? StringView() : line.first(colon);
      if (!(name.equals_ignore_case("authorization") || name.equals_ignore_case("cookie") || name.equals_ignore_case("proxy-authorization")))
        if (!kept.append(line)) {
          fail(HttpError::OutOfMemory);
          return false;
        }
      at = end;
    }
    extra_headers_ = util::move(kept);
  }
  url_ = util::move(next);
  ++redirect_count_;
  begin_connection();
  return false;
}

// A gzip or deflate body becomes the plain data.
bool HttpClient::decode_body() {
  const StringView encoding = header("content-encoding");
  if (encoding.empty() || encoding.equals_ignore_case("identity") || body_.size() == 0) return true;
  const bool gzip = encoding.equals_ignore_case("gzip") || encoding.equals_ignore_case("x-gzip");
  const bool deflate = encoding.equals_ignore_case("deflate");
  if (!gzip && !deflate) return true;   // some other coding: the body is left as it came
  ByteBuffer plain(allocator_);
  bool ok;
  if (gzip) {
    ok = gunzip(body_.view(), &plain, max_body_);
  } else {   // "deflate" is a zlib stream, though some servers send bare deflate data
    ok = media::inflate(body_.view(), &plain, max_body_, false);
    if (!ok) {
      plain.clear();
      ok = media::inflate(body_.view(), &plain, max_body_, true);
    }
  }
  if (!ok) return false;
  body_.clear();
  body_.append(plain.view());
  decoded_ = true;
  return true;
}

void HttpClient::finish() {
  release_socket(true);
  if (!sink_ && !decode_body()) {
    error_ = HttpError::Protocol;
    state_ = HttpState::Failed;
    return;
  }
  state_ = HttpState::Done;
}

bool HttpClient::run(uint32_t timeout_ms) {
  const uint64_t start = stack_.now_ms();
  while (busy()) {
    stack_.poll();
    poll();
    if (!busy()) break;
    if (stack_.now_ms() - start > timeout_ms) {
      fail(HttpError::Timeout);
      break;
    }
    kernel::sleep_us(100);
  }
  return state_ == HttpState::Done || state_ == HttpState::Upgraded;
}

// ---- WebSockets

// The server said 101: checks the handshake (RFC 6455 4.2.2), then the connection carries frames. `rest` is what came after the head.
bool HttpClient::accept_upgrade(ConstByteView rest) {
  String expected(allocator_);
  bool ok = ws_accept_value(ws_key_.view(), &expected);
  if (!ok) {
    fail(HttpError::OutOfMemory);
    return false;
  }
  const StringView accept = header("sec-websocket-accept");
  ok = header("upgrade").equals_ignore_case("websocket") && list_has(header("connection"), "upgrade") && accept.equals(expected.view());
  // the subprotocol must be one that was offered
  const StringView protocol = header("sec-websocket-protocol");
  if (ok && !protocol.empty()) {
    ok = list_has(ws_offered_.view(), protocol);
    if (ok) ok = ws_protocol_.assign(protocol);
  }
  // the extension: none, or permessage-deflate that was offered, without context takeover in either direction (the only way messages are handled)
  const StringView extensions = header("sec-websocket-extensions");
  bool deflate = false;
  if (ok && !extensions.empty()) {
    const uint64_t semi = extensions.find(';');
    const StringView name = trim_blanks(semi == StringView::npos ? extensions : extensions.first(semi));
    ok = ws_offer_deflate_ && name.equals("permessage-deflate") && extensions.find(',') == StringView::npos;
    if (ok) {
      bool server_no_takeover = false;
      uint64_t at = semi;
      ws_deflate_window_ = 15;
      while (ok && at != StringView::npos) {
        const uint64_t next = extensions.find(';', at + 1);
        const StringView param = trim_blanks(extensions.substr(at + 1, (next == StringView::npos ? extensions.size() : next) - at - 1));
        const uint64_t eq = param.find('=');
        const StringView pname = trim_blanks(eq == StringView::npos ? param : param.first(eq));
        uint64_t bits = 0;
        const bool has_value = eq != StringView::npos && parse_decimal(trim_blanks(param.substr(eq + 1)), &bits);
        if (pname.equals("server_no_context_takeover") && eq == StringView::npos) server_no_takeover = true;
        else if (pname.equals("client_no_context_takeover") && eq == StringView::npos) {}
        else if (pname.equals("server_max_window_bits") && has_value && bits >= 8 && bits <= 15) {}   // (a smaller window of the server's is no trouble to inflate)
        else if (pname.equals("client_max_window_bits") && has_value && bits >= 8 && bits <= 15) ws_deflate_window_ = static_cast<uint32_t>(bits);
        else ok = false;   // something unknown
        at = next;
      }
      ok = ok && server_no_takeover;
      deflate = ok;
    }
  }
  if (!ok) {
    fail(HttpError::WebSocketHandshake);
    return false;
  }
  ws_reader_ = UniquePointer<WsReader>::make(allocator_, allocator_);
  if (!ws_reader_) {
    fail(HttpError::OutOfMemory);
    return false;
  }
  ws_reader_->set_max_message(max_body_);
  ws_reader_->set_deflate(deflate);
  ws_deflate_ = deflate;
  head_done_ = true;
  state_ = HttpState::Upgraded;
  last_progress_ms_ = stack_.now_ms();
  return rest.empty() || ws_reader_->feed(rest);
}

bool HttpClient::ws_queue(WsOpcode opcode, ConstByteView payload) {
  if (state_ != HttpState::Upgraded || ws_closed_ || ws_sent_close_) return false;
  if (ws_out_.size() - ws_out_sent_ > (16u << 20)) return false;   // the connection is not keeping up
  uint8_t mask[4];
  fill_random(ByteView(mask, 4));
  const bool data = opcode == WsOpcode::Text || opcode == WsOpcode::Binary;
  bool ok;
  if (data && ws_deflate_) {
    ByteBuffer packed(allocator_);
    ok = media::deflate_message(payload, &packed, 6, allocator_, ws_deflate_window_) && ws_build_frame(opcode, true, true, packed.view(), mask, &ws_out_);
  } else {
    ok = ws_build_frame(opcode, true, false, payload, mask, &ws_out_);
  }
  if (!ok) return false;
  ws_flush();
  return true;
}

bool HttpClient::ws_send_text(StringView text) {
  return ws_queue(WsOpcode::Text, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}
bool HttpClient::ws_send_binary(ConstByteView data) { return ws_queue(WsOpcode::Binary, data); }
bool HttpClient::ws_ping(ConstByteView payload) { return payload.size() <= 125 && ws_queue(WsOpcode::Ping, payload); }

bool HttpClient::ws_close(uint16_t code, StringView reason) {
  if (state_ != HttpState::Upgraded || ws_closed_ || ws_sent_close_) return false;
  ByteBuffer payload(allocator_);
  if (!ws_close_payload(code, reason, &payload)) return false;
  if (!ws_queue(WsOpcode::Close, payload.view())) return false;
  ws_sent_close_ = true;
  ws_close_deadline_ = stack_.now_ms() + 5000;
  ws_flush();
  return true;
}

void HttpClient::ws_flush() {
  while (socket_ && ws_out_sent_ < ws_out_.size()) {
    const uint32_t n = socket_->send(ws_out_.view().subview(ws_out_sent_));
    if (n == 0) break;
    ws_out_sent_ += n;
    last_progress_ms_ = stack_.now_ms();
  }
  if (ws_out_sent_ == ws_out_.size() && ws_out_sent_ > 0) {
    ws_out_.clear();
    ws_out_sent_ = 0;
  }
}

// The WebSocket is over: nothing more will arrive; the connection is closed once what is queued has gone out.
void HttpClient::ws_finish(uint16_t code) {
  if (ws_closed_) return;
  ws_closed_ = true;
  ws_close_code_ = code;
}

void HttpClient::ws_service() {
  if (!socket_) return;
  const uint64_t now = stack_.now_ms();
  ws_flush();
  if (!ws_closed_) {
    // take in what the server sent, but not more than can be dealt with
    // (a message waiting to be taken holds the rest back - unless this side has started closing: then what arrives is dropped, so the close can finish)
    while ((!ws_have_message_ || ws_sent_close_) && ws_reader_->buffered() < (1u << 20)) {
      uint8_t buffer[2048];
      const uint32_t n = socket_->receive(ByteView(buffer, sizeof buffer));
      if (n == 0) break;
      last_progress_ms_ = now;
      if (!ws_reader_->feed(ConstByteView(buffer, n))) {
        ws_finish(kWsInternalError);
        break;
      }
    }
    while (!ws_closed_ && (!ws_have_message_ || ws_sent_close_)) {
      const WsEvent e = ws_reader_->next();
      if (e.kind == WsEvent::Kind::None) break;
      if (e.kind == WsEvent::Kind::Message) {
        if (ws_sent_close_) continue;
        ws_message_.clear();
        if (!ws_message_.append(e.payload)) {
          ws_finish(kWsInternalError);
          break;
        }
        ws_message_text_ = e.text;
        ws_have_message_ = true;
      } else if (e.kind == WsEvent::Kind::Ping) {
        ByteBuffer copy(allocator_);
        if (!ws_sent_close_ && copy.append(e.payload)) ws_queue(WsOpcode::Pong, copy.view());
      } else if (e.kind == WsEvent::Kind::Close) {
        ws_close_reason_.clear();
        ws_close_reason_.assign(StringView(reinterpret_cast<const char*>(e.payload.data()), e.payload.size()));
        if (!ws_sent_close_) {   // answer it with the same code
          ByteBuffer payload(allocator_);
          if (e.code != kWsNoStatus && ws_close_payload(e.code, StringView(), &payload)) ws_queue(WsOpcode::Close, payload.view());
          else ws_queue(WsOpcode::Close, ConstByteView());
          ws_sent_close_ = true;
        }
        ws_finish(e.code);
      } else if (e.kind == WsEvent::Kind::Failure) {
        if (!ws_sent_close_) {
          ByteBuffer payload(allocator_);
          if (ws_close_payload(e.code, StringView(), &payload)) ws_queue(WsOpcode::Close, payload.view());
          ws_sent_close_ = true;
        }
        ws_finish(e.code);
      }
    }
    if (!ws_closed_ && (!ws_have_message_ || ws_sent_close_) && (socket_->failed() || (socket_->peer_closed() && socket_->available() == 0))) ws_finish(kWsAbnormal);
    if (!ws_closed_ && ws_sent_close_ && now > ws_close_deadline_) ws_finish(kWsAbnormal);
  }
  ws_flush();
  // closed: let go of the connection when everything queued has been handed over (or the peer is gone, or we have waited long enough)
  if (ws_closed_ && !ws_have_message_) {
    const bool drained = ws_out_sent_ >= ws_out_.size();
    if (drained || socket_->failed() || now > ws_close_deadline_ + 2000) {
      release_socket(true);
      state_ = HttpState::Done;
    }
  }
}

bool HttpClient::ws_receive(WebSocketMessage* message) {
  if (state_ == HttpState::Upgraded) ws_service();
  if (!ws_have_message_) return false;
  message->text = ws_message_text_;
  message->data.clear();
  if (!message->data.append(ws_message_.view())) return false;
  ws_have_message_ = false;
  ws_message_.clear();
  if (state_ == HttpState::Upgraded) ws_service();
  return true;
}

}  // namespace network
