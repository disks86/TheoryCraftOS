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

#include "Gui/Controls/GridModel.hpp"

namespace gui {

int32_t TextGridModel::add_row(const StringView* cells, uint32_t count) {
  if (columns_ == 0) return -1;
  const uint64_t before = cells_.count();
  for (uint32_t c = 0; c < columns_; ++c) {
    String cell(allocator_);
    if ((c < count && !cell.assign(cells[c])) || !cells_.push_back(util::move(cell))) {
      while (cells_.count() > before) cells_.remove_at(cells_.count() - 1);  // undo the partial row
      return -1;
    }
  }
  return static_cast<int32_t>(rows_++);
}

bool TextGridModel::set_cell(uint32_t row, uint32_t column, StringView text) {
  if (row >= rows_ || column >= columns_) return false;
  return cells_[uint64_t{row} * columns_ + column].assign(text);
}

bool TextGridModel::remove_row(uint32_t row) {
  if (row >= rows_) return false;
  for (uint32_t c = 0; c < columns_; ++c) cells_.remove_at(uint64_t{row} * columns_);
  --rows_;
  return true;
}

bool TextGridModel::cell_text(uint32_t row, uint32_t column, String* out) const {
  if (row >= rows_ || column >= columns_) return out->assign(StringView());
  return out->assign(cells_[uint64_t{row} * columns_ + column].view());
}

}  // namespace gui
