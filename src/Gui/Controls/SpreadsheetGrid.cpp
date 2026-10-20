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

#include "Gui/Controls/SpreadsheetGrid.hpp"
#include "Gui/Painter.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr uint32_t kGridLine = 0xFFD0D0D0, kSelectedCell = 0xFFDCE6F7, kSelectedHeader = 0xFFA8B8D8, kActiveFrame = 0xFF000000;

uint32_t digits_of(uint32_t n) {
  uint32_t d = 1;
  while (n >= 10) {
    n /= 10;
    ++d;
  }
  return d;
}
}  // namespace

SpreadsheetGrid::SpreadsheetGrid(allocators::IAllocator& allocator)
    : Control(allocator),
      widths_(allocator),
      vscroll_(allocator, Orientation::Vertical),
      hscroll_(allocator, Orientation::Horizontal),
      edit_text_(allocator),
      cell_text_(allocator) {
  vscroll_.on_value_changed(Handler::immediate(&SpreadsheetGrid::scrolled, this));
  hscroll_.on_value_changed(Handler::immediate(&SpreadsheetGrid::scrolled, this));
  add(&vscroll_);
  add(&hscroll_);
  vscroll_.set_visible(false);
  hscroll_.set_visible(false);
}

void SpreadsheetGrid::scrolled(void* self, Control&) { static_cast<SpreadsheetGrid*>(self)->on_scroll_changed(); }

void SpreadsheetGrid::on_scroll_changed() {
  if (syncing_) return;
  first_row_ = static_cast<uint32_t>(vscroll_.value());
  first_column_ = static_cast<uint32_t>(hscroll_.value());
  invalidate();
}

void SpreadsheetGrid::column_title(uint32_t column, String* out) {
  out->clear();
  if (column >= 26) out->push_back(static_cast<char>('A' + column / 26 - 1));
  out->push_back(static_cast<char>('A' + column % 26));
}

void SpreadsheetGrid::set_model(ISheetView* model) {
  UiGuard lock(this);
  model_ = model;
  model_changed();
}

void SpreadsheetGrid::model_changed() {
  UiGuard lock(this);
  const uint32_t c = columns(), r = rows();
  if (active_column_ >= c) active_column_ = c ? c - 1 : 0;
  if (anchor_column_ >= c) anchor_column_ = c ? c - 1 : 0;
  if (active_row_ >= r) active_row_ = r ? r - 1 : 0;
  if (anchor_row_ >= r) anchor_row_ = r ? r - 1 : 0;
  if (first_column_ >= c) first_column_ = c ? c - 1 : 0;
  if (first_row_ >= r) first_row_ = r ? r - 1 : 0;
  update_scroll_bars();
  invalidate();
}

// --- Selection ----------------------------------------------------------------------------------------------------------------------------------------

void SpreadsheetGrid::notify_selection() {
  invalidate();
  if (selection_changed_) fire(selection_changed_);
}

void SpreadsheetGrid::select(uint32_t column, uint32_t row, bool extend) {
  UiGuard lock(this);
  if (!columns() || !rows()) return;
  if (column >= columns()) column = columns() - 1;
  if (row >= rows()) row = rows() - 1;
  const bool same = column == active_column_ && row == active_row_ && (extend || single_cell_selected());
  active_column_ = column;
  active_row_ = row;
  if (!extend) {
    anchor_column_ = column;
    anchor_row_ = row;
  }
  ensure_visible(column, row);
  if (!same) notify_selection();
}

void SpreadsheetGrid::select_all() {
  UiGuard lock(this);
  if (!columns() || !rows()) return;
  anchor_column_ = 0;
  anchor_row_ = 0;
  active_column_ = columns() - 1;
  active_row_ = rows() - 1;
  notify_selection();
}

void SpreadsheetGrid::move(int32_t dc, int32_t dr, bool extend) {
  const int64_t c = static_cast<int64_t>(active_column_) + dc, r = static_cast<int64_t>(active_row_) + dr;
  select(c < 0 ? 0 : static_cast<uint32_t>(c), r < 0 ? 0 : static_cast<uint32_t>(r), extend);
}

void SpreadsheetGrid::set_edit_text(StringView text) {
  UiGuard lock(this);
  editing_ = true;
  edit_text_.assign(text);
  invalidate();
}

void SpreadsheetGrid::stop_editing() {
  UiGuard lock(this);
  if (!editing_) return;
  editing_ = false;
  invalidate();
}

void SpreadsheetGrid::set_column_width(uint32_t column, int32_t width) {
  UiGuard lock(this);
  if (column >= columns()) return;
  if (width < kMinColumnWidth) width = kMinColumnWidth;
  while (widths_.count() <= column)
    if (!widths_.push_back(kDefaultColumnWidth)) return;
  widths_[column] = width;
  update_scroll_bars();
  invalidate();
}

// --- Geometry -----------------------------------------------------------------------------------------------------------------------------------------

int32_t SpreadsheetGrid::row_header_width() const { return static_cast<int32_t>(digits_of(rows())) * Painter::kGlyphWidth + 12; }

Rect2D SpreadsheetGrid::body_rect() const {
  const int32_t x = kBorder + row_header_width(), y = kBorder + kHeaderHeight;
  const int32_t w = static_cast<int32_t>(bounds_.width) - kBorder - x - (show_v_ ? ScrollBar::kThickness : 0);
  const int32_t h = static_cast<int32_t>(bounds_.height) - kBorder - y - (show_h_ ? ScrollBar::kThickness : 0);
  return {x, y, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

uint32_t SpreadsheetGrid::visible_row_count() const { return body_rect().height / kRowHeight; }

uint32_t SpreadsheetGrid::visible_column_count() const {
  const Rect2D body = body_rect();
  int32_t x = 0;
  uint32_t n = 0;
  for (uint32_t c = first_column_; c < columns(); ++c) {
    x += column_width(c);
    if (x > static_cast<int32_t>(body.width)) break;
    ++n;
  }
  return n ? n : 1;
}

int32_t SpreadsheetGrid::total_width() const {
  int32_t w = 0;
  for (uint32_t c = 0; c < columns(); ++c) w += column_width(c);
  return w;
}

Rect2D SpreadsheetGrid::cell_rect(uint32_t column, uint32_t row) const {
  const Rect2D body = body_rect();
  int32_t x = body.x;
  if (column >= first_column_) {
    for (uint32_t c = first_column_; c < column; ++c) x += column_width(c);
  } else {
    for (uint32_t c = column; c < first_column_; ++c) x -= column_width(c);
  }
  const int32_t y = body.y + (static_cast<int32_t>(row) - static_cast<int32_t>(first_row_)) * kRowHeight;
  return {x, y, static_cast<uint32_t>(column_width(column)), static_cast<uint32_t>(kRowHeight)};
}

bool SpreadsheetGrid::cell_at(int32_t x, int32_t y, uint32_t* column, uint32_t* row) const {
  const Rect2D body = body_rect();
  if (!contains(body, x, y)) return false;
  const uint32_t r = first_row_ + static_cast<uint32_t>((y - body.y) / kRowHeight);
  int32_t left = body.x;
  for (uint32_t c = first_column_; c < columns(); ++c) {
    const int32_t w = column_width(c);
    if (x < left + w) {
      if (r >= rows()) return false;
      *column = c;
      *row = r;
      return true;
    }
    left += w;
  }
  return false;
}

// The column whose letter is at x in the header, and whether x is on the border at its right edge (to resize it).
int32_t SpreadsheetGrid::column_at_x(int32_t x, bool* on_border) const {
  const Rect2D body = body_rect();
  *on_border = false;
  int32_t left = body.x;
  for (uint32_t c = first_column_; c < columns(); ++c) {
    const int32_t right = left + column_width(c);
    if (x >= left && x < right + 1) {
      *on_border = x >= right - kResizeZone;
      if (x >= right + 1) break;
      return static_cast<int32_t>(c);
    }
    left = right;
    if (left > static_cast<int32_t>(body.x + body.width)) break;
  }
  return -1;
}

void SpreadsheetGrid::update_scroll_bars() {
  const int32_t r = static_cast<int32_t>(rows()), c = static_cast<int32_t>(columns());
  const int32_t avail_w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - row_header_width();
  const int32_t avail_h = static_cast<int32_t>(bounds_.height) - 2 * kBorder - kHeaderHeight;
  bool v = r * kRowHeight > avail_h;
  bool h = total_width() > avail_w - (v ? ScrollBar::kThickness : 0);
  if (h && !v) v = r * kRowHeight > avail_h - ScrollBar::kThickness;
  show_v_ = v;
  show_h_ = h;
  const int32_t rows_page = static_cast<int32_t>(visible_row_count());
  const int32_t columns_page = static_cast<int32_t>(visible_column_count());
  const uint32_t max_row = r > rows_page ? static_cast<uint32_t>(r - rows_page) : 0;
  const uint32_t max_column = c > columns_page ? static_cast<uint32_t>(c - columns_page) : 0;
  if (first_row_ > max_row) first_row_ = max_row;
  if (first_column_ > max_column) first_column_ = max_column;
  syncing_ = true;
  vscroll_.set_metrics(r, rows_page);
  vscroll_.set_value(static_cast<int32_t>(first_row_));
  hscroll_.set_metrics(c, columns_page);
  hscroll_.set_value(static_cast<int32_t>(first_column_));
  syncing_ = false;
  vscroll_.set_visible(show_v_);
  hscroll_.set_visible(show_h_);
}

void SpreadsheetGrid::ensure_visible(uint32_t column, uint32_t row) {
  const uint32_t rows_page = visible_row_count() ? visible_row_count() : 1;
  if (row < first_row_) first_row_ = row;
  else if (row >= first_row_ + rows_page) first_row_ = row - rows_page + 1;
  if (column < first_column_) {
    first_column_ = column;
  } else {   // far enough to the left that the column's right edge is inside the body
    const int32_t room = static_cast<int32_t>(body_rect().width);
    while (first_column_ < column) {
      int32_t used = 0;
      for (uint32_t c = first_column_; c <= column; ++c) used += column_width(c);
      if (used <= room) break;
      ++first_column_;
    }
  }
  update_scroll_bars();
}

void SpreadsheetGrid::layout() {
  update_scroll_bars();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t thick = ScrollBar::kThickness;
  const Rect2D body = body_rect();
  place(vscroll_, {w - kBorder - thick, body.y, static_cast<uint32_t>(thick), body.height});
  place(hscroll_, {body.x, h - kBorder - thick, body.width, static_cast<uint32_t>(thick)});
  Control::layout();
}

// --- Painting -----------------------------------------------------------------------------------------------------------------------------------------

void SpreadsheetGrid::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  const Rect2D body = body_rect();
  const uint32_t left = selection_left(), right = selection_right(), top = selection_top(), bottom = selection_bottom();
  String title(allocator());

  // Corner and column letters.
  const Rect2D corner{kBorder, kBorder, static_cast<uint32_t>(row_header_width()), static_cast<uint32_t>(kHeaderHeight)};
  painter.fill(corner, t.face);
  painter.bevel(corner, false);
  painter.push_frame({body.x, kBorder, body.width, static_cast<uint32_t>(kHeaderHeight)});
  {
    int32_t x = 0;
    for (uint32_t c = first_column_; c < columns() && x < static_cast<int32_t>(body.width); ++c) {
      const int32_t w = column_width(c);
      const bool selected = c >= left && c <= right;
      const Rect2D cell{x, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(kHeaderHeight)};
      painter.fill(cell, selected ? kSelectedHeader : t.face);
      painter.bevel(cell, selected);
      column_title(c, &title);
      painter.push_frame(cell);
      painter.text_centered({0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(kHeaderHeight)}, title.view(), ink);
      painter.pop_frame();
      x += w;
    }
  }
  painter.pop_frame();

  // Row numbers.
  painter.push_frame({kBorder, body.y, static_cast<uint32_t>(row_header_width()), body.height});
  for (uint32_t r = first_row_, y = 0; r < rows() && y < body.height; ++r, y += kRowHeight) {
    const bool selected = r >= top && r <= bottom;
    const Rect2D cell{0, static_cast<int32_t>(y), static_cast<uint32_t>(row_header_width()), static_cast<uint32_t>(kRowHeight)};
    painter.fill(cell, selected ? kSelectedHeader : t.face);
    painter.bevel(cell, selected);
    title.clear();
    title.append_uint(r + 1);
    painter.text_centered(cell, title.view(), ink);
  }
  painter.pop_frame();

  // Cells.
  painter.push_frame(body);
  bool right_aligned = false;
  for (uint32_t r = first_row_, y = 0; r < rows() && y < body.height; ++r, y += kRowHeight) {
    int32_t x = 0;
    for (uint32_t c = first_column_; c < columns() && x < static_cast<int32_t>(body.width); ++c) {
      const int32_t w = column_width(c);
      const Rect2D cell{x, static_cast<int32_t>(y), static_cast<uint32_t>(w), static_cast<uint32_t>(kRowHeight)};
      const bool in_selection = c >= left && c <= right && r >= top && r <= bottom;
      const bool active = c == active_column_ && r == active_row_;
      painter.fill(cell, in_selection && !active && !single_cell_selected() ? kSelectedCell : t.field);
      painter.fill({x, static_cast<int32_t>(y) + kRowHeight - 1, static_cast<uint32_t>(w), 1}, kGridLine);
      painter.fill({x + w - 1, static_cast<int32_t>(y), 1, static_cast<uint32_t>(kRowHeight)}, kGridLine);
      const bool editing_here = editing_ && active;
      if (editing_here) {
        cell_text_.assign(edit_text_.view());
        right_aligned = false;
      } else if (!model_ || !model_->cell_text(c, r, &cell_text_, &right_aligned)) {
        cell_text_.clear();
      }
      if (!cell_text_.empty()) {
        painter.push_frame({x + 1, static_cast<int32_t>(y), static_cast<uint32_t>(w > 2 ? w - 2 : 0), static_cast<uint32_t>(kRowHeight - 1)});
        const int32_t tw = Painter::text_width(cell_text_.view());
        int32_t tx = 3;
        if (editing_here && tw > w - 8) tx = w - 8 - tw;   // the end of long text, where it is being typed
        else if (right_aligned && tw < w - 8) tx = w - 5 - tw;
        painter.text(tx, (kRowHeight - Painter::kGlyphHeight) / 2, cell_text_.view(), ink);
        painter.pop_frame();
      }
      x += w;
    }
  }
  // The active cell's frame.
  if (columns() && rows() && active_row_ >= first_row_ && active_column_ >= first_column_) {
    const Rect2D a = cell_rect(active_column_, active_row_);
    const Rect2D r{a.x - body.x - 1, a.y - body.y - 1, a.width + 1, a.height + 1};
    painter.outline(r, kActiveFrame);
    painter.outline({r.x + 1, r.y + 1, r.width > 2 ? r.width - 2 : 0, r.height > 2 ? r.height - 2 : 0}, kActiveFrame);
  }
  painter.pop_frame();
}

// --- Mouse --------------------------------------------------------------------------------------------------------------------------------------------

void SpreadsheetGrid::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left || !columns() || !rows()) return;
  const Rect2D body = body_rect();
  uint32_t column, row;
  if (args.y >= kBorder && args.y < kBorder + kHeaderHeight) {   // the column letters (and the corner)
    if (args.x < body.x) {
      select_all();
      return;
    }
    bool border = false;
    const int32_t c = column_at_x(args.x, &border);
    if (c < 0) return;
    if (border) {
      drag_ = Drag::Resize;
      resize_column_ = c;
      resize_origin_x_ = args.x;
      resize_origin_width_ = column_width(static_cast<uint32_t>(c));
      return;
    }
    anchor_column_ = static_cast<uint32_t>(c);
    anchor_row_ = 0;
    active_column_ = static_cast<uint32_t>(c);
    active_row_ = rows() - 1;
    drag_ = Drag::Columns;
    notify_selection();
  } else if (args.x >= kBorder && args.x < body.x && args.y >= body.y) {   // the row numbers
    const uint32_t r = first_row_ + static_cast<uint32_t>((args.y - body.y) / kRowHeight);
    if (r >= rows()) return;
    anchor_row_ = r;
    anchor_column_ = 0;
    active_row_ = r;
    active_column_ = columns() - 1;
    drag_ = Drag::Rows;
    notify_selection();
  } else if (cell_at(args.x, args.y, &column, &row)) {
    drag_ = Drag::Cells;
    select(column, row, false);
  }
}

void SpreadsheetGrid::on_mouse_move(const MouseArgs& args) {
  if (drag_ == Drag::None) return;
  const Rect2D body = body_rect();
  if (drag_ == Drag::Resize) {
    set_column_width(static_cast<uint32_t>(resize_column_), resize_origin_width_ + args.x - resize_origin_x_);
    return;
  }
  // Out of the body the pointer still counts: the nearest cell, so a drag past the edge reaches the cells beyond (and scrolls to them).
  int32_t x = args.x < body.x ? body.x : args.x;
  int32_t y = args.y < body.y ? body.y : args.y;
  const int32_t max_x = body.x + static_cast<int32_t>(body.width) - 1, max_y = body.y + static_cast<int32_t>(body.height) - 1;
  if (x > max_x) x = max_x;
  if (y > max_y) y = max_y;
  uint32_t column, row;
  const bool found = cell_at(x, y, &column, &row);
  if (!found) {
    column = active_column_;
    row = active_row_;
  }
  const int32_t margin = 2;
  if (args.x > max_x + margin && active_column_ + 1 < columns()) column = active_column_ + 1;
  else if (args.x < body.x - margin && active_column_ > 0) column = active_column_ - 1;
  if (args.y > max_y + margin && active_row_ + 1 < rows()) row = active_row_ + 1;
  else if (args.y < body.y - margin && active_row_ > 0) row = active_row_ - 1;
  if (drag_ == Drag::Columns) {
    row = rows() - 1;
  } else if (drag_ == Drag::Rows) {
    column = columns() - 1;
  }
  select(column, row, true);
}

void SpreadsheetGrid::on_mouse_up(const MouseArgs&) { drag_ = Drag::None; }

void SpreadsheetGrid::on_mouse_wheel(const MouseArgs& args) {
  int64_t top = static_cast<int64_t>(first_row_) + (args.wheel > 0 ? -3 : args.wheel < 0 ? 3 : 0);
  if (top < 0) top = 0;
  first_row_ = static_cast<uint32_t>(top);
  update_scroll_bars();
  invalidate();
}

// --- Keys ---------------------------------------------------------------------------------------------------------------------------------------------

bool SpreadsheetGrid::on_key(const KeyArgs& args) {
  using namespace hardware;
  const bool shift = args.modifiers & modifier::kShift, control = args.modifiers & modifier::kControl;
  const int32_t page = visible_row_count() > 1 ? static_cast<int32_t>(visible_row_count()) - 1 : 1;
  if (control) {
    switch (args.usage) {
      case key::kA: select_all(); return true;
      case key::kA + 2: if (copy_requested_) fire(copy_requested_); return true;    // C
      case key::kA + 23: if (cut_requested_) fire(cut_requested_); return true;     // X
      case key::kA + 21: if (paste_requested_) fire(paste_requested_); return true; // V
      case 0x4A: select(0, 0, shift); return true;                                  // Ctrl+Home
      default: break;
    }
  }
  switch (args.usage) {
    case key::kRight: move(1, 0, shift); return true;
    case key::kLeft: move(-1, 0, shift); return true;
    case key::kDown: move(0, 1, shift); return true;
    case key::kUp: move(0, -1, shift); return true;
    case key::kTab: move(shift ? -1 : 1, 0, false); return true;
    case key::kEnter: move(0, shift ? -1 : 1, false); return true;
    case 0x4A: select(0, active_row_, shift); return true;                          // Home
    case 0x4D: select(columns() ? columns() - 1 : 0, active_row_, shift); return true;  // End
    case 0x4E: move(0, page, shift); return true;                                   // Page Down
    case 0x4B: move(0, -page, shift); return true;                                  // Page Up
    case key::kDelete:
    case key::kBackspace:
      if (clear_requested_) fire(clear_requested_);
      return true;
    case key::kF1 + 1:   // F2
      request_char_ = 0;
      if (edit_requested_) fire(edit_requested_);
      return true;
    default: break;
  }
  if (!control && args.character >= 32 && args.character < 127) {   // typing starts an edit
    request_char_ = args.character;
    if (edit_requested_) fire(edit_requested_);
    return true;
  }
  return false;
}

}  // namespace gui
