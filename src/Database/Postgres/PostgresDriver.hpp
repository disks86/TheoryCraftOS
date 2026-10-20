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
#include "Crypto/Random.hpp"
#include "Database/IDatabaseDriver.hpp"

namespace database::postgres {

// The driver for PostgreSQL servers: URLs of the form postgres://user:password@host[:5432]/database[?sslmode=require]. `random` (may be nullptr)
// is where the client nonce of SCRAM-SHA-256 logins comes from; without it only servers that ask for a clear text or MD5 password can be logged in to.
class PostgresDriver final : public IDatabaseDriver {
 public:
  static constexpr uint16_t kDefaultPort = 5432;

  explicit PostgresDriver(allocators::IAllocator& allocator, crypto::IRandom* random = nullptr) : allocator_(allocator), random_(random) {}
  void set_random(crypto::IRandom* random) { random_ = random; }

  const char* name() const override { return "postgres"; }
  bool handles_scheme(StringView scheme) const override { return scheme.equals("postgres") || scheme.equals("postgresql"); }
  uint16_t default_port() const override { return kDefaultPort; }
  DbStatus connect(const ConnectionOptions& options, IDatabaseTransport& transport, UniquePointer<IConnection>* connection, DbError* error) override;

 private:
  allocators::IAllocator& allocator_;
  crypto::IRandom* random_;
};

}  // namespace database::postgres
