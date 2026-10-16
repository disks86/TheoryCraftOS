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
#include "Network/NetworkTypes.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

enum class TcpState : uint8_t {
  Closed,
  Listen,
  SynSent,
  SynReceived,
  Established,
  FinWait1,
  FinWait2,
  CloseWait,
  Closing,
  LastAck,
  TimeWait,
};

const char* tcp_state_name(TcpState state);

// One end of a TCP connection: a reliable, ordered byte stream. Polled like the
// rest of the stack - connect() and close() only start the process, and data
// moves while the stack's poll() is being called. Typical client:
//
//   auto socket = stack.tcp_open();
//   socket->connect(server, 80);
//   while (socket->state() == TcpState::SynSent) stack.poll();
//   socket->send(request);
//   ... stack.poll(); socket->receive(buffer) ...
//   socket->close();
//
// and server: listen(port), then accept() until it returns a socket.
//
// Sockets are shared pointers: the stack keeps each connection alive for as long
// as the protocol needs (closing handshakes, retransmissions) even after you let
// go of your reference. A connection you drop while it is still open is closed.
class ITcpSocket {
 public:
  virtual TcpState state() const = 0;

  // Begins connecting to a server (the handshake completes later: watch state()).
  virtual bool connect(Ipv4Address address, uint16_t port) = 0;
  // Starts accepting connections on `port` (up to 8 waiting to be accept()ed).
  virtual bool listen(uint16_t port) = 0;
  // A connection that has completed its handshake, or null if there is none yet.
  virtual SharedPointer<ITcpSocket> accept() = 0;

  // Queues bytes for sending; returns how many fitted into the send buffer
  // (16 KiB), possibly fewer than asked for or 0. Resend the rest later.
  virtual uint32_t send(ConstByteView data) = 0;
  // Copies up to `capacity` received bytes out; returns how many (0 if none yet).
  virtual uint32_t receive(ByteView buffer) = 0;
  // Bytes received and waiting.
  virtual uint32_t available() const = 0;
  // Bytes accepted by send() that the peer has not acknowledged yet.
  virtual uint32_t unsent_or_unacknowledged() const = 0;

  // True once data can flow (Established, or the peer has closed its side and we
  // may still send).
  virtual bool is_connected() const = 0;
  // True once the peer has closed its side and everything it sent was received.
  virtual bool peer_closed() const = 0;
  // True if the connection died abnormally (reset, timed out, refused).
  virtual bool failed() const = 0;

  // Finishes sending what is queued, then closes our side (a FIN). Receiving
  // continues until the peer closes too.
  virtual void close() = 0;
  // Drops the connection at once with a reset.
  virtual void abort() = 0;

  virtual uint16_t local_port() const = 0;
  virtual Ipv4Address remote_address() const = 0;
  virtual uint16_t remote_port() const = 0;

 protected:
  ITcpSocket() = default;
  ~ITcpSocket() = default;
};

}  // namespace network
