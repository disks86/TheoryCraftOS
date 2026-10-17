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

// A list of items that can be shown three ways, switched with set_view_mode():
//  - Details: a header of column titles over one row per item (like DataGrid; drag the border at a
//    header's right edge to resize a column). The first column is the item's name and has its icon.
//  - List: small icons with the name, filled top to bottom and then left to right; scrolls sideways.
//  - Icons: large icons with the name below, in a grid that wraps to the width; scrolls vertically.
// Every item has a text cell per column (the first is its name) and an icon. One item is selected: click it, or use the
// arrow keys (along the way the items are laid out), Home/End; Enter - or a second click on the selected item
// within a double-click interval - activates it. Ctrl+C copies the selected item's cells (tab separated) to the clipboard. The mouse wheel scrolls.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/GridModel.hpp"
#include "Gui/Controls/ScrollBar.hpp"

namespace gui {

enum class ViewMode : uint8_t { List, Icons, Details };
enum class ItemIcon : uint8_t { File, Folder, Drive };

// Draws one of the item icons with its top left at (x, y), `size` pixels square (a multiple of 16 works best).
void draw_item_icon(Painter& painter, ItemIcon icon, int32_t x, int32_t y, int32_t size);

class ListView : public Control {
 public:
  static constexpr int32_t kBorder = 2, kHeaderHeight = 20, kRowHeight = 18, kSmallIcon = 16, kLargeIcon = 32;
  static constexpr int32_t kIconCellWidth = 80, kIconCellHeight = 68, kMinColumnWidth = 24, kResizeZone = 3;
  static constexpr int32_t kMinListColumnWidth = 96, kMaxListColumnWidth = 240;
  static constexpr uint64_t kDoubleClickUs = 500000;

  explicit ListView(allocators::IAllocator& allocator);

  // --- View
  void set_view_mode(ViewMode mode);
  ViewMode view_mode() const { return mode_; }

  // --- Columns (for the details view; the first is the name, which is the only one the other views show).
  // Returns the new column's index, or -1 (not allowed once there are items). At least one column is needed.
  int32_t add_column(StringView title, int32_t width, Align align = Align::Left);
  uint32_t column_count() const { return static_cast<uint32_t>(columns_.count()); }
  int32_t column_width(uint32_t column) const { return column < columns_.count() ? columns_[column].width : 0; }
  void set_column_width(uint32_t column, int32_t width);

  // --- Items. `cells` holds the text of each column (missing ones are empty). Returns the item's index or -1.
  int32_t add_item(const StringView* cells, uint32_t count, ItemIcon icon);
  void clear_items();
  uint32_t item_count() const { return model_.row_count(); }
  ItemIcon item_icon(uint32_t item) const { return item < icons_.count() ? static_cast<ItemIcon>(icons_[item]) : ItemIcon::File; }
  bool item_text(uint32_t item, uint32_t column, String* out) const { return model_.cell_text(item, column, out); }

  // Whether Ctrl+C copies the selected item's cells (default); a program with a better idea of what to copy turns it off.
  void set_copy_enabled(bool enabled) { UiGuard lock(this); copy_enabled_ = enabled; }
  // Puts the selected item's cells (tab separated) on the clipboard; false if nothing is selected or there is no clipboard.
  bool copy_selected_item();

  // --- Selection
  int32_t selected_item() const { return selected_; }  // -1 = none
  // Selects an item (-1 clears) and scrolls it into view. The handler fires when the selection changes.
  void select_item(int32_t item);
  void on_selection_changed(Handler handler) { UiGuard lock(this); selection_changed_ = handler; }
  // Fired by Enter or a double click on the selected item.
  void on_item_activated(Handler handler) { UiGuard lock(this); activated_ = handler; }
  // The time (microseconds) that double clicks are measured with; the pointed-to value is read on every click.
  // Without a clock only Enter activates.
  void set_clock(const uint64_t* now_us) { UiGuard lock(this); clock_ = now_us; }

  // --- Geometry (for painting and tests)
  Rect2D body_rect() const;    // where the items are
  Rect2D header_rect() const;  // details view only
  // Where an item is, in this control's coordinates (scrolled); false if it does not exist.
  bool item_rect(uint32_t item, Rect2D* out) const;
  int32_t scroll_x() const { return scroll_x_; }
  int32_t scroll_y() const { return scroll_y_; }
  ScrollBar& vertical_scroll_bar() { return vscroll_; }
  ScrollBar& horizontal_scroll_bar() { return hscroll_; }

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  void prepare_context_menu(int32_t x, int32_t y) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {320, 200}; }

 private:
  struct Column {
    String title;
    int32_t width;
    Align align;
  };
  // How the items sit in the content (before scrolling).
  struct Metrics {
    int32_t cell_w = 0, cell_h = 0;
    int32_t per_line = 1;  // List: items per column; Icons: items per row
    int32_t content_w = 0, content_h = 0;
  };

  Metrics measure(int32_t viewport_w, int32_t viewport_h) const;
  int32_t total_column_width() const;
  int32_t list_cell_width() const;
  void update_scroll_bars();
  void ensure_visible(uint32_t item);
  int32_t item_at(int32_t x, int32_t y) const;
  void move_selection(int32_t delta);
  void scroll_by(int32_t dx, int32_t dy);
  int32_t column_at(int32_t x, bool* on_border) const;
  void paint_item_text(Painter& painter, const Rect2D& cell, uint32_t item, bool selected, uint32_t color);
  void on_scroll_changed();
  static void vertical_changed(void* self, Control&);
  static void horizontal_changed(void* self, Control&);

  TextGridModel model_;
  List<uint8_t> icons_;
  List<Column> columns_;
  ViewMode mode_ = ViewMode::Details;
  int32_t selected_ = -1;
  int32_t scroll_x_ = 0, scroll_y_ = 0;
  Metrics metrics_;
  bool show_v_ = false, show_h_ = false;
  bool syncing_ = false, copy_enabled_ = true;
  ScrollBar vscroll_, hscroll_;
  Handler selection_changed_, activated_;
  const uint64_t* clock_ = nullptr;
  int32_t last_click_item_ = -1;
  uint64_t last_click_us_ = 0;
  String text_;  // scratch for painting

  int32_t resizing_ = -1;
  int32_t resize_origin_x_ = 0, resize_origin_width_ = 0;
};

}  // namespace gui
