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
                                uint16_t port, uint32_t timeout_ms, UniquePointer<IStream>* result) {
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
  UniquePointer<IStream> stream = UniquePointer<IStream>::make_as<TcpStream>(allocator, stack, util::move(socket));
  if (!stream) return StreamStatus::OutOfMemory;
  *result = util::move(stream);
  return StreamStatus::Ok;
}

StreamStatus TcpStream::connect(network::INetworkStack& stack, allocators::IAllocator& allocator, StringView host, uint16_t port,
                                uint32_t timeout_ms, UniquePointer<IStream>* result) {
  network::Ipv4Address address{};
  if (!stack.resolve(host, timeout_ms == kInfiniteTimeout ? 0xFFFFFFFEu : timeout_ms, &address)) return StreamStatus::HostNotFound;
  return connect(stack, allocator, address, port, timeout_ms, result);
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
