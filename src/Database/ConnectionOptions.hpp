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

namespace database {

// Where a connection goes and how. Made by hand or from a URL:
//
//   scheme://[user[:password]@]host[:port][/database][?option=value&...]
//
// User, password, database and option values may use %XX escapes. The options understood: sslmode (disable, allow/prefer, require, verify-ca and
// verify-full count as require), connect_timeout (seconds), application_name, max_result_bytes; others are ignored. `postgresql` is another name
// for the `postgres` scheme.
struct ConnectionOptions {
  explicit ConnectionOptions(allocators::IAllocator& allocator) : allocator_(&allocator), driver(allocator), host(allocator), user(allocator), password(allocator), database(allocator), application_name(allocator) {}
  ConnectionOptions(ConnectionOptions&&) = default;
  ConnectionOptions& operator=(ConnectionOptions&&) = default;

  allocators::IAllocator& allocator() const { return *allocator_; }

  static constexpr uint32_t kDefaultConnectTimeoutMs = 10000;
  static constexpr uint32_t kDefaultQueryTimeoutMs = 60000;
  static constexpr uint64_t kDefaultMaxResultBytes = 16u << 20;

  allocators::IAllocator* allocator_;
  String driver;                 // the scheme: "postgres"
  String host;
  uint16_t port = 0;             // 0: the driver's usual port
  String user, password, database;
  String application_name;
  TlsMode tls = TlsMode::Prefer;
  uint32_t connect_timeout_ms = kDefaultConnectTimeoutMs;
  uint32_t query_timeout_ms = kDefaultQueryTimeoutMs;   // how long to wait for the server's answer to a statement
  uint64_t max_result_bytes = kDefaultMaxResultBytes;
};

// Takes a URL apart. False (and `error` says what is wrong, if given) for a missing scheme or host, a port outside 1..65535, a bad %XX escape or an
// option with a bad value.
bool parse_database_url(StringView url, ConnectionOptions* out, DbError* error = nullptr);

}  // namespace database
