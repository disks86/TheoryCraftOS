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
#include "Database/DatabaseTypes.hpp"
#include "Stream/IStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace database {

// Where to connect.
struct Endpoint {
  StringView host;
  uint16_t port = 0;
  uint32_t timeout_ms = 10000;   // for connecting and for the TLS handshake
  TlsMode tls = TlsMode::Prefer;
};

// How a protocol asks for TLS before anything else is said: the bytes it sends in the clear, and the one byte of the answer that means "yes,
// start TLS now" (PostgreSQL: the SSLRequest and 'S'). Any other answer byte means no TLS on this connection.
struct TlsProbe {
  ConstByteView request;
  uint8_t accept = 0;
};

// How a driver gets a byte stream to the server, so that it need not know about the network stack (and so that tests can supply a stream of
// their own). The stream is blocking and has read and write timeouts (IStream::set_read_timeout).
class IDatabaseTransport {
 public:
  // Opens the stream. With `endpoint.tls` other than Disable and a `probe`, the probe is sent first and, when the server accepts, TLS is
  // started on the same connection: `*encrypted` says whether it was. Require with a server that says no is TlsFailed. `error` (may be
  // nullptr) gets the details of a failure.
  virtual DbStatus open(const Endpoint& endpoint, const TlsProbe* probe, UniquePointer<stream::IStream>* stream, bool* encrypted, DbError* error) = 0;

 protected:
  constexpr IDatabaseTransport() = default;
  ~IDatabaseTransport() = default;
};

}  // namespace database
