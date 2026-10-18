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
      reason_(allocator), headers_(allocator), body_(allocator) {}

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
  if (!tls_socket_) return;
  tls_error_ = tls_socket_->error();
  cert_status_ = tls_socket_->certificate_status();
  tls_alert_ = tls_socket_->alert_received();
  cipher_suite_ = tls_socket_->cipher_suite_name();
}

void HttpClient::release_socket(bool graceful) {
  capture_tls();
  if (socket_) {
    if (graceful) socket_->close();
    else socket_->abort();
  }
  socket_ = SharedPointer<ITcpSocket>();
  tls_socket_ = nullptr;
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
  error_ = HttpError::None;
  state_ = HttpState::Idle;
  redirect_count_ = 0;
  tls_error_ = TlsError::None;
  cert_status_ = crypto::CertStatus::Ok;
  tls_alert_ = 0;
  cipher_suite_ = "";
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

void HttpClient::poll() {
  if (!busy()) return;
  const uint64_t now = stack_.now_ms();
  if (state_ == HttpState::Resolving) {
    if (!literal_address_) {
      if (!dns_) return fail(HttpError::Resolve);
      const DnsStatus status = dns_->status();
      if (status == DnsStatus::Pending) {
        if (now > deadline_ms_) fail(HttpError::Timeout);
        return;
      }
      if (status != DnsStatus::Resolved) return fail(HttpError::Resolve);
      address_ = dns_->address();
      dns_ = SharedPointer<IDnsQuery>();
    }
    // Open the connection (wrapped in TLS for https).
    if (url_.https) {
      TlsConfig config = tls_;
      config.server_name = literal_address_ ? nullptr : url_.host.c_str();
      config.alpn = "http/1.1";
      SharedPointer<ITcpSocket> socket = SharedPointer<ITcpSocket>::make_as<TlsSocket>(allocator_, stack_, allocator_, config);
      tls_socket_ = static_cast<TlsSocket*>(socket.get());
      socket_ = util::move(socket);
    } else {
      socket_ = stack_.tcp_open();
    }
    if (!socket_) return fail(HttpError::OutOfMemory);
    if (!socket_->connect(address_, url_.port)) return fail(HttpError::Connect);
    state_ = HttpState::Connecting;
  }
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
  if (busy() && now - last_progress_ms_ > stall_timeout_ms_) fail(HttpError::Timeout);
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
    if (status_ >= 100 && status_ < 200) {   // an interim answer: forget it, the real one follows
      head_.clear();
      if (rest.size() == 0) return true;
      head_.append(rest.view());
      continue;
    }
    head_done_ = true;
    state_ = HttpState::ReceivingBody;
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
  if (status == 101) {   // a protocol switch: nothing here speaks anything else
    fail(HttpError::Protocol);
    return false;
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
  return state_ == HttpState::Done;
}

}  // namespace network
