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

#include "Database/Net/NetworkTransport.hpp"
#include "Kernel/Thread.hpp"
#include "Stream/Tcp/TcpStream.hpp"

namespace database {

using network::INetworkStack;
using network::ITcpSocket;
using network::TcpState;

DbStatus NetworkTransport::fail(DbError* error, DbStatus status, const char* text) {
  if (error) error->set(status, text);
  return status;
}

DbStatus NetworkTransport::open(const Endpoint& endpoint, const TlsProbe* probe, UniquePointer<stream::IStream>* stream, bool* encrypted, DbError* error) {
  *encrypted = false;
  SharedPointer<ITcpSocket> socket;
  INetworkStack* stack = nullptr;
  if (network_.connect(endpoint.host, endpoint.port, endpoint.timeout_ms, &socket, &stack) != filesystem::Status::Ok || !socket || !stack)
    return fail(error, DbStatus::ConnectFailed, "the server could not be found or reached");
  const uint64_t deadline = stack->now_ms() + endpoint.timeout_ms;

  SharedPointer<ITcpSocket> line = socket;   // what the stream will use: the socket itself, or TLS on top of it
  const bool try_tls = probe && endpoint.tls != TlsMode::Disable && (has_tls_ || endpoint.tls == TlsMode::Require);
  if (try_tls) {
    if (!has_tls_) {
      socket->abort();
      return fail(error, DbStatus::TlsFailed, "TLS is required and this system has no TLS configuration");
    }
    // Ask in the clear, and read the one byte that answers.
    ConstByteView pending = probe->request;
    while (!pending.empty()) {
      const uint32_t sent = socket->send(pending);
      pending = pending.subview(sent);
      if (!pending.empty()) {
        if (socket->failed() || stack->now_ms() >= deadline) return fail(error, DbStatus::ConnectionLost, "the connection broke while asking for TLS");
        stack->poll();
        kernel::sleep_us(200);
      }
    }
    uint8_t answer = 0;
    for (;;) {
      if (socket->receive(ByteView(&answer, 1)) == 1) break;
      if (socket->failed() || socket->peer_closed()) return fail(error, DbStatus::ConnectionLost, "the server closed the connection");
      if (stack->now_ms() >= deadline) return fail(error, DbStatus::Timeout, "the server did not answer in time");
      stack->poll();
      kernel::sleep_us(200);
    }
    if (answer == probe->accept) {
      network::TlsConfig config = tls_;
      String name(allocator_);
      name.assign(endpoint.host);
      config.server_name = name.c_str();
      config.handshake_timeout_ms = endpoint.timeout_ms;
      line = SharedPointer<ITcpSocket>::make_as<network::TlsSocket>(allocator_, *stack, allocator_, config, socket);
      if (!line) return fail(error, DbStatus::OutOfMemory, "out of memory");
      while (!line->is_connected()) {   // the handshake
        if (line->failed() || line->state() == TcpState::Closed) return fail(error, DbStatus::TlsFailed, "the TLS handshake with the server failed");
        if (stack->now_ms() >= deadline) return fail(error, DbStatus::TlsFailed, "the TLS handshake with the server took too long");
        stack->poll();
        kernel::sleep_us(200);
      }
      *encrypted = true;
    } else if (endpoint.tls == TlsMode::Require) {
      socket->abort();
      return fail(error, DbStatus::TlsFailed, "the server does not do TLS and it is required");
    }
  }
  *stream = UniquePointer<stream::IStream>::make_as<stream::TcpStream>(allocator_, *stack, line);
  if (!*stream) return fail(error, DbStatus::OutOfMemory, "out of memory");
  return DbStatus::Ok;
}

}  // namespace database
