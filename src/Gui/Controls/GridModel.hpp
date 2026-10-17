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
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"
#include "Utility/String.hpp"

namespace gui {

// The data a DataGrid shows: rows of text cells. A program with its own records implements this and lets
// the grid ask for what is on screen, so a million rows cost nothing; for small tables use TextGridModel
// (or DataGrid's own add_row). Rows are numbered from 0 and the grid never changes the model.
class IGridModel {
 public:
  virtual uint32_t row_count() const = 0;
  // Puts the text of one cell in `*out` (replacing what it held). False if it ran out of memory.
  virtual bool cell_text(uint32_t row, uint32_t column, String* out) const = 0;

 protected:
  constexpr IGridModel() = default;
  ~IGridModel() = default;
};

// A table of strings held in memory: a fixed number of columns (set while the table is empty) and any
// number of rows.
class TextGridModel final : public IGridModel {
 public:
  explicit TextGridModel(allocators::IAllocator& allocator) : allocator_(allocator), cells_(allocator) {}

  // Only while there are no rows. False otherwise.
  bool set_column_count(uint32_t columns) {
    if (rows_ != 0) return false;
    columns_ = columns;
    return true;
  }
  uint32_t column_count() const { return columns_; }
  uint32_t row_count() const override { return rows_; }

  // Appends a row; missing cells are empty, extra ones ignored. Returns the row's index, or -1 if memory ran out
  // (the table is then unchanged).
  int32_t add_row(const StringView* cells, uint32_t count);
  bool set_cell(uint32_t row, uint32_t column, StringView text);
  bool remove_row(uint32_t row);
  void clear() {
    cells_.clear();
    rows_ = 0;
  }
  bool cell_text(uint32_t row, uint32_t column, String* out) const override;

 private:
  allocators::IAllocator& allocator_;
  List<String> cells_;  // row after row
  uint32_t columns_ = 0;
  uint32_t rows_ = 0;
};

}  // namespace gui
