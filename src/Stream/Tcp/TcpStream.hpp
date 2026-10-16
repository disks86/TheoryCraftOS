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
#include "Stream/IAsyncStream.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace stream {

// A stream over a TCP connection, like C#'s NetworkStream: read and write only
// (no seeking), with timeouts, usable two ways.
//
// Blocking (read/write/flush): waits by calling the network stack's poll() until
// it can make progress - not for a thread that is the only one polling the stack.
//
// Async (read_async/write_async/flush_async, connect_async): suspends only the
// calling task; the executor runs other tasks meanwhile. Each wait polls the stack
// itself (cheap and idempotent), so nothing else needs to drive it.
//
//   UniquePointer<IAsyncStream> conn;
//   if (co_await TcpStream::connect_async(stack, allocator, "example.com", 80, 5000, &conn) == StreamStatus::Ok) {
//     co_await write_text_async(*conn, "GET / HTTP/1.0\r\n\r\n");
//     co_await conn->flush_async();
//     for (;;) { IoResult r = co_await conn->read_async(buffer); if (r.status != Ok || !r.count) break; ... }
//   }
//
// read returns Ok with 0 bytes when the peer has closed its side and all its data
// was read, ConnectionLost if the connection was reset or timed out, and TimedOut
// when nothing arrived within the read timeout (default 30 s). write returns once
// everything is queued for sending; flush waits until the peer acknowledged it all.
// close() ends our side gracefully (the stack finishes the closing handshake);
// destroying the stream does the same without waiting.
class TcpStream final : public AsyncStreamBase {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 30000;

  // Connects to `address`:`port`, waiting up to `timeout_ms` for the handshake.
  static StreamStatus connect(network::INetworkStack& stack, allocators::IAllocator& allocator, network::Ipv4Address address,
                              uint16_t port, uint32_t timeout_ms, UniquePointer<IAsyncStream>* result);
  // Resolves `host` (a name or dotted quad) first; the timeout covers each step.
  static StreamStatus connect(network::INetworkStack& stack, allocators::IAllocator& allocator, StringView host, uint16_t port,
                              uint32_t timeout_ms, UniquePointer<IAsyncStream>* result);
  // The same without blocking the thread. `result` must stay valid until the task completes.
  static async::Task<StreamStatus> connect_async(network::INetworkStack& stack, allocators::IAllocator& allocator,
                                                 network::Ipv4Address address, uint16_t port, uint32_t timeout_ms,
                                                 UniquePointer<IAsyncStream>* result, async::CancellationToken token = {});
  static async::Task<StreamStatus> connect_async(network::INetworkStack& stack, allocators::IAllocator& allocator, StringView host,
                                                 uint16_t port, uint32_t timeout_ms, UniquePointer<IAsyncStream>* result,
                                                 async::CancellationToken token = {});

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

 protected:
  async::Task<IoResult> on_read_async(ByteView buffer, async::CancellationToken token) override;
  async::Task<StreamStatus> on_write_async(ConstByteView data, async::CancellationToken token) override;
  async::Task<StreamStatus> on_flush_async(async::CancellationToken token) override;

 private:
  // One polling step; true once `deadline_ms` (an absolute time, or "never") has passed.
  bool wait_step(uint64_t deadline_ms);
  uint64_t deadline_after(uint32_t timeout_ms) const;

  network::INetworkStack& stack_;
  SharedPointer<network::ITcpSocket> socket_;
  uint32_t read_timeout_ms_ = kDefaultTimeoutMs;
  uint32_t write_timeout_ms_ = kDefaultTimeoutMs;
};

// Accepts incoming connections on a port as streams.
//
//   TcpListener listener(stack, allocator);
//   listener.listen(8080);
//   UniquePointer<IAsyncStream> client;
//   while (co_await listener.accept_async(&client) == StreamStatus::Ok) executor.spawn(serve(move(client)));
class TcpListener {
 public:
  TcpListener(network::INetworkStack& stack, allocators::IAllocator& allocator) : stack_(stack), allocator_(allocator) {}
  ~TcpListener() { stop(); }
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Starts listening. False if the port is taken or memory ran out.
  bool listen(uint16_t port);
  void stop();
  uint16_t port() const { return socket_ ? socket_->local_port() : 0; }

  // Waits for a connection (up to `timeout_ms`; TimedOut) and wraps it in a stream.
  StreamStatus accept(uint32_t timeout_ms, UniquePointer<IAsyncStream>* result);
  async::Task<StreamStatus> accept_async(UniquePointer<IAsyncStream>* result, uint32_t timeout_ms = kInfiniteTimeout,
                                         async::CancellationToken token = {});

 private:
  StreamStatus wrap(SharedPointer<network::ITcpSocket> accepted, UniquePointer<IAsyncStream>* result);

  network::INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  SharedPointer<network::ITcpSocket> socket_;
};

}  // namespace stream
