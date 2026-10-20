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

// Database access for programs that do not care which database they talk to: the types every driver shares. A program gets an IConnection from the
// DatabaseManager (by URL, "postgres://user:password@host/database"), runs SQL with IConnection::execute() and reads the answer from an IResultSet;
// what differs between databases stays inside the driver (see Database/Postgres). There are no exceptions: every call returns a DbStatus, and the
// connection keeps the details of the last failure (DbError).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"

namespace database {

enum class DbStatus : uint8_t {
  Ok,
  InvalidArgument,   // a bad URL, SQL with the wrong number of parameters, ...
  UnknownDriver,     // no driver for the URL's scheme
  ResolveFailed,     // the host name did not resolve
  ConnectFailed,     // the TCP connection was refused or timed out
  TlsFailed,         // the server does not do TLS and it was required, or the handshake failed
  AuthFailed,        // the server refused the user or the password
  Protocol,          // the server sent something that is not valid for the protocol
  Server,            // the server reported an error (see DbError::sqlstate and message)
  Timeout,           // the server did not answer in time
  ConnectionLost,    // the connection broke
  Closed,            // the connection was closed (by close() or after an error that left it unusable)
  TooLarge,          // an answer over the connection's limit
  NotSupported,      // the driver or the server cannot do this
  OutOfMemory,
};
const char* db_status_name(DbStatus status);

// What went wrong with the last operation on a connection.
struct DbError {
  explicit DbError(allocators::IAllocator& allocator) : sqlstate(allocator), message(allocator), detail(allocator) {}
  DbStatus status = DbStatus::Ok;
  String sqlstate;   // the five characters of the SQLSTATE, when the server gave one ("23505" for a duplicate key)
  String message;
  String detail;     // more from the server (which value, which constraint), or empty
  void clear() {
    status = DbStatus::Ok;
    sqlstate.clear();
    message.clear();
    detail.clear();
  }
  void set(DbStatus s, StringView text) {
    clear();
    status = s;
    message.assign(text);
  }
};

// The kinds of value a column holds, as far as a program needs to tell them apart. Everything can be read as text.
enum class DbType : uint8_t { Unknown, Boolean, Integer, Real, Decimal, Text, Bytes, Date, Time, Timestamp, Json, Uuid };
const char* db_type_name(DbType type);

// A value handed to a statement in place of a "?". It points at the data it is made from, which must stay valid until execute() returns.
struct DbParam {
  enum class Kind : uint8_t { Null, Boolean, Integer, Real, Text, Bytes };
  Kind kind = Kind::Null;
  bool boolean = false;
  int64_t integer = 0;
  double real = 0;
  StringView text;
  ConstByteView bytes;

  static DbParam null() { return DbParam(); }
  static DbParam from_bool(bool v) {
    DbParam p;
    p.kind = Kind::Boolean;
    p.boolean = v;
    return p;
  }
  static DbParam from_int(int64_t v) {
    DbParam p;
    p.kind = Kind::Integer;
    p.integer = v;
    return p;
  }
  static DbParam from_real(double v) {
    DbParam p;
    p.kind = Kind::Real;
    p.real = v;
    return p;
  }
  static DbParam from_text(StringView v) {
    DbParam p;
    p.kind = Kind::Text;
    p.text = v;
    return p;
  }
  static DbParam from_bytes(ConstByteView v) {
    DbParam p;
    p.kind = Kind::Bytes;
    p.bytes = v;
    return p;
  }
};

// How a connection is asked to treat TLS.
enum class TlsMode : uint8_t {
  Disable,   // never
  Prefer,    // when the server offers it (and it is plain otherwise)
  Require,   // or the connection fails
};

}  // namespace database
