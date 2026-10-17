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
#include "Gui/Control.hpp"
#include "Gui/Controls/GridModel.hpp"
#include "Gui/Controls/ScrollBar.hpp"

namespace gui {

enum class Align : uint8_t { Left, Center, Right };

// A table: a header row of column titles over rows of cells, with scroll bars when there is more than fits.
//  - Rows come from an IGridModel (set_model) or, for small tables, from the grid's own TextGridModel
//    (add_column, then add_row).
//  - Clicking a header sorts by that column, again to reverse it (numbers sort as numbers, other text
//    without regard to case; the sort is stable). Dragging the border at a header's right edge resizes the column.
//  - One row is selected: click it, or use Up/Down/Page Up/Page Down/Home/End; Enter activates it. Left/Right
//    scroll sideways and the mouse wheel scrolls three rows. Ctrl+C copies the selected row (cells separated by tabs) to the clipboard.
// Row numbers given to and returned by the grid are the model's, not positions on screen.
class DataGrid : public Control {
 public:
  static constexpr int32_t kHeaderHeight = 20, kRowHeight = 18, kBorder = 2, kMinColumnWidth = 24, kResizeZone = 3;

  explicit DataGrid(allocators::IAllocator& allocator);

  // --- Columns. Returns the new column's index, or -1 (not allowed once the grid's own table has rows).
  int32_t add_column(StringView title, int32_t width, Align align = Align::Left);
  uint32_t column_count() const { return static_cast<uint32_t>(columns_.count()); }
  StringView column_title(uint32_t column) const { return column < columns_.count() ? columns_[column].title.view() : StringView(); }
  int32_t column_width(uint32_t column) const { return column < columns_.count() ? columns_[column].width : 0; }
  void set_column_width(uint32_t column, int32_t width);

  // --- Rows. A model of your own (nullptr goes back to the grid's own table). Call model_changed() when it
  // changes behind the grid's back.
  void set_model(IGridModel* model);
  void model_changed();
  uint32_t row_count() const { return model_->row_count(); }
  // The grid's own table (used when no model was set): returns the row number or -1.
  int32_t add_row(const StringView* cells, uint32_t count);
  bool set_cell(uint32_t row, uint32_t column, StringView text);
  void clear_rows();

  // --- Selection
  int32_t selected_row() const { return selected_; }  // -1 = none
  // Selects a row (-1 clears) and scrolls it into view. The handler fires when the selection changes.
  void select_row(int32_t row);
  void on_selection_changed(Handler handler) { UiGuard lock(this); selection_changed_ = handler; }
  // Fired by Enter on the selected row.
  void on_row_activated(Handler handler) { UiGuard lock(this); activated_ = handler; }

  // --- Sorting. column -1 restores the model's order.
  bool sort_by(int32_t column, bool ascending = true);
  int32_t sort_column() const { return sort_column_; }
  bool sort_ascending() const { return ascending_; }
  void on_sort_changed(Handler handler) { UiGuard lock(this); sort_changed_ = handler; }
  // The model row shown at screen position `index` (0 = top of the sorted list), or -1.
  int32_t row_at_position(uint32_t index) const { return index < order_.count() ? static_cast<int32_t>(order_[index]) : -1; }

  // --- Scrolling and geometry (for painting and tests)
  uint32_t first_visible_position() const { return top_; }
  int32_t horizontal_offset() const { return scroll_x_; }
  uint32_t visible_row_count() const;
  Rect2D body_rect() const;     // where the rows are
  Rect2D header_rect() const;
  int32_t column_left(uint32_t column) const;  // x of the column's left edge in this control (scrolled)
  ScrollBar& vertical_scroll_bar() { return vscroll_; }
  ScrollBar& horizontal_scroll_bar() { return hscroll_; }

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {320, 160}; }

 private:
  struct Column {
    String title;
    int32_t width;
    Align align;
  };

  int32_t total_width() const;
  void update_scroll_bars();
  void rebuild_order();
  bool row_less(uint32_t a, uint32_t b);
  void clamp_scroll();
  void ensure_visible(uint32_t position);
  int32_t position_of_row(int32_t row) const;
  void move_selection_to(int32_t position);
  int32_t column_at(int32_t x, bool* on_border) const;
  void toggle_sort(int32_t column);
  void on_scroll_changed();
  static void vertical_changed(void* self, Control&);
  static void horizontal_changed(void* self, Control&);

  TextGridModel own_;
  IGridModel* model_;
  List<Column> columns_;
  List<uint32_t> order_;   // model rows in the order shown
  int32_t selected_ = -1;  // a model row
  int32_t sort_column_ = -1;
  bool ascending_ = true;
  uint32_t top_ = 0;       // position of the first visible row
  int32_t scroll_x_ = 0;
  ScrollBar vscroll_, hscroll_;
  bool show_v_ = false, show_h_ = false;
  bool syncing_ = false;   // the grid is moving the scroll bars itself
  Handler selection_changed_, activated_, sort_changed_;
  String cell_a_, cell_b_;  // scratch for comparisons and painting

  // Mouse state.
  int32_t resizing_ = -1;
  int32_t resize_origin_x_ = 0, resize_origin_width_ = 0;
  int32_t header_pressed_ = -1;
};

}  // namespace gui
