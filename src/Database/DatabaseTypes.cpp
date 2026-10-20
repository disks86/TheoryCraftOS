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

#include "Database/DatabaseTypes.hpp"

namespace database {

const char* db_status_name(DbStatus status) {
  switch (status) {
    case DbStatus::Ok: return "ok";
    case DbStatus::InvalidArgument: return "invalid argument";
    case DbStatus::UnknownDriver: return "unknown driver";
    case DbStatus::ResolveFailed: return "host not found";
    case DbStatus::ConnectFailed: return "connection failed";
    case DbStatus::TlsFailed: return "TLS failed";
    case DbStatus::AuthFailed: return "authentication failed";
    case DbStatus::Protocol: return "protocol error";
    case DbStatus::Server: return "server error";
    case DbStatus::Timeout: return "timed out";
    case DbStatus::ConnectionLost: return "connection lost";
    case DbStatus::Closed: return "closed";
    case DbStatus::TooLarge: return "answer too large";
    case DbStatus::NotSupported: return "not supported";
    case DbStatus::OutOfMemory: return "out of memory";
  }
  return "unknown";
}

const char* db_type_name(DbType type) {
  switch (type) {
    case DbType::Boolean: return "boolean";
    case DbType::Integer: return "integer";
    case DbType::Real: return "real";
    case DbType::Decimal: return "decimal";
    case DbType::Text: return "text";
    case DbType::Bytes: return "bytes";
    case DbType::Date: return "date";
    case DbType::Time: return "time";
    case DbType::Timestamp: return "timestamp";
    case DbType::Json: return "json";
    case DbType::Uuid: return "uuid";
    default: return "unknown";
  }
}

}  // namespace database
