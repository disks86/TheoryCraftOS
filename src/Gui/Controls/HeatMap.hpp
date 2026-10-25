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

// A heat map: a grid of cells, one per (row, column), each filled with a colour that follows its value along a blue - teal - yellow - red ramp
// (the lowest value blue, the highest red). Row labels stand on the left and column labels above, cut off to their cell. The ramp spans the smallest
// to the largest value unless set_range() fixes it (values outside a fixed range take the end colours). The values can be printed in the cells.
// A click on a cell selects it (outlined, its row and column labels highlighted); the arrow keys move the selection. Values are not negative.
// No floating point is used.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/StringList.hpp"
#include "ObjectContainer/List.hpp"

namespace gui {

class HeatMap : public Control {
 public:
  explicit HeatMap(allocators::IAllocator& allocator) : Control(allocator), row_labels_(allocator), column_labels_(allocator), values_(allocator) {}

  // Replaces the grid by `rows` x `columns` cells of value 0 and empty labels, and clears the selection. False (leaving an empty grid) if a
  // dimension is negative or above kMaxDimension, the cells would be more than kMaxCells, or memory ran out.
  bool set_dimensions(int32_t rows, int32_t columns);
  int32_t row_count() const { return row_labels_.count(); }
  int32_t column_count() const { return column_labels_.count(); }
  // Out of range indexes are ignored (and read as an empty label / 0).
  void set_row_label(int32_t row, StringView label);
  void set_column_label(int32_t column, StringView label);
  StringView row_label(int32_t row) const { return row_labels_.at(row); }
  StringView column_label(int32_t column) const { return column_labels_.at(column); }
  void set_value(int32_t row, int32_t column, uint64_t value);
  uint64_t value(int32_t row, int32_t column) const;

  // The ends of the colour ramp: automatic (the smallest and largest value) until set_range() is called. A low above the high are swapped.
  void set_range(uint64_t low, uint64_t high);
  void clear_range() { UiGuard lock(this); fixed_range_ = false; invalidate(); }
  bool fixed_range() const { return fixed_range_; }
  uint64_t range_low() const;   // the ramp's low end in use
  uint64_t range_high() const;  // the ramp's high end in use
  // Where a value falls on the ramp, 0..1000 (0 if the range is a single value).
  uint32_t position(uint64_t value) const;
  // The colour of a ramp position (0..1000, larger values count as 1000).
  static uint32_t ramp_color(uint32_t permille);
  uint32_t color_of(int32_t row, int32_t column) const { return ramp_color(position(value(row, column))); }

  void set_show_values(bool shown) { UiGuard lock(this); show_values_ = shown; invalidate(); }
  bool show_values() const { return show_values_; }

  int32_t selected_row() const { return selected_row_; }        // -1 = none
  int32_t selected_column() const { return selected_column_; }  // -1 = none
  void select_cell(int32_t row, int32_t column);                // out of range clears the selection
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // The area of the cells, in this control's coordinates; width 0 if there is no room (every cell needs a pixel).
  Rect2D grid_rect() const;
  // The cell (r, c); the cells tile the grid. An empty rectangle for an index out of range.
  Rect2D cell_rect(int32_t row, int32_t column) const;
  // The cell under a point of this control, or false.
  bool cell_at(int32_t x, int32_t y, int32_t& row, int32_t& column) const;

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  static constexpr int32_t kMargin = 4, kLabelHeight = 18, kMaxLabelWidth = 96, kMaxDimension = 1024, kMaxCells = 1 << 18;

 protected:
  Size default_size() const override { return {260, 160}; }

 private:
  int32_t row_label_width() const;
  int32_t header_height() const;
  void select(int32_t row, int32_t column);

  StringList row_labels_;
  StringList column_labels_;
  List<uint64_t> values_;  // row-major
  uint64_t low_ = 0, high_ = 0;
  bool fixed_range_ = false;
  bool show_values_ = false;
  int32_t selected_row_ = -1, selected_column_ = -1;
  Handler changed_;
};

}  // namespace gui
