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

// The tables of the system database, in memory, and how they are written to and read from a file.
#pragma once
#include "Database/DatabaseTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace database::local {

// One value in a table or on its way through an expression. Booleans are Integers 0 and 1; a BOOLEAN column shows them as true and false.
struct Value {
  enum class Kind : uint8_t { Null, Integer, Real, Text, Bytes };
  explicit Value(allocators::IAllocator& allocator) : text(allocator) {}
  Value(Value&&) = default;
  Value& operator=(Value&&) = default;

  Kind kind = Kind::Null;
  int64_t integer = 0;
  double real = 0;
  String text;   // the characters of a Text, the bytes of a Bytes

  bool is_null() const { return kind == Kind::Null; }
  void set_null() {
    kind = Kind::Null;
    text.clear();
  }
  void set_int(int64_t v) {
    kind = Kind::Integer;
    integer = v;
    text.clear();
  }
  void set_real(double v) {
    kind = Kind::Real;
    real = v;
    text.clear();
  }
  bool set_text(StringView v) {
    kind = Kind::Text;
    return text.assign(v);
  }
  bool set_bytes(StringView v) {
    kind = Kind::Bytes;
    return text.assign(v);
  }
  bool copy_from(const Value& other) {
    kind = other.kind;
    integer = other.integer;
    real = other.real;
    return text.assign(other.text.view());
  }
};

struct ColumnDef {
  explicit ColumnDef(allocators::IAllocator& allocator) : name(allocator), default_value(allocator) {}
  ColumnDef(ColumnDef&&) = default;
  ColumnDef& operator=(ColumnDef&&) = default;

  String name;
  DbType type = DbType::Unknown;   // Unknown: values are kept as they come
  bool not_null = false;
  bool primary_key = false;        // the columns flagged together are the table's key
  bool has_default = false;
  Value default_value;
};

// A table: its columns and its rows, which are kept one after the other in a single list.
class Table {
 public:
  Table(allocators::IAllocator& allocator, StringView table_name) : allocator_(allocator), name(allocator), columns(allocator), cells(allocator) { name.assign(table_name); }

  allocators::IAllocator& allocator() const { return allocator_; }
  uint64_t row_count() const { return columns.count() ? cells.count() / columns.count() : 0; }
  Value& at(uint64_t row, uint64_t column) { return cells[row * columns.count() + column]; }
  const Value& at(uint64_t row, uint64_t column) const { return cells[row * columns.count() + column]; }
  // The column called `column_name` (not case sensitive), or -1.
  int32_t find_column(StringView column_name) const;
  bool has_key() const;
  // Adds a row of exactly columns.count() values (moved).
  bool add_row(ArrayView<Value> row);
  // Removes the rows whose flag is not 0.
  void erase_rows(const List<uint8_t>& flags);

  String name;
  List<ColumnDef> columns;
  List<Value> cells;

 private:
  allocators::IAllocator& allocator_;
};

class Catalog {
 public:
  explicit Catalog(allocators::IAllocator& allocator) : allocator_(allocator), tables_(allocator) {}
  allocators::IAllocator& allocator() const { return allocator_; }

  uint64_t table_count() const { return tables_.count(); }
  Table& table_at(uint64_t index) { return *tables_[index]; }
  Table* find(StringView name) const;
  bool add(UniquePointer<Table> table) { return tables_.push_back(static_cast<UniquePointer<Table>&&>(table)); }
  bool drop(StringView name);
  void clear() { tables_.clear(); }

  // The whole catalog as bytes (a "TCDB" file with a checksum at the end).
  bool serialize(ByteBuffer* out) const;
  // Replaces the contents with those of `data`. On a failure (Corrupt) the catalog is left empty and `error` says why.
  DbStatus load(ConstByteView data, DbError* error);

 private:
  allocators::IAllocator& allocator_;
  List<UniquePointer<Table>> tables_;
};

}  // namespace database::local
