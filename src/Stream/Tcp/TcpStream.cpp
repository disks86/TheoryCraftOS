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

#include "Stream/Tcp/TcpStream.hpp"
#include "Async/Executor.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/Move.hpp"

namespace stream {

using network::TcpState;

uint64_t TcpStream::deadline_after(uint32_t timeout_ms) const {
  return timeout_ms == kInfiniteTimeout ? ~uint64_t{0} : stack_.now_ms() + timeout_ms;
}

bool TcpStream::wait_step(uint64_t deadline_ms) {
  if (deadline_ms != ~uint64_t{0} && stack_.now_ms() >= deadline_ms) return true;
  stack_.poll();
  kernel::sleep_us(200);
  return false;
}

StreamStatus TcpStream::connect(network::INetworkStack& stack, allocators::IAllocator& allocator, network::Ipv4Address address,
                                uint16_t port, uint32_t timeout_ms, UniquePointer<IAsyncStream>* result) {
  SharedPointer<network::ITcpSocket> socket = stack.tcp_open();
  if (!socket) return StreamStatus::OutOfMemory;
  if (!socket->connect(address, port)) return StreamStatus::IoError;
  const uint64_t deadline = timeout_ms == kInfiniteTimeout ? ~uint64_t{0} : stack.now_ms() + timeout_ms;
  while (!socket->is_connected()) {
    if (socket->failed() || socket->state() == TcpState::Closed) return StreamStatus::ConnectionLost;
    if (deadline != ~uint64_t{0} && stack.now_ms() >= deadline) {
      socket->abort();
      return StreamStatus::TimedOut;
    }
    stack.poll();
    kernel::sleep_us(200);
  }
  UniquePointer<IAsyncStream> stream = UniquePointer<IAsyncStream>::make_as<TcpStream>(allocator, stack, util::move(socket));
  if (!stream) return StreamStatus::OutOfMemory;
  *result = util::move(stream);
  return StreamStatus::Ok;
}

StreamStatus TcpStream::connect(network::INetworkStack& stack, allocators::IAllocator& allocator, StringView host, uint16_t port,
                                uint32_t timeout_ms, UniquePointer<IAsyncStream>* result) {
  network::Ipv4Address address{};
  if (!stack.resolve(host, timeout_ms == kInfiniteTimeout ? 0xFFFFFFFEu : timeout_ms, &address)) return StreamStatus::HostNotFound;
  return connect(stack, allocator, address, port, timeout_ms, result);
}

// Shared by the sync and async connect: the state of a connecting socket.
static bool connect_settled(const SharedPointer<network::ITcpSocket>& socket) {
  return socket->is_connected() || socket->failed() || socket->state() == TcpState::Closed;
}

async::Task<StreamStatus> TcpStream::connect_async(network::INetworkStack& stack, allocators::IAllocator& allocator,
                                                   network::Ipv4Address address, uint16_t port, uint32_t timeout_ms,
                                                   UniquePointer<IAsyncStream>* result, async::CancellationToken token) {
  SharedPointer<network::ITcpSocket> socket = stack.tcp_open();
  if (!socket) co_return StreamStatus::OutOfMemory;
  if (!socket->connect(address, port)) co_return StreamStatus::IoError;
  const bool settled = co_await async::wait_until(
      [&stack, &socket, &token] {
        stack.poll();
        return connect_settled(socket) || token.cancelled();
      },
      timeout_ms);
  if (!settled) {
    socket->abort();
    co_return StreamStatus::TimedOut;
  }
  if (!socket->is_connected()) {
    if (token.cancelled()) socket->abort();
    co_return token.cancelled() ? StreamStatus::Cancelled : StreamStatus::ConnectionLost;
  }
  UniquePointer<IAsyncStream> stream = UniquePointer<IAsyncStream>::make_as<TcpStream>(allocator, stack, util::move(socket));
  if (!stream) co_return StreamStatus::OutOfMemory;
  *result = util::move(stream);
  co_return StreamStatus::Ok;
}

async::Task<StreamStatus> TcpStream::connect_async(network::INetworkStack& stack, allocators::IAllocator& allocator, StringView host,
                                                   uint16_t port, uint32_t timeout_ms, UniquePointer<IAsyncStream>* result,
                                                   async::CancellationToken token) {
  SharedPointer<network::IDnsQuery> query = stack.dns().query(host);
  if (!query) co_return StreamStatus::HostNotFound;
  const bool answered = co_await async::wait_until(
      [&stack, &query, &token] {
        stack.poll();
        return query->status() != network::DnsStatus::Pending || token.cancelled();
      },
      timeout_ms);
  if (!answered) co_return StreamStatus::TimedOut;
  if (token.cancelled()) co_return StreamStatus::Cancelled;
  if (query->status() != network::DnsStatus::Resolved) co_return StreamStatus::HostNotFound;
  co_return co_await connect_async(stack, allocator, query->address(), port, timeout_ms, result, token);
}

async::Task<IoResult> TcpStream::on_read_async(ByteView buffer, async::CancellationToken token) {
  IoResult result;
  if (buffer.empty()) {
    result.status = check();
    co_return result;
  }
  for (;;) {
    if (closed_) {
      result.status = StreamStatus::Closed;
      co_return result;
    }
    const uint32_t got = socket_->receive(buffer);  // data first, so nothing is lost to a close or reset that follows
    if (got) {
      result.count = got;
      co_return result;
    }
    if (socket_->peer_closed()) co_return result;  // end of stream
    if (socket_->failed() || socket_->state() == TcpState::Closed) {
      result.status = StreamStatus::ConnectionLost;
      co_return result;
    }
    if (token.cancelled()) {
      result.status = StreamStatus::Cancelled;
      co_return result;
    }
    const bool woke = co_await async::wait_until(
        [this, &token] {
          if (closed_) return true;
          stack_.poll();
          return socket_->available() > 0 || socket_->peer_closed() || socket_->failed() || socket_->state() == TcpState::Closed ||
                 token.cancelled();
        },
        read_timeout_ms_);
    if (!woke) {
      result.status = StreamStatus::TimedOut;
      co_return result;
    }
  }
}

async::Task<StreamStatus> TcpStream::on_write_async(ConstByteView data, async::CancellationToken token) {
  while (!data.empty()) {
    if (closed_) co_return StreamStatus::Closed;
    if (socket_->failed() || !socket_->is_connected()) co_return StreamStatus::ConnectionLost;
    if (token.cancelled()) co_return StreamStatus::Cancelled;
    uint32_t sent = 0;
    const bool progressed = co_await async::wait_until(  // the condition itself tries to send: it holds once there was room
        [this, &data, &token, &sent] {
          if (closed_ || token.cancelled()) return true;
          stack_.poll();
          if (socket_->failed() || !socket_->is_connected()) return true;
          sent = socket_->send(data);
          return sent > 0;
        },
        write_timeout_ms_);
    if (!progressed) co_return StreamStatus::TimedOut;
    data = data.subview(sent);
  }
  co_return check();
}

async::Task<StreamStatus> TcpStream::on_flush_async(async::CancellationToken token) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) co_return s;
  const bool drained = co_await async::wait_until(
      [this, &token] {
        if (closed_ || token.cancelled()) return true;
        stack_.poll();
        return socket_->unsent_or_unacknowledged() == 0 || socket_->failed() || socket_->state() == TcpState::Closed;
      },
      write_timeout_ms_);
  if (closed_) co_return StreamStatus::Closed;
  if (!drained) co_return StreamStatus::TimedOut;
  if (token.cancelled()) co_return StreamStatus::Cancelled;
  co_return socket_->failed() ? StreamStatus::ConnectionLost : StreamStatus::Ok;
}

StreamStatus TcpStream::read(ByteView buffer, uint64_t* count) {
  *count = 0;
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (buffer.empty()) return StreamStatus::Ok;
  const uint64_t deadline = deadline_after(read_timeout_ms_);
  for (;;) {
    const uint32_t got = socket_->receive(buffer);  // data first, so nothing is lost to a close or reset that follows
    if (got) {
      *count = got;
      return StreamStatus::Ok;
    }
    if (socket_->peer_closed()) return StreamStatus::Ok;  // end of stream
    if (socket_->failed() || socket_->state() == TcpState::Closed) return StreamStatus::ConnectionLost;
    if (wait_step(deadline)) return StreamStatus::TimedOut;
  }
}

StreamStatus TcpStream::write(ConstByteView data) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  uint64_t deadline = deadline_after(write_timeout_ms_);
  while (!data.empty()) {
    if (socket_->failed() || !socket_->is_connected()) return StreamStatus::ConnectionLost;
    const uint32_t sent = socket_->send(data);
    if (sent) {
      data = data.subview(sent);
      deadline = deadline_after(write_timeout_ms_);  // the timeout is for lack of progress
      continue;
    }
    if (wait_step(deadline)) return StreamStatus::TimedOut;
  }
  return StreamStatus::Ok;
}

StreamStatus TcpStream::flush() {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  uint64_t deadline = deadline_after(write_timeout_ms_);
  uint32_t pending = socket_->unsent_or_unacknowledged();
  while (pending) {
    if (socket_->failed() || socket_->state() == TcpState::Closed) return StreamStatus::ConnectionLost;
    if (wait_step(deadline)) return StreamStatus::TimedOut;
    const uint32_t now_pending = socket_->unsent_or_unacknowledged();
    if (now_pending < pending) deadline = deadline_after(write_timeout_ms_);
    pending = now_pending;
  }
  return socket_->failed() ? StreamStatus::ConnectionLost : StreamStatus::Ok;
}

StreamStatus TcpStream::close() {
  if (closed_) return StreamStatus::Ok;
  closed_ = true;
  if (socket_) {
    socket_->close();  // graceful: the stack keeps the connection until the handshake is over
    socket_.reset();
  }
  return StreamStatus::Ok;
}

void TcpStream::abort() {
  if (closed_) return;
  closed_ = true;
  if (socket_) {
    socket_->abort();
    socket_.reset();
  }
}

StreamStatus TcpStream::set_read_timeout(uint32_t milliseconds) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  read_timeout_ms_ = milliseconds;
  return StreamStatus::Ok;
}

StreamStatus TcpStream::set_write_timeout(uint32_t milliseconds) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  write_timeout_ms_ = milliseconds;
  return StreamStatus::Ok;
}

}  // namespace stream

namespace stream {

bool TcpListener::listen(uint16_t port) {
  stop();
  SharedPointer<network::ITcpSocket> socket = stack_.tcp_open();
  if (!socket || !socket->listen(port)) return false;
  socket_ = util::move(socket);
  return true;
}

void TcpListener::stop() {
  if (socket_) {
    socket_->close();
    socket_.reset();
  }
}

StreamStatus TcpListener::wrap(SharedPointer<network::ITcpSocket> accepted, UniquePointer<IAsyncStream>* result) {
  UniquePointer<IAsyncStream> stream = UniquePointer<IAsyncStream>::make_as<TcpStream>(allocator_, stack_, util::move(accepted));
  if (!stream) return StreamStatus::OutOfMemory;
  *result = util::move(stream);
  return StreamStatus::Ok;
}

StreamStatus TcpListener::accept(uint32_t timeout_ms, UniquePointer<IAsyncStream>* result) {
  if (!socket_) return StreamStatus::Closed;
  const uint64_t deadline = timeout_ms == kInfiniteTimeout ? ~uint64_t{0} : stack_.now_ms() + timeout_ms;
  for (;;) {
    SharedPointer<network::ITcpSocket> accepted = socket_->accept();
    if (accepted) return wrap(util::move(accepted), result);
    if (deadline != ~uint64_t{0} && stack_.now_ms() >= deadline) return StreamStatus::TimedOut;
    stack_.poll();
    kernel::sleep_us(200);
  }
}

async::Task<StreamStatus> TcpListener::accept_async(UniquePointer<IAsyncStream>* result, uint32_t timeout_ms,
                                                    async::CancellationToken token) {
  if (!socket_) co_return StreamStatus::Closed;
  SharedPointer<network::ITcpSocket> accepted;
  const bool got = co_await async::wait_until(
      [this, &accepted, &token] {
        if (!socket_ || token.cancelled()) return true;
        stack_.poll();
        accepted = socket_->accept();
        return static_cast<bool>(accepted);
      },
      timeout_ms);
  if (!got) co_return StreamStatus::TimedOut;
  if (!accepted) co_return token.cancelled() ? StreamStatus::Cancelled : StreamStatus::Closed;
  co_return wrap(util::move(accepted), result);
}

}  // namespace stream
