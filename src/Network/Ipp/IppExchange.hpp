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
#include "Network/INetworkStack.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

enum class IppExchangeState : uint8_t { Idle, Connecting, Sending, Receiving, Done, Failed };

// One IPP request and its answer over HTTP/1.1 (POST of application/ipp; the answer may come with a Content-Length, chunked, or
// simply end with the connection). Polled like the sockets underneath: call poll() while the stack is polled. One exchange at a time;
// start() again after Done or Failed. The connection is closed after each exchange.
class IppExchange {
 public:
  static constexpr uint16_t kPort = 631;
  static constexpr uint32_t kMaxResponse = 128 * 1024;
  IppExchange(INetworkStack& stack, allocators::IAllocator& allocator) : stack_(stack), raw_(allocator), head_(allocator), body_(allocator) {}
  ~IppExchange() { cancel(); }
  IppExchange(const IppExchange&) = delete;
  IppExchange& operator=(const IppExchange&) = delete;

  // Sends `ipp_head` (the encoded request) followed by `document` (may be empty; must stay valid until Done or Failed) to
  // http://address:port/path. `host` is the Host header value. False if a socket could not be opened.
  bool start(Ipv4Address address, uint16_t port, const char* path, ConstByteView ipp_head, ConstByteView document);
  void poll();
  void cancel();
  IppExchangeState state() const { return state_; }
  bool busy() const { return state_ == IppExchangeState::Connecting || state_ == IppExchangeState::Sending || state_ == IppExchangeState::Receiving; }
  // The HTTP status (200 when all went well); 0 while unknown.
  uint32_t http_status() const { return http_status_; }
  // The answer's body once Done.
  ConstByteView body() const { return body_.view(); }
  uint64_t bytes_sent() const { return sent_; }

 private:
  bool parse_response();  // true once the answer is complete (and body_ filled)
  void fail();
  INetworkStack& stack_;
  SharedPointer<ITcpSocket> socket_;
  ByteBuffer raw_, head_, body_;
  ConstByteView document_;
  uint64_t sent_ = 0, total_ = 0;
  uint64_t deadline_ms_ = 0, last_progress_ms_ = 0;
  uint32_t http_status_ = 0;
  IppExchangeState state_ = IppExchangeState::Idle;
};

}  // namespace network
