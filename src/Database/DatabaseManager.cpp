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

#include "Database/DatabaseManager.hpp"

namespace database {

bool DatabaseManager::add_driver(IDatabaseDriver* driver) {
  if (!driver || find_driver(driver->name())) return false;
  return drivers_.push_back(driver);
}

IDatabaseDriver* DatabaseManager::find_driver(StringView scheme) const {
  for (uint64_t i = 0; i < drivers_.count(); ++i)
    if (drivers_[i]->handles_scheme(scheme) || scheme.equals(drivers_[i]->name())) return drivers_[i];
  return nullptr;
}

DbStatus DatabaseManager::connect(StringView url, UniquePointer<IConnection>* connection, DbError* error) {
  ConnectionOptions options(allocator_);
  DbError local(allocator_);
  DbError* e = error ? error : &local;
  if (!parse_database_url(url, &options, e)) return DbStatus::InvalidArgument;
  return connect(options, connection, e);
}

DbStatus DatabaseManager::connect(const ConnectionOptions& options, UniquePointer<IConnection>* connection, DbError* error) {
  DbError local(allocator_);
  DbError* e = error ? error : &local;
  e->clear();
  IDatabaseDriver* driver = find_driver(options.driver.view());
  if (!driver) {
    e->set(DbStatus::UnknownDriver, "no driver for this kind of database");
    e->detail.assign(options.driver.view());
    return DbStatus::UnknownDriver;
  }
  if (!transport_) {
    e->set(DbStatus::ConnectFailed, "no network is available to connect through");
    return DbStatus::ConnectFailed;
  }
  connection->reset();
  return driver->connect(options, *transport_, connection, e);
}

}  // namespace database
