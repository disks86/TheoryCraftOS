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
#include "ObjectContainer/List.hpp"

namespace database {

// An IResultSet that keeps every value as the text the database sent. The drivers of databases that send text (PostgreSQL's simple form, and
// most others) fill one in: describe the columns, then add the rows cell by cell.
//
//   TextResultSet rows(allocator);
//   rows.add_column("id", DbType::Integer);
//   rows.begin_row(); rows.add_cell("42");
//   rows.set_command("SELECT 1", 1);
class TextResultSet final : public IResultSet {
 public:
  explicit TextResultSet(allocators::IAllocator& allocator) : allocator_(allocator), columns_(allocator), cells_(allocator), data_(allocator), command_(allocator) {}

  // --- Filling in
  bool add_column(StringView name, DbType type);
  // Starts a row; the cells follow (exactly column_count() of them). With no columns it just counts the row.
  bool begin_row();
  bool add_cell(StringView text);
  bool add_null();
  void set_command(StringView command, uint64_t rows_affected) {
    command_.assign(command);
    rows_affected_ = rows_affected;
  }
  // The memory the values take (a driver compares it with its limit while rows come in).
  uint64_t bytes_used() const { return data_.size() + cells_.count() * sizeof(Cell); }
  // False if the last row has fewer cells than there are columns.
  bool complete() const { return columns_.count() == 0 || cells_.count() % columns_.count() == 0; }

  // --- IResultSet
  uint32_t column_count() const override { return static_cast<uint32_t>(columns_.count()); }
  StringView column_name(uint32_t column) const override { return column < columns_.count() ? columns_[column].name.view() : StringView(); }
  DbType column_type(uint32_t column) const override { return column < columns_.count() ? columns_[column].type : DbType::Unknown; }
  int32_t find_column(StringView name) const override;
  uint64_t row_count() const override { return columns_.count() ? cells_.count() / columns_.count() : rows_without_columns_; }
  bool is_null(uint64_t row, uint32_t column) const override;
  StringView text(uint64_t row, uint32_t column) const override;
  bool get_bool(uint64_t row, uint32_t column, bool* out) const override;
  bool get_int(uint64_t row, uint32_t column, int64_t* out) const override;
  bool get_real(uint64_t row, uint32_t column, double* out) const override;
  bool get_bytes(uint64_t row, uint32_t column, ByteBuffer* out) const override;
  uint64_t rows_affected() const override { return rows_affected_; }
  StringView command() const override { return command_.view(); }

 private:
  struct Column {
    String name;
    DbType type;
  };
  struct Cell {
    uint64_t offset;
    uint32_t length;
    bool null;
  };
  const Cell* cell(uint64_t row, uint32_t column) const {
    if (column >= columns_.count() || row >= row_count()) return nullptr;
    return &cells_[row * columns_.count() + column];
  }
  StringView view_of(const Cell& c) const { return StringView(reinterpret_cast<const char*>(data_.view().data()) + c.offset, c.length); }

  allocators::IAllocator& allocator_;
  List<Column> columns_;
  List<Cell> cells_;
  ByteBuffer data_;
  String command_;
  uint64_t rows_affected_ = 0;
  uint64_t rows_without_columns_ = 0;
};

}  // namespace database
