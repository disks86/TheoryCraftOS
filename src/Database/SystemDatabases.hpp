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
#include "Database/DatabaseManager.hpp"
#include "Database/Net/NetworkTransport.hpp"
#include "Database/Postgres/PostgresDriver.hpp"

namespace database {

// The database access this system offers: the drivers it has (PostgreSQL) behind one DatabaseManager, reaching servers over the machine's network
// interfaces. Programs are given the manager and ask it for connections by URL; they never see which driver answers.
//
//   database::SystemDatabases databases(allocator, network, &random);
//   databases.manager().connect("postgres://user:password@db.example.com/app", &connection, &error);
//
// `random` is where SCRAM logins get their nonce (it must be initialized); `tls` (may be nullptr) lets connections be encrypted - the trust store,
// random source and clock for the handshake. Everything given must outlive this object.
class SystemDatabases {
 public:
  SystemDatabases(allocators::IAllocator& allocator, network::IServerNetwork& network, crypto::IRandom* random, const network::TlsConfig* tls = nullptr)
      : transport_(network, allocator, tls), postgres_(allocator, random), manager_(allocator, &transport_) {
    manager_.add_driver(&postgres_);
  }
  SystemDatabases(const SystemDatabases&) = delete;
  SystemDatabases& operator=(const SystemDatabases&) = delete;

  DatabaseManager& manager() { return manager_; }
  NetworkTransport& transport() { return transport_; }

 private:
  NetworkTransport transport_;
  postgres::PostgresDriver postgres_;
  DatabaseManager manager_;
};

}  // namespace database
