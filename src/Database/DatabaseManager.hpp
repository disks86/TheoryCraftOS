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
#include "Database/IDatabaseDriver.hpp"
#include "ObjectContainer/List.hpp"

namespace database {

// The place programs go to for a database connection: it knows the drivers and the transport, and picks the driver by the scheme of the URL.
//
//   database::DatabaseManager databases(allocator, &transport);
//   databases.add_driver(&postgres);
//   UniquePointer<database::IConnection> db;
//   database::DbError error(allocator);
//   if (databases.connect("postgres://user:pw@host/dbname", &db, &error) != database::DbStatus::Ok) report(error.message.view());
class DatabaseManager {
 public:
  explicit DatabaseManager(allocators::IAllocator& allocator, IDatabaseTransport* transport = nullptr) : allocator_(allocator), drivers_(allocator), transport_(transport) {}

  // Makes a driver available (not owned: it must outlive the manager). False if one with the same name is there or memory ran out.
  bool add_driver(IDatabaseDriver* driver);
  void set_transport(IDatabaseTransport* transport) { transport_ = transport; }
  uint32_t driver_count() const { return static_cast<uint32_t>(drivers_.count()); }
  IDatabaseDriver* driver(uint32_t index) const { return index < drivers_.count() ? drivers_[index] : nullptr; }
  // The driver whose name or scheme is `scheme`, or nullptr.
  IDatabaseDriver* find_driver(StringView scheme) const;

  DbStatus connect(StringView url, UniquePointer<IConnection>* connection, DbError* error = nullptr);
  DbStatus connect(const ConnectionOptions& options, UniquePointer<IConnection>* connection, DbError* error = nullptr);

 private:
  allocators::IAllocator& allocator_;
  List<IDatabaseDriver*> drivers_;
  IDatabaseTransport* transport_;
};

}  // namespace database
