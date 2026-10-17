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

#include "Network/Ipp/IppExchange.hpp"

namespace network {

namespace {
constexpr uint64_t kConnectTimeoutMs = 10000;
constexpr uint64_t kStallTimeoutMs = 30000;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// Does the header line starting at `p` (up to `end`) start with `name` (lower case, ending in ':')? Returns the offset of the value or 0.
uint32_t header_value(const uint8_t* p, uint32_t length, const char* name) {
  uint32_t i = 0;
  for (; name[i]; ++i)
    if (i >= length || lower(static_cast<char>(p[i])) != name[i]) return 0;
  while (i < length && (p[i] == ' ' || p[i] == '\t')) ++i;
  return i;
}

void append_text(ByteBuffer& b, const char* s) {
  uint32_t n = 0;
  while (s[n]) ++n;
  b.append(ConstByteView(reinterpret_cast<const uint8_t*>(s), n));
}
void append_number(ByteBuffer& b, uint64_t v) {
  char digits[21];
  uint32_t k = 0;
  do { digits[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
  char out[21];
  for (uint32_t i = 0; i < k; ++i) out[i] = digits[k - 1 - i];
  b.append(ConstByteView(reinterpret_cast<const uint8_t*>(out), k));
}
}  // namespace

bool IppExchange::start(Ipv4Address address, uint16_t port, const char* path, ConstByteView ipp_head, ConstByteView document) {
  if (busy()) return false;
  cancel();
  head_.clear();
  raw_.clear();
  body_.clear();
  http_status_ = 0;
  sent_ = 0;
  document_ = document;
  const uint64_t content = ipp_head.size() + document.size();
  append_text(head_, "POST ");
  append_text(head_, path && path[0] ? path : "/");
  append_text(head_, " HTTP/1.1\r\nHost: ");
  for (int i = 0; i < 4; ++i) {
    append_number(head_, (address.value() >> (24 - 8 * i)) & 0xff);
    if (i < 3) append_text(head_, ".");
  }
  append_text(head_, ":");
  append_number(head_, port);
  append_text(head_, "\r\nUser-Agent: TheoryCraftOS\r\nContent-Type: application/ipp\r\nAccept: application/ipp\r\nConnection: close\r\nContent-Length: ");
  append_number(head_, content);
  append_text(head_, "\r\n\r\n");
  if (!head_.append(ipp_head)) {
    state_ = IppExchangeState::Failed;
    return false;
  }
  total_ = head_.size() + document.size();
  socket_ = stack_.tcp_open();
  if (!socket_ || !socket_->connect(address, port)) {
    socket_ = SharedPointer<ITcpSocket>();
    state_ = IppExchangeState::Failed;
    return false;
  }
  deadline_ms_ = stack_.now_ms() + kConnectTimeoutMs;
  last_progress_ms_ = stack_.now_ms();
  state_ = IppExchangeState::Connecting;
  return true;
}

void IppExchange::fail() {
  if (socket_) socket_->abort();
  socket_ = SharedPointer<ITcpSocket>();
  state_ = IppExchangeState::Failed;
}

void IppExchange::cancel() {
  if (socket_) socket_->abort();
  socket_ = SharedPointer<ITcpSocket>();
  state_ = IppExchangeState::Idle;
}

void IppExchange::poll() {
  if (!socket_ || !busy()) return;
  const uint64_t now = stack_.now_ms();
  if (state_ == IppExchangeState::Connecting) {
    if (socket_->failed() || now > deadline_ms_) return fail();
    if (!socket_->is_connected()) return;
    last_progress_ms_ = now;
    state_ = IppExchangeState::Sending;
  }
  if (socket_->failed()) return fail();
  if (state_ == IppExchangeState::Sending) {
    while (sent_ < total_) {
      const ConstByteView part = sent_ < head_.size() ? head_.view().subview(sent_) : document_.subview(sent_ - head_.size());
      const uint32_t n = socket_->send(part);
      if (n == 0) break;
      sent_ += n;
      last_progress_ms_ = now;
    }
    if (sent_ == total_) state_ = IppExchangeState::Receiving;  // the printer may answer before it has everything, so we read from the first byte on
  }
  // Answers can arrive while a long document is still going out (an error that ends the request early), so read in both states.
  uint8_t buffer[1024];
  for (uint32_t n; (n = socket_->receive(ByteView(buffer, sizeof buffer))) > 0;) {
    last_progress_ms_ = now;
    if (raw_.size() + n > kMaxResponse) return fail();
    raw_.append(ConstByteView(buffer, n));
  }
  if (raw_.size() > 0 && parse_response()) {
    socket_->close();
    socket_ = SharedPointer<ITcpSocket>();
    state_ = IppExchangeState::Done;
    return;
  }
  if (socket_->peer_closed() && state_ == IppExchangeState::Receiving && socket_->available() == 0 && !parse_response()) return fail();
  if (now - last_progress_ms_ > kStallTimeoutMs) fail();
}

bool IppExchange::parse_response() {
  const uint8_t* p = raw_.view().data();
  const uint64_t size = raw_.size();
  // Skip "100 Continue" interim responses, then find the end of the header.
  uint64_t start = 0;
  for (int rounds = 0; rounds < 4; ++rounds) {
    uint64_t head_end = 0;
    for (uint64_t i = start; i + 3 < size; ++i)
      if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') { head_end = i + 4; break; }
    if (!head_end) return false;
    // "HTTP/1.x NNN"
    if (head_end - start < 12 || p[start] != 'H' || p[start + 1] != 'T' || p[start + 2] != 'T' || p[start + 3] != 'P') {
      http_status_ = 0;
      return false;
    }
    uint32_t status = 0;
    for (uint64_t i = start + 9; i < start + 12 && p[i] >= '0' && p[i] <= '9'; ++i) status = status * 10 + (p[i] - '0');
    if (status == 100) { start = head_end; continue; }
    http_status_ = status;
    uint64_t content_length = 0;
    bool have_length = false, chunked = false;
    for (uint64_t line = start; line < head_end;) {
      uint64_t e = line;
      while (e + 1 < head_end && !(p[e] == '\r' && p[e + 1] == '\n')) ++e;
      const uint32_t len = static_cast<uint32_t>(e - line);
      if (const uint32_t v = header_value(p + line, len, "content-length:")) {
        content_length = 0;
        for (uint32_t i = v; i < len && p[line + i] >= '0' && p[line + i] <= '9'; ++i) content_length = content_length * 10 + (p[line + i] - '0');
        have_length = true;
      } else if (const uint32_t t = header_value(p + line, len, "transfer-encoding:")) {
        chunked = len - t >= 7 && lower(static_cast<char>(p[line + t])) == 'c' && lower(static_cast<char>(p[line + t + 1])) == 'h';
      }
      line = e + 2;
    }
    if (chunked) {
      body_.clear();
      uint64_t pos = head_end;
      while (true) {
        uint64_t chunk = 0;
        uint64_t i = pos;
        bool digits = false;
        for (; i < size && ((p[i] >= '0' && p[i] <= '9') || (lower(static_cast<char>(p[i])) >= 'a' && lower(static_cast<char>(p[i])) <= 'f')); ++i) {
          const char c = lower(static_cast<char>(p[i]));
          chunk = chunk * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
          digits = true;
          if (chunk > kMaxResponse) return false;
        }
        if (!digits) return false;
        while (i + 1 < size && !(p[i] == '\r' && p[i + 1] == '\n')) ++i;   // skip chunk extensions
        if (i + 1 >= size) return false;
        i += 2;
        if (chunk == 0) return true;  // the last chunk (trailers are ignored)
        if (i + chunk + 2 > size) return false;
        if (!body_.append(ConstByteView(p + i, chunk))) return false;
        pos = i + chunk + 2;
      }
    }
    if (have_length) {
      if (size < head_end + content_length) return false;
      body_.clear();
      return body_.append(ConstByteView(p + head_end, content_length));
    }
    // Neither: the body runs to the end of the connection.
    if (!socket_ || !socket_->peer_closed() || socket_->available() != 0) return false;
    body_.clear();
    return body_.append(ConstByteView(p + head_end, size - head_end));
  }
  return false;
}

}  // namespace network
