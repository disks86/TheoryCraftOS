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
#include "Database/IResultSet.hpp"
#include "Utility/UniquePointer.hpp"

namespace database {

// One session with a database server. Calls wait for the server (a connection is used by one thread at a time) and return a DbStatus; on a
// failure last_error() has the details. A connection whose conversation with the server broke (DbStatus::ConnectionLost or Protocol) is closed:
// open another. Destroying the connection (its UniquePointer) closes it.
//
//   UniquePointer<IConnection> db;
//   if (manager.connect("postgres://app:secret@db.example.com/shop", &db, &error) != DbStatus::Ok) return;
//   const DbParam args[] = {DbParam::from_int(42), DbParam::from_text("paid")};
//   UniquePointer<IResultSet> rows;
//   if (db->execute("SELECT id, total FROM orders WHERE customer = ? AND state = ?", args, &rows) == DbStatus::Ok)
//     for (uint64_t r = 0; r < rows->row_count(); ++r) use(rows->text(r, 0), rows->text(r, 1));
class IConnection {
 public:
  virtual const char* driver_name() const = 0;
  virtual bool is_open() const = 0;
  // The server's own version text ("16.2"), or "".
  virtual StringView server_version() const = 0;
  // True once the connection is encrypted with TLS.
  virtual bool encrypted() const = 0;

  // Runs one SQL statement. A "?" in the SQL stands for the next entry of `params` (the driver turns it into its own marker; write "??" for a
  // literal question mark; "?" inside quotes and comments is left alone), so the values never become part of the SQL text. `result` (may be
  // nullptr when the answer is not wanted) receives the rows, or, for a statement without rows, just what it did. Without parameters the SQL
  // may hold several statements separated by ';' (the result is the last one's).
  virtual DbStatus execute(StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result) = 0;
  DbStatus execute(StringView sql, UniquePointer<IResultSet>* result = nullptr) { return execute(sql, ArrayView<const DbParam>(), result); }

  // Transactions: statements between begin() and commit() take effect together, rollback() drops them.
  virtual DbStatus begin() = 0;
  virtual DbStatus commit() = 0;
  virtual DbStatus rollback() = 0;
  virtual bool in_transaction() const = 0;

  // Asks the server whether it is still there.
  virtual DbStatus ping() = 0;

  virtual const DbError& last_error() const = 0;
  // Ends the session politely. Safe to call twice.
  virtual void close() = 0;

 protected:
  constexpr IConnection() = default;
  ~IConnection() = default;
};

}  // namespace database
