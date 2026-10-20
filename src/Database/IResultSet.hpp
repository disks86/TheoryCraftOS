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
#include "Utility/ByteBuffer.hpp"

namespace database {

// The answer to a statement: its columns and all of its rows, held in memory (a connection refuses an answer over its limit instead of using up
// the memory). Rows and columns count from 0. A statement that returns no rows (INSERT, CREATE TABLE) has no columns; rows_affected() and
// command() then say what it did. Text read from a result is UTF-8 and stays valid for as long as the result lives.
class IResultSet {
 public:
  virtual uint32_t column_count() const = 0;
  virtual StringView column_name(uint32_t column) const = 0;
  virtual DbType column_type(uint32_t column) const = 0;
  // The column with this name (not case sensitive), or -1.
  virtual int32_t find_column(StringView name) const = 0;
  virtual uint64_t row_count() const = 0;

  virtual bool is_null(uint64_t row, uint32_t column) const = 0;
  // The value as the database wrote it ("" for NULL and for cells outside the result).
  virtual StringView text(uint64_t row, uint32_t column) const = 0;
  // The value as a C++ type. False for NULL, and when the value does not read as that type (text in an integer column, ...); *out is then untouched.
  virtual bool get_bool(uint64_t row, uint32_t column, bool* out) const = 0;
  virtual bool get_int(uint64_t row, uint32_t column, int64_t* out) const = 0;
  virtual bool get_real(uint64_t row, uint32_t column, double* out) const = 0;
  // The value as bytes: the contents of a binary column (hex or escape form decoded), the UTF-8 bytes of anything else. *out is replaced.
  virtual bool get_bytes(uint64_t row, uint32_t column, ByteBuffer* out) const = 0;

  // Rows the statement inserted, updated or deleted (the number of rows for a query); 0 when it does not say.
  virtual uint64_t rows_affected() const = 0;
  // The server's name for what it did ("SELECT 3", "INSERT 0 1", "CREATE TABLE").
  virtual StringView command() const = 0;

 protected:
  constexpr IResultSet() = default;
  ~IResultSet() = default;
};

}  // namespace database
