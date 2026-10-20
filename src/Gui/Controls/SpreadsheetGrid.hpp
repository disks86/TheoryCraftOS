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
#include "Gui/Controls/ScrollBar.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace gui {

// What a SpreadsheetGrid shows.
class ISheetView {
 public:
  virtual uint32_t column_count() const = 0;
  virtual uint32_t row_count() const = 0;
  // Writes how the cell reads into `out`; `right` true for a number (the grid aligns those on the right). False if the cell could not be read.
  virtual bool cell_text(uint32_t column, uint32_t row, String* out, bool* right) = 0;

 protected:
  constexpr ISheetView() = default;
  ~ISheetView() = default;
};

// A sheet of cells in rows and columns: column letters (A, B, ... Z, AA, ...) across the top, row numbers down the side, and the cells of an ISheetView
// between them, with scroll bars when there are more than fit.
//  - One cell is active (a heavy frame) and a block of cells is selected, from an anchor cell to the active one. A click selects a cell, a drag selects
//    a block, a click on a column letter or a row number selects the column or the row, the corner selects everything. The border between two column
//    letters can be dragged to change the column's width.
//  - Arrow keys move the active cell (with Shift they stretch the selection), Tab and Shift+Tab move right and left, Enter and Shift+Enter down and up,
//    Home goes to the first column (Ctrl+Home to A1), Page Up and Page Down move a page, Ctrl+A selects everything. The mouse wheel scrolls three rows.
//  - The grid does not edit. Typing a character or F2 fires on_edit_requested (edit_request_char() is the character, 0 for F2) and the program
//    puts the text in whatever edits it; while that goes on set_edit_text() shows the text in the active cell. Delete and Backspace fire on_clear_requested
//    and Ctrl+C, Ctrl+X and Ctrl+V fire on_copy_requested, on_cut_requested and on_paste_requested.
class SpreadsheetGrid : public Control {
 public:
  static constexpr int32_t kHeaderHeight = 18, kRowHeight = 18, kDefaultColumnWidth = 80, kMinColumnWidth = 24, kBorder = 2, kResizeZone = 3;

  explicit SpreadsheetGrid(allocators::IAllocator& allocator);

  // The cells shown (nullptr: none). It must outlive the grid. Call model_changed() when its contents or size change.
  void set_model(ISheetView* model);
  void model_changed();

  // --- Selection. Columns and rows count from 0.
  uint32_t active_column() const { return active_column_; }
  uint32_t active_row() const { return active_row_; }
  uint32_t selection_left() const { return active_column_ < anchor_column_ ? active_column_ : anchor_column_; }
  uint32_t selection_right() const { return active_column_ < anchor_column_ ? anchor_column_ : active_column_; }
  uint32_t selection_top() const { return active_row_ < anchor_row_ ? active_row_ : anchor_row_; }
  uint32_t selection_bottom() const { return active_row_ < anchor_row_ ? anchor_row_ : active_row_; }
  bool single_cell_selected() const { return active_column_ == anchor_column_ && active_row_ == anchor_row_; }
  // Makes (column, row) the active cell and scrolls it into view; `extend` keeps the anchor (a bigger selection). The handler fires when the active cell
  // or the selection changed.
  void select(uint32_t column, uint32_t row, bool extend = false);
  void select_all();
  void on_selection_changed(Handler handler) { UiGuard lock(this); selection_changed_ = handler; }

  // --- Editing hooks (see above)
  void on_edit_requested(Handler handler) { UiGuard lock(this); edit_requested_ = handler; }
  void on_clear_requested(Handler handler) { UiGuard lock(this); clear_requested_ = handler; }
  void on_copy_requested(Handler handler) { UiGuard lock(this); copy_requested_ = handler; }
  void on_cut_requested(Handler handler) { UiGuard lock(this); cut_requested_ = handler; }
  void on_paste_requested(Handler handler) { UiGuard lock(this); paste_requested_ = handler; }
  char edit_request_char() const { return request_char_; }
  // Shows `text` in the active cell as what is being typed there; stop_editing() goes back to the cell's own text.
  void set_edit_text(StringView text);
  void stop_editing();
  bool editing() const { return editing_; }

  // --- Columns
  int32_t column_width(uint32_t column) const { return column < widths_.count() ? widths_[column] : kDefaultColumnWidth; }
  void set_column_width(uint32_t column, int32_t width);

  // --- Scrolling and geometry (for painting and tests)
  uint32_t first_column() const { return first_column_; }
  uint32_t first_row() const { return first_row_; }
  uint32_t visible_row_count() const;
  uint32_t visible_column_count() const;   // columns shown completely
  int32_t row_header_width() const;
  Rect2D body_rect() const;                 // where the cells are
  // The cell's rectangle in this control (it may be off screen: then it lies outside body_rect()).
  Rect2D cell_rect(uint32_t column, uint32_t row) const;
  // The cell under a point, or false (headers, empty space).
  bool cell_at(int32_t x, int32_t y, uint32_t* column, uint32_t* row) const;
  ScrollBar& vertical_scroll_bar() { return vscroll_; }
  ScrollBar& horizontal_scroll_bar() { return hscroll_; }
  // "A", ... "Z", "AA", ... for a column.
  static void column_title(uint32_t column, String* out);

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {400, 240}; }

 private:
  uint32_t columns() const { return model_ ? model_->column_count() : 0; }
  uint32_t rows() const { return model_ ? model_->row_count() : 0; }
  int32_t total_width() const;
  void update_scroll_bars();
  void ensure_visible(uint32_t column, uint32_t row);
  void move(int32_t dc, int32_t dr, bool extend);
  void notify_selection();
  int32_t column_at_x(int32_t x, bool* on_border) const;
  void on_scroll_changed();
  static void scrolled(void* self, Control&);

  ISheetView* model_ = nullptr;
  List<int32_t> widths_;
  uint32_t active_column_ = 0, active_row_ = 0, anchor_column_ = 0, anchor_row_ = 0;
  uint32_t first_column_ = 0, first_row_ = 0;
  ScrollBar vscroll_, hscroll_;
  bool show_v_ = false, show_h_ = false, syncing_ = false;
  Handler selection_changed_, edit_requested_, clear_requested_, copy_requested_, cut_requested_, paste_requested_;
  char request_char_ = 0;
  String edit_text_, cell_text_;
  bool editing_ = false;
  // Mouse state.
  enum class Drag : uint8_t { None, Cells, Columns, Rows, Resize } drag_ = Drag::None;
  int32_t resize_column_ = -1, resize_origin_x_ = 0, resize_origin_width_ = 0;
};

}  // namespace gui
