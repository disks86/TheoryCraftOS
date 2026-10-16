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
#include "Allocators/IAllocator.hpp"
#include "Network/INetworkStack.hpp"
#include "Stream/StreamBase.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace stream {

// A stream over a TCP connection, like C#'s NetworkStream: read and write only
// (no seeking), blocking, with timeouts. The network stack is polled, so a read or
// write waits by calling the stack's poll() until it can make progress - do not
// use one from the thread that is the only one polling the stack.
//
//   UniquePointer<IStream> conn;
//   if (TcpStream::connect(stack, allocator, "example.com", 80, 5000, &conn) == StreamStatus::Ok) {
//     write_text(*conn, "GET / HTTP/1.0\r\n\r\n");
//     conn->flush();
//     ... conn->read(buffer, &got) ... // got == 0 once the server has closed
//   }
//
// read() returns Ok with 0 bytes when the peer has closed its side and all its
// data was read, ConnectionLost if the connection was reset or timed out, and
// TimedOut when nothing arrived within the read timeout (default 30 s). write()
// returns once everything is queued for sending; flush() waits until the peer
// acknowledged it all. close() ends our side gracefully (the stack finishes the
// closing handshake); destroying the stream does the same without waiting.
class TcpStream final : public StreamBase {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 30000;

  // Connects to `address`:`port`, waiting up to `timeout_ms` for the handshake.
  static StreamStatus connect(network::INetworkStack& stack, allocators::IAllocator& allocator, network::Ipv4Address address,
                              uint16_t port, uint32_t timeout_ms, UniquePointer<IStream>* result);
  // Resolves `host` (a name or dotted quad) first; the timeout covers each step.
  static StreamStatus connect(network::INetworkStack& stack, allocators::IAllocator& allocator, StringView host, uint16_t port,
                              uint32_t timeout_ms, UniquePointer<IStream>* result);

  // Over an established socket, for example one returned by ITcpSocket::accept().
  TcpStream(network::INetworkStack& stack, SharedPointer<network::ITcpSocket> socket)
      : stack_(stack), socket_(static_cast<SharedPointer<network::ITcpSocket>&&>(socket)) {}
  ~TcpStream() { close(); }
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;

  bool can_read() const override { return !closed_; }
  bool can_write() const override { return !closed_; }
  bool can_timeout() const override { return true; }

  StreamStatus read(ByteView buffer, uint64_t* count) override;
  StreamStatus write(ConstByteView data) override;
  StreamStatus flush() override;
  StreamStatus close() override;
  StreamStatus set_read_timeout(uint32_t milliseconds) override;
  StreamStatus set_write_timeout(uint32_t milliseconds) override;

  // Drops the connection with a reset instead of closing it gracefully.
  void abort();
  network::Ipv4Address remote_address() const { return socket_ ? socket_->remote_address() : network::Ipv4Address{}; }
  uint16_t remote_port() const { return socket_ ? socket_->remote_port() : 0; }

 private:
  // One polling step; true once `deadline_ms` (an absolute time, or "never") has passed.
  bool wait_step(uint64_t deadline_ms);
  uint64_t deadline_after(uint32_t timeout_ms) const;

  network::INetworkStack& stack_;
  SharedPointer<network::ITcpSocket> socket_;
  uint32_t read_timeout_ms_ = kDefaultTimeoutMs;
  uint32_t write_timeout_ms_ = kDefaultTimeoutMs;
};

}  // namespace stream
