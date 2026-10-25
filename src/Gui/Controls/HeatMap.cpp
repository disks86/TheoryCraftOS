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

#include "Gui/Controls/HeatMap.hpp"
#include "Gui/Color.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/String.hpp"

namespace gui {

namespace {

constexpr uint32_t kRamp[] = {0xFF2C4DA8, 0xFF3FB5A5, 0xFFF2D14B, 0xFFD7301F};  // blue, teal, yellow, red
constexpr uint32_t kRampSegments = sizeof(kRamp) / sizeof(kRamp[0]) - 1;

// value * scale / top for value <= top, without overflow: both are halved until the product fits.
uint32_t share(uint64_t value, uint64_t top, uint32_t scale) {
  if (value >= top) return scale;
  while (top > UINT64_MAX / scale) {
    top >>= 1;
    value >>= 1;
  }
  return top ? static_cast<uint32_t>(value * scale / top) : 0;
}

uint8_t mix(uint8_t a, uint8_t b, uint32_t t) {  // t 0..1000
  return static_cast<uint8_t>((uint32_t{a} * (1000 - t) + uint32_t{b} * t) / 1000);
}

}  // namespace

bool HeatMap::set_dimensions(int32_t rows, int32_t columns) {
  UiGuard lock(this);
  row_labels_.clear();
  column_labels_.clear();
  values_.clear();
  selected_row_ = selected_column_ = -1;
  invalidate();
  if (rows < 0 || columns < 0 || rows > kMaxDimension || columns > kMaxDimension || int64_t{rows} * columns > kMaxCells) return false;
  bool ok = true;
  for (int32_t r = 0; ok && r < rows; ++r) ok = row_labels_.add("") >= 0;
  for (int32_t c = 0; ok && c < columns; ++c) ok = column_labels_.add("") >= 0;
  const int64_t cells = rows && columns ? int64_t{rows} * columns : 0;
  ok = ok && values_.reserve(static_cast<uint64_t>(cells));
  for (int64_t i = 0; ok && i < cells; ++i) ok = values_.push_back(0);
  if (!ok) {
    row_labels_.clear();
    column_labels_.clear();
    values_.clear();
  }
  return ok;
}

void HeatMap::set_row_label(int32_t row, StringView label) {
  UiGuard lock(this);
  if (row_labels_.set(row, label)) invalidate();
}

void HeatMap::set_column_label(int32_t column, StringView label) {
  UiGuard lock(this);
  if (column_labels_.set(column, label)) invalidate();
}

uint64_t HeatMap::value(int32_t row, int32_t column) const {
  if (row < 0 || column < 0 || row >= row_count() || column >= column_count()) return 0;
  const uint64_t* v = values_.get(static_cast<uint64_t>(row) * static_cast<uint64_t>(column_count()) + static_cast<uint64_t>(column));
  return v ? *v : 0;
}

void HeatMap::set_value(int32_t row, int32_t column, uint64_t value) {
  UiGuard lock(this);
  if (row < 0 || column < 0 || row >= row_count() || column >= column_count()) return;
  uint64_t* v = values_.get(static_cast<uint64_t>(row) * static_cast<uint64_t>(column_count()) + static_cast<uint64_t>(column));
  if (!v) return;
  *v = value;
  invalidate();
}

void HeatMap::set_range(uint64_t low, uint64_t high) {
  UiGuard lock(this);
  low_ = low < high ? low : high;
  high_ = low < high ? high : low;
  fixed_range_ = true;
  invalidate();
}

uint64_t HeatMap::range_low() const {
  if (fixed_range_) return low_;
  uint64_t low = UINT64_MAX;
  for (const uint64_t v : values_.view()) low = v < low ? v : low;
  return low == UINT64_MAX && values_.count() == 0 ? 0 : low;
}

uint64_t HeatMap::range_high() const {
  if (fixed_range_) return high_;
  uint64_t high = 0;
  for (const uint64_t v : values_.view()) high = v > high ? v : high;
  return high;
}

uint32_t HeatMap::position(uint64_t value) const {
  const uint64_t low = range_low(), high = range_high();
  if (high <= low || value <= low) return 0;
  if (value >= high) return 1000;
  return share(value - low, high - low, 1000);
}

uint32_t HeatMap::ramp_color(uint32_t permille) {
  if (permille >= 1000) return kRamp[kRampSegments];
  const uint32_t scaled = permille * kRampSegments;  // 0 .. 3000
  const uint32_t segment = scaled / 1000, t = scaled % 1000;
  const uint32_t a = kRamp[segment], b = kRamp[segment + 1];
  return make_color(mix(red_of(a), red_of(b), t), mix(green_of(a), green_of(b), t), mix(blue_of(a), blue_of(b), t));
}

int32_t HeatMap::row_label_width() const {
  int32_t widest = 0;
  for (int32_t r = 0; r < row_count(); ++r) {
    const int32_t w = Painter::text_width(row_label(r));
    widest = w > widest ? w : widest;
  }
  if (widest == 0) return 0;
  return (widest < kMaxLabelWidth ? widest : kMaxLabelWidth) + 6;
}

int32_t HeatMap::header_height() const {
  for (int32_t c = 0; c < column_count(); ++c) {
    if (column_label(c).size()) return kLabelHeight;
  }
  return 0;
}

Rect2D HeatMap::grid_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t x = kMargin + row_label_width(), y = kMargin + header_height();
  const int32_t gw = w - x - kMargin, gh = h - y - kMargin;
  if (row_count() == 0 || column_count() == 0 || gw < column_count() || gh < row_count()) return {0, 0, 0, 0};
  return {x, y, static_cast<uint32_t>(gw), static_cast<uint32_t>(gh)};
}

Rect2D HeatMap::cell_rect(int32_t row, int32_t column) const {
  const Rect2D g = grid_rect();
  const int32_t rows = row_count(), columns = column_count();
  if (g.width == 0 || row < 0 || column < 0 || row >= rows || column >= columns) return {0, 0, 0, 0};
  const int64_t gw = g.width, gh = g.height;
  const int32_t x0 = g.x + static_cast<int32_t>(gw * column / columns), x1 = g.x + static_cast<int32_t>(gw * (column + 1) / columns);
  const int32_t y0 = g.y + static_cast<int32_t>(gh * row / rows), y1 = g.y + static_cast<int32_t>(gh * (row + 1) / rows);
  return {x0, y0, static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
}

bool HeatMap::cell_at(int32_t x, int32_t y, int32_t& row, int32_t& column) const {
  const Rect2D g = grid_rect();
  if (g.width == 0 || !contains(g, x, y)) return false;
  const int32_t rows = row_count(), columns = column_count();
  int32_t c = static_cast<int32_t>(int64_t{x - g.x} * columns / g.width), r = static_cast<int32_t>(int64_t{y - g.y} * rows / g.height);
  // Rounding in cell_rect() can put a pixel in the neighbour: settle it against the real cells.
  while (c > 0 && x < cell_rect(0, c).x) --c;
  while (c + 1 < columns && x >= cell_rect(0, c + 1).x) ++c;
  while (r > 0 && y < cell_rect(r, 0).y) --r;
  while (r + 1 < rows && y >= cell_rect(r + 1, 0).y) ++r;
  row = r;
  column = c;
  return true;
}

void HeatMap::select(int32_t row, int32_t column) {
  if (row == selected_row_ && column == selected_column_) return;
  selected_row_ = row;
  selected_column_ = column;
  invalidate();
  fire(changed_);
}

void HeatMap::select_cell(int32_t row, int32_t column) {
  UiGuard lock(this);
  if (row < 0 || column < 0 || row >= row_count() || column >= column_count()) select(-1, -1);
  else select(row, column);
}

void HeatMap::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const Rect2D g = grid_rect();
  if (g.width == 0) return;
  const int32_t rows = row_count(), columns = column_count();

  for (int32_t r = 0; r < rows; ++r) {
    const Rect2D first = cell_rect(r, 0);
    const bool hot = r == selected_row_;
    const Rect2D label{kMargin, first.y, static_cast<uint32_t>(g.x - kMargin), first.height};
    if (hot) painter.fill(label, t.selection);
    painter.push_frame(label);
    const int32_t th = Painter::kGlyphHeight;
    painter.text(2, (static_cast<int32_t>(first.height) - th) / 2, row_label(r), hot ? t.selection_text : t.text);
    painter.pop_frame();
    for (int32_t c = 0; c < columns; ++c) {
      const Rect2D cell = cell_rect(r, c);
      const uint32_t color = color_of(r, c);
      painter.fill(cell, color);
      if (cell.width >= 6 && cell.height >= 6) painter.outline(cell, t.field);
      if (show_values_) {
        String text(allocator());
        text.append_uint(value(r, c));
        if (Painter::text_width(text.view()) + 2 <= static_cast<int32_t>(cell.width) && Painter::kGlyphHeight <= static_cast<int32_t>(cell.height)) {
          painter.text_centered(cell, text.view(), contrasting_text_color(color));
        }
      }
    }
  }
  for (int32_t c = 0; c < columns; ++c) {
    const Rect2D first = cell_rect(0, c);
    const bool hot = c == selected_column_;
    const Rect2D label{first.x, kMargin, first.width, static_cast<uint32_t>(g.y - kMargin)};
    if (label.height == 0) break;
    if (hot) painter.fill(label, t.selection);
    painter.push_frame(label);
    const int32_t tw = Painter::text_width(column_label(c));
    const int32_t tx = tw < static_cast<int32_t>(first.width) ? (static_cast<int32_t>(first.width) - tw) / 2 : 0;
    painter.text(tx, (kLabelHeight - Painter::kGlyphHeight) / 2, column_label(c), hot ? t.selection_text : t.text);
    painter.pop_frame();
  }
  if (selected_row_ >= 0) {
    const Rect2D cell = cell_rect(selected_row_, selected_column_);
    painter.outline(cell, t.selection);
    if (cell.width > 4 && cell.height > 4) painter.outline({cell.x + 1, cell.y + 1, cell.width - 2, cell.height - 2}, t.selection_text);
  }
  if (focused()) painter.focus_frame(all);
}

void HeatMap::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  int32_t row = -1, column = -1;
  if (!cell_at(args.x, args.y, row, column)) {
    select(-1, -1);
    return;
  }
  select(row, column);
}

bool HeatMap::on_key(const KeyArgs& args) {
  const int32_t rows = row_count(), columns = column_count();
  if (rows == 0 || columns == 0) return false;
  int32_t dr = 0, dc = 0;
  if (args.usage == hardware::key::kRight) dc = 1;
  else if (args.usage == hardware::key::kLeft) dc = -1;
  else if (args.usage == hardware::key::kDown) dr = 1;
  else if (args.usage == hardware::key::kUp) dr = -1;
  else return false;
  if (selected_row_ < 0) {
    select(0, 0);
    return true;
  }
  const int32_t r = selected_row_ + dr, c = selected_column_ + dc;
  if (r >= 0 && r < rows && c >= 0 && c < columns) select(r, c);
  return true;
}

}  // namespace gui
