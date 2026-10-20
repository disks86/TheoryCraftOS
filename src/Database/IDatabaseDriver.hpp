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
#include "Database/ConnectionOptions.hpp"
#include "Database/IConnection.hpp"
#include "Database/IDatabaseTransport.hpp"

namespace database {

// What it takes to talk to one kind of database. A driver is registered with the DatabaseManager and chosen by the scheme of the URL.
class IDatabaseDriver {
 public:
  // "postgres"
  virtual const char* name() const = 0;
  // Does a URL scheme ("postgres", "postgresql") belong to this driver?
  virtual bool handles_scheme(StringView scheme) const = 0;
  // The port used when the options have none.
  virtual uint16_t default_port() const = 0;
  // False for a driver that does not use the network (the database lives in the machine): it can connect when the manager has no transport.
  virtual bool needs_transport() const { return true; }
  // Connects, logs in and returns the open connection. The transport must stay valid for as long as the connection is used.
  virtual DbStatus connect(const ConnectionOptions& options, IDatabaseTransport& transport, UniquePointer<IConnection>* connection, DbError* error) = 0;

 protected:
  constexpr IDatabaseDriver() = default;
  ~IDatabaseDriver() = default;
};

}  // namespace database
