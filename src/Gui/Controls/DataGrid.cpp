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

#include "Gui/Controls/DataGrid.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

namespace gui {

namespace {
// Digits with an optional sign and decimal fraction ("-12", "3.5", " 40 "), as millionths. False if it is not that.
bool parse_number(StringView s, int64_t* out) {
  uint64_t i = 0, n = s.size();
  while (i < n && s[i] == ' ') ++i;
  while (n > i && s[n - 1] == ' ') --n;
  bool negative = false;
  if (i < n && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
  int64_t whole = 0, fraction = 0, scale = 100000;
  uint32_t digits = 0, fraction_digits = 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    if (++digits > 12) return false;
    whole = whole * 10 + (s[i++] - '0');
  }
  if (i < n && s[i] == '.') {
    ++i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      ++fraction_digits;
      if (scale > 0) {
        fraction += (s[i] - '0') * scale;
        scale /= 10;
      }
      ++i;
    }
  }
  if (i != n || (digits == 0 && fraction_digits == 0)) return false;
  const int64_t value = whole * 1000000 + fraction;
  *out = negative ? -value : value;
  return true;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

int compare_text(StringView a, StringView b) {
  const uint64_t n = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < n; ++i) {
    const char x = lower(a[i]), y = lower(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}
}  // namespace

DataGrid::DataGrid(allocators::IAllocator& allocator)
    : Control(allocator),
      own_(allocator),
      model_(&own_),
      columns_(allocator),
      order_(allocator),
      vscroll_(allocator, Orientation::Vertical),
      hscroll_(allocator, Orientation::Horizontal),
      cell_a_(allocator),
      cell_b_(allocator) {
  vscroll_.on_value_changed(Handler::immediate(&DataGrid::vertical_changed, this));
  hscroll_.on_value_changed(Handler::immediate(&DataGrid::horizontal_changed, this));
  hscroll_.set_step(16);
  add(&vscroll_);
  add(&hscroll_);
  vscroll_.set_visible(false);
  hscroll_.set_visible(false);
}

void DataGrid::vertical_changed(void* self, Control&) { static_cast<DataGrid*>(self)->on_scroll_changed(); }
void DataGrid::horizontal_changed(void* self, Control&) { static_cast<DataGrid*>(self)->on_scroll_changed(); }

void DataGrid::on_scroll_changed() {
  if (syncing_) return;
  top_ = static_cast<uint32_t>(vscroll_.value());
  scroll_x_ = hscroll_.value();
  invalidate();
}

// --- Columns -------------------------------------------------------------------

int32_t DataGrid::add_column(StringView title, int32_t width, Align align) {
  UiGuard lock(this);
  if (model_ == &own_ && own_.row_count() > 0) return -1;
  Column column{String(allocator()), width < kMinColumnWidth ? kMinColumnWidth : width, align};
  if (!column.title.assign(title) || !columns_.push_back(util::move(column))) return -1;
  if (model_ == &own_) own_.set_column_count(static_cast<uint32_t>(columns_.count()));
  update_scroll_bars();
  invalidate();
  return static_cast<int32_t>(columns_.count()) - 1;
}

void DataGrid::set_column_width(uint32_t column, int32_t width) {
  UiGuard lock(this);
  if (column >= columns_.count()) return;
  columns_[column].width = width < kMinColumnWidth ? kMinColumnWidth : width;
  update_scroll_bars();
  invalidate();
}

int32_t DataGrid::total_width() const {
  int32_t sum = 0;
  for (const Column& c : columns_) sum += c.width;
  return sum;
}

int32_t DataGrid::column_left(uint32_t column) const {
  int32_t x = kBorder - scroll_x_;
  for (uint32_t i = 0; i < column && i < columns_.count(); ++i) x += columns_[i].width;
  return x;
}

// --- Rows ----------------------------------------------------------------------

void DataGrid::set_model(IGridModel* model) {
  UiGuard lock(this);
  model_ = model ? model : &own_;
  selected_ = -1;
  top_ = 0;
  rebuild_order();
}

void DataGrid::model_changed() {
  UiGuard lock(this);
  rebuild_order();
  if (selected_ >= static_cast<int32_t>(model_->row_count())) selected_ = -1;
}

int32_t DataGrid::add_row(const StringView* cells, uint32_t count) {
  UiGuard lock(this);
  if (model_ != &own_) return -1;
  const int32_t row = own_.add_row(cells, count);
  if (row < 0) return -1;
  if (sort_column_ < 0) order_.push_back(static_cast<uint32_t>(row));
  else rebuild_order();
  update_scroll_bars();
  invalidate();
  return row;
}

bool DataGrid::set_cell(uint32_t row, uint32_t column, StringView text) {
  UiGuard lock(this);
  if (model_ != &own_ || !own_.set_cell(row, column, text)) return false;
  if (static_cast<int32_t>(column) == sort_column_) rebuild_order();
  invalidate();
  return true;
}

void DataGrid::clear_rows() {
  UiGuard lock(this);
  if (model_ != &own_) return;
  own_.clear();
  order_.clear();
  top_ = 0;
  const bool had_selection = selected_ >= 0;
  selected_ = -1;
  update_scroll_bars();
  invalidate();
  if (had_selection) fire(selection_changed_);
}

// Compares two rows by the sort column. Numbers go by value, anything else by text without regard to case.
bool DataGrid::row_less(uint32_t a, uint32_t b) {
  const uint32_t column = static_cast<uint32_t>(sort_column_);
  model_->cell_text(a, column, &cell_a_);
  model_->cell_text(b, column, &cell_b_);
  int64_t x, y;
  int order;
  if (parse_number(cell_a_.view(), &x) && parse_number(cell_b_.view(), &y)) order = x < y ? -1 : x > y ? 1 : 0;
  else order = compare_text(cell_a_.view(), cell_b_.view());
  return ascending_ ? order < 0 : order > 0;
}

// The rows in the order shown: the model's, or sorted by the sort column (a stable merge sort).
void DataGrid::rebuild_order() {
  const uint32_t n = model_->row_count();
  order_.clear();
  order_.reserve(n);
  for (uint32_t i = 0; i < n; ++i) order_.push_back(i);
  if (sort_column_ >= 0 && static_cast<uint32_t>(sort_column_) < columns_.count() && n > 1) {
    ArrayView<uint32_t> a = allocate_view<uint32_t>(allocator(), n), b = allocate_view<uint32_t>(allocator(), n);
    if (!a.empty() && !b.empty()) {
      for (uint32_t i = 0; i < n; ++i) a[i] = i;
      for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t start = 0; start < n; start += 2 * width) {
          const uint32_t mid = start + width < n ? start + width : n, end = start + 2 * width < n ? start + 2 * width : n;
          uint32_t l = start, r = mid, out = start;
          while (l < mid && r < end) b[out++] = row_less(a[r], a[l]) ? a[r++] : a[l++];  // the right one only if strictly less
          while (l < mid) b[out++] = a[l++];
          while (r < end) b[out++] = a[r++];
        }
        ArrayView<uint32_t> t = a;
        a = b;
        b = t;
      }
      for (uint32_t i = 0; i < n; ++i) order_[i] = a[i];
    }
    release_view(allocator(), a);
    release_view(allocator(), b);
  }
  update_scroll_bars();
  invalidate();
}

bool DataGrid::sort_by(int32_t column, bool ascending) {
  UiGuard lock(this);
  if (column < -1 || column >= static_cast<int32_t>(columns_.count())) return false;
  sort_column_ = column;
  ascending_ = ascending;
  rebuild_order();
  if (selected_ >= 0) ensure_visible(static_cast<uint32_t>(position_of_row(selected_) < 0 ? 0 : position_of_row(selected_)));
  fire(sort_changed_);
  return true;
}

void DataGrid::toggle_sort(int32_t column) {
  if (column == sort_column_) sort_by(column, !ascending_);
  else sort_by(column, true);
}

// --- Selection and scrolling ----------------------------------------------------

int32_t DataGrid::position_of_row(int32_t row) const {
  for (uint32_t i = 0; i < order_.count(); ++i)
    if (static_cast<int32_t>(order_[i]) == row) return static_cast<int32_t>(i);
  return -1;
}

void DataGrid::select_row(int32_t row) {
  UiGuard lock(this);
  if (row < -1 || row >= static_cast<int32_t>(model_->row_count())) return;
  if (row >= 0) ensure_visible(static_cast<uint32_t>(position_of_row(row)));
  if (row == selected_) return;
  selected_ = row;
  invalidate();
  fire(selection_changed_);
}

void DataGrid::move_selection_to(int32_t position) {
  const int32_t n = static_cast<int32_t>(order_.count());
  if (n == 0) return;
  if (position < 0) position = 0;
  if (position >= n) position = n - 1;
  select_row(static_cast<int32_t>(order_[position]));
}

void DataGrid::ensure_visible(uint32_t position) {
  const uint32_t visible = visible_row_count();
  if (visible == 0) return;
  if (position < top_) top_ = position;
  else if (position >= top_ + visible) top_ = position + 1 - visible;
  update_scroll_bars();
  invalidate();
}

Rect2D DataGrid::header_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (show_v_ ? ScrollBar::kThickness : 0);
  return {kBorder, kBorder, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(kHeaderHeight)};
}

Rect2D DataGrid::body_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (show_v_ ? ScrollBar::kThickness : 0);
  const int32_t h = static_cast<int32_t>(bounds_.height) - 2 * kBorder - kHeaderHeight - (show_h_ ? ScrollBar::kThickness : 0);
  return {kBorder, kBorder + kHeaderHeight, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

uint32_t DataGrid::visible_row_count() const { return body_rect().height / kRowHeight; }

// Works out which scroll bars are needed, keeps the scroll position inside the content and tells the bars.
void DataGrid::update_scroll_bars() {
  const int32_t rows = static_cast<int32_t>(order_.count());
  const int32_t content_w = total_width();
  const int32_t avail_w = static_cast<int32_t>(bounds_.width) - 2 * kBorder;
  const int32_t avail_h = static_cast<int32_t>(bounds_.height) - 2 * kBorder - kHeaderHeight;
  bool v = rows * kRowHeight > avail_h;
  bool h = content_w > avail_w - (v ? ScrollBar::kThickness : 0);
  if (h && !v) v = rows * kRowHeight > avail_h - ScrollBar::kThickness;
  show_v_ = v;
  show_h_ = h;
  const Rect2D body = body_rect();
  const uint32_t visible = body.height / kRowHeight;
  const uint32_t max_top = static_cast<uint32_t>(rows) > visible ? static_cast<uint32_t>(rows) - visible : 0;
  if (top_ > max_top) top_ = max_top;
  const int32_t max_x = content_w > static_cast<int32_t>(body.width) ? content_w - static_cast<int32_t>(body.width) : 0;
  if (scroll_x_ > max_x) scroll_x_ = max_x;
  if (scroll_x_ < 0) scroll_x_ = 0;
  syncing_ = true;
  vscroll_.set_metrics(rows, static_cast<int32_t>(visible));
  vscroll_.set_value(static_cast<int32_t>(top_));
  hscroll_.set_metrics(content_w, static_cast<int32_t>(body.width));
  hscroll_.set_value(scroll_x_);
  syncing_ = false;
  vscroll_.set_visible(show_v_);
  hscroll_.set_visible(show_h_);
}

void DataGrid::layout() {
  update_scroll_bars();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t thick = ScrollBar::kThickness;
  vscroll_.set_bounds({w - kBorder - thick, kBorder + kHeaderHeight, static_cast<uint32_t>(thick),
                       static_cast<uint32_t>(h - 2 * kBorder - kHeaderHeight - (show_h_ ? thick : 0) > 0 ? h - 2 * kBorder - kHeaderHeight - (show_h_ ? thick : 0) : 0)});
  hscroll_.set_bounds({kBorder, h - kBorder - thick, static_cast<uint32_t>(w - 2 * kBorder - (show_v_ ? thick : 0) > 0 ? w - 2 * kBorder - (show_v_ ? thick : 0) : 0),
                       static_cast<uint32_t>(thick)});
  Control::layout();
}

// --- Painting ------------------------------------------------------------------

void DataGrid::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const uint32_t text_color = enabled() ? t.text : t.disabled_text;
  const uint32_t alternate = (t.field & 0xFFFFFF) == 0xFFFFFF ? 0xFFF2F2F2u : t.field;

  // Header.
  const Rect2D header = header_rect();
  painter.push_frame(header);
  for (uint32_t c = 0; c < columns_.count(); ++c) {
    const int32_t x = column_left(c) - kBorder, w = columns_[c].width;
    if (x + w < 0 || x > static_cast<int32_t>(header.width)) continue;
    const Rect2D cell{x, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(kHeaderHeight)};
    painter.fill(cell, t.face);
    painter.bevel(cell, header_pressed_ == static_cast<int32_t>(c));
    painter.push_frame(cell);
    const StringView title = columns_[c].title.view();
    const int32_t room = w - 8 - (sort_column_ == static_cast<int32_t>(c) ? 12 : 0);
    int32_t tx = 4;
    if (columns_[c].align == Align::Right) tx = 4 + (room > Painter::text_width(title) ? room - Painter::text_width(title) : 0);
    else if (columns_[c].align == Align::Center) tx = 4 + (room > Painter::text_width(title) ? (room - Painter::text_width(title)) / 2 : 0);
    painter.text(tx, (kHeaderHeight - Painter::kGlyphHeight) / 2, title, text_color);
    if (sort_column_ == static_cast<int32_t>(c)) {  // a small triangle: up for ascending
      const int32_t cx = w - 10, cy = kHeaderHeight / 2;
      for (int32_t i = 0; i < 4; ++i) {
        const int32_t half = ascending_ ? i : 3 - i;
        painter.line(cx - half, cy - 2 + i, cx + half, cy - 2 + i, text_color);
      }
    }
    painter.pop_frame();
  }
  painter.pop_frame();

  // Rows.
  const Rect2D body = body_rect();
  painter.push_frame(body);
  const uint32_t visible = (body.height + kRowHeight - 1) / kRowHeight;  // the last may be partly visible
  const int32_t full_width = total_width() > static_cast<int32_t>(body.width) ? total_width() : static_cast<int32_t>(body.width);
  for (uint32_t i = 0; i < visible && top_ + i < order_.count(); ++i) {
    const uint32_t position = top_ + i;
    const uint32_t row = order_[position];
    const int32_t y = static_cast<int32_t>(i) * kRowHeight;
    const bool selected = static_cast<int32_t>(row) == selected_;
    const uint32_t background = selected ? (focused() ? t.selection : t.shadow) : (position % 2 ? alternate : t.field);
    painter.fill({-scroll_x_, y, static_cast<uint32_t>(full_width), static_cast<uint32_t>(kRowHeight)}, background);
    for (uint32_t c = 0; c < columns_.count(); ++c) {
      const int32_t x = column_left(c) - kBorder, w = columns_[c].width;
      if (x + w < 0 || x > static_cast<int32_t>(body.width)) continue;
      model_->cell_text(row, c, &cell_a_);
      const StringView text = cell_a_.view();
      painter.push_frame({x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(kRowHeight)});
      int32_t tx = 4;
      const int32_t room = w - 8;
      if (columns_[c].align == Align::Right) tx = 4 + (room > Painter::text_width(text) ? room - Painter::text_width(text) : 0);
      else if (columns_[c].align == Align::Center) tx = 4 + (room > Painter::text_width(text) ? (room - Painter::text_width(text)) / 2 : 0);
      painter.text(tx, (kRowHeight - Painter::kGlyphHeight) / 2, text, selected ? t.selection_text : text_color);
      painter.pop_frame();
    }
    if (selected && focused()) painter.focus_frame({-scroll_x_, y, static_cast<uint32_t>(full_width), static_cast<uint32_t>(kRowHeight)});
  }
  // Lines between the columns.
  for (uint32_t c = 0; c < columns_.count(); ++c) {
    const int32_t x = column_left(c) - kBorder + columns_[c].width - 1;
    if (x >= 0 && x < static_cast<int32_t>(body.width)) painter.line(x, 0, x, static_cast<int32_t>(body.height) - 1, t.face);
  }
  painter.pop_frame();

  // The corner of the header above the vertical scroll bar, and the square where the two bars meet.
  if (show_v_)
    painter.fill({static_cast<int32_t>(bounds_.width) - kBorder - ScrollBar::kThickness, kBorder, static_cast<uint32_t>(ScrollBar::kThickness), static_cast<uint32_t>(kHeaderHeight)}, t.face);
  if (show_v_ && show_h_)
    painter.fill({static_cast<int32_t>(bounds_.width) - kBorder - ScrollBar::kThickness, static_cast<int32_t>(bounds_.height) - kBorder - ScrollBar::kThickness,
                  static_cast<uint32_t>(ScrollBar::kThickness), static_cast<uint32_t>(ScrollBar::kThickness)}, t.face);
}

// --- Input ---------------------------------------------------------------------

int32_t DataGrid::column_at(int32_t x, bool* on_border) const {
  *on_border = false;
  for (uint32_t c = 0; c < columns_.count(); ++c) {
    const int32_t left = column_left(c), right = left + columns_[c].width;
    if (x >= right - kResizeZone && x < right + kResizeZone) {
      *on_border = true;
      return static_cast<int32_t>(c);
    }
    if (x >= left && x < right) return static_cast<int32_t>(c);
  }
  return -1;
}

void DataGrid::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const Rect2D header = header_rect(), body = body_rect();
  if (contains(header, args.x, args.y)) {
    bool border = false;
    const int32_t column = column_at(args.x, &border);
    if (column < 0) return;
    if (border) {
      resizing_ = column;
      resize_origin_x_ = args.x;
      resize_origin_width_ = columns_[column].width;
    } else {
      header_pressed_ = column;
    }
    invalidate();
  } else if (contains(body, args.x, args.y)) {
    const uint32_t position = top_ + static_cast<uint32_t>((args.y - body.y) / kRowHeight);
    if (position < order_.count()) select_row(static_cast<int32_t>(order_[position]));
  }
}

void DataGrid::on_mouse_move(const MouseArgs& args) {
  if (resizing_ >= 0) set_column_width(static_cast<uint32_t>(resizing_), resize_origin_width_ + args.x - resize_origin_x_);
}

void DataGrid::on_mouse_up(const MouseArgs& args) {
  const int32_t pressed = header_pressed_;
  header_pressed_ = -1;
  resizing_ = -1;
  invalidate();
  if (pressed < 0 || !contains(header_rect(), args.x, args.y)) return;
  bool border = false;
  if (column_at(args.x, &border) == pressed) toggle_sort(pressed);
}

void DataGrid::on_mouse_wheel(const MouseArgs& args) {
  const int32_t rows = static_cast<int32_t>(order_.count());
  int32_t top = static_cast<int32_t>(top_) + (args.wheel > 0 ? -3 : args.wheel < 0 ? 3 : 0);
  if (top < 0) top = 0;
  top_ = static_cast<uint32_t>(top > rows ? rows : top);
  update_scroll_bars();
  invalidate();
}

bool DataGrid::on_key(const KeyArgs& args) {
  using namespace hardware;
  const int32_t n = static_cast<int32_t>(order_.count());
  const int32_t current = selected_ >= 0 ? position_of_row(selected_) : -1;
  const int32_t page = static_cast<int32_t>(visible_row_count()) > 1 ? static_cast<int32_t>(visible_row_count()) - 1 : 1;
  switch (args.usage) {
    case key::kDown: move_selection_to(current < 0 ? 0 : current + 1); return true;
    case key::kUp: move_selection_to(current < 0 ? n - 1 : current - 1); return true;
    case 0x4E: move_selection_to(current < 0 ? 0 : current + page); return true;  // Page Down
    case 0x4B: move_selection_to(current < 0 ? n - 1 : current - page); return true;  // Page Up
    case 0x4A: move_selection_to(0); return true;      // Home
    case 0x4D: move_selection_to(n - 1); return true;  // End
    case key::kEnter:
      if (selected_ >= 0) fire(activated_);
      return true;
    case key::kLeft:
    case key::kRight: {
      const int32_t step = args.usage == key::kRight ? 16 : -16;
      scroll_x_ = scroll_x_ + step < 0 ? 0 : scroll_x_ + step;
      update_scroll_bars();
      invalidate();
      return true;
    }
    default:
      return false;
  }
}

}  // namespace gui
