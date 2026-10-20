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
#include "Database/IDatabaseTransport.hpp"
#include "Network/ServerNetwork.hpp"
#include "Network/Tls/TlsSocket.hpp"

namespace database {

// Database connections over the OS's own network stacks: `network` finds the host (by name or address, on whichever interface can reach it) and
// opens the TCP connection, which becomes a TLS socket when encryption is started, and the stream is a TcpStream, which polls the stack while it
// waits. `tls` (may be nullptr) is the TLS configuration for encrypted connections - the trust store, random source and clock; without one a
// connection can only be plain, and TlsMode::Require fails.
class NetworkTransport final : public IDatabaseTransport {
 public:
  NetworkTransport(network::IServerNetwork& network, allocators::IAllocator& allocator, const network::TlsConfig* tls = nullptr)
      : network_(network), allocator_(allocator), has_tls_(tls != nullptr) {
    if (tls) tls_ = *tls;
  }
  void set_tls(const network::TlsConfig* tls) {
    has_tls_ = tls != nullptr;
    if (tls) tls_ = *tls;
  }

  DbStatus open(const Endpoint& endpoint, const TlsProbe* probe, UniquePointer<stream::IStream>* stream, bool* encrypted, DbError* error) override;

 private:
  static DbStatus fail(DbError* error, DbStatus status, const char* text);

  network::IServerNetwork& network_;
  allocators::IAllocator& allocator_;
  network::TlsConfig tls_;
  bool has_tls_;
};

}  // namespace database
