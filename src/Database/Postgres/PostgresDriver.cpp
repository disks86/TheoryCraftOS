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

#include "Database/Postgres/PostgresDriver.hpp"
#include "Database/Postgres/PostgresConnection.hpp"

namespace database::postgres {

DbStatus PostgresDriver::connect(const ConnectionOptions& options, IDatabaseTransport& transport, UniquePointer<IConnection>* connection, DbError* error) {
  DbError local(allocator_);
  DbError* e = error ? error : &local;
  if (options.host.empty()) {
    e->set(DbStatus::InvalidArgument, "no host to connect to");
    return DbStatus::InvalidArgument;
  }
  if (options.user.empty()) {
    e->set(DbStatus::InvalidArgument, "PostgreSQL needs a user name");
    return DbStatus::InvalidArgument;
  }
  return PostgresConnection::connect(allocator_, options, transport, random_, connection, e);
}

}  // namespace database::postgres
