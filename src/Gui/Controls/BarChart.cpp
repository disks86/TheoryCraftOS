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

#include "Gui/Controls/BarChart.hpp"
#include "Gui/Color.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/String.hpp"

namespace gui {

namespace {

constexpr uint32_t kPalette[] = {0xFF4E79A7, 0xFFF28E2B, 0xFFE15759, 0xFF76B7B2, 0xFF59A14F, 0xFFEDC948, 0xFFB07AA1, 0xFF9C755F};
constexpr uint32_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// value * scale / top for value <= top, without overflow: both are halved until the product fits (the result changes by far less than a pixel).
uint32_t share(uint64_t value, uint64_t top, uint32_t scale) {
  if (value >= top) return scale;
  while (top > UINT64_MAX / scale) {
    top >>= 1;
    value >>= 1;
  }
  return top ? static_cast<uint32_t>(value * scale / top) : 0;
}

uint32_t lighten(uint32_t argb) {
  auto up = [](uint8_t c) { return static_cast<uint8_t>(c + (255 - c) / 2); };
  return make_color(up(red_of(argb)), up(green_of(argb)), up(blue_of(argb)));
}

}  // namespace

int32_t BarChart::add_bar(StringView label, uint64_t value, uint32_t color) {
  UiGuard lock(this);
  if (color == 0) color = kPalette[static_cast<uint32_t>(labels_.count()) % kPaletteSize];
  if (!values_.push_back(value)) return -1;
  if (!colors_.push_back(color)) {
    values_.remove_at(values_.count() - 1);
    return -1;
  }
  const int32_t index = labels_.add(label);
  if (index < 0) {
    values_.remove_at(values_.count() - 1);
    colors_.remove_at(colors_.count() - 1);
    return -1;
  }
  invalidate();
  return index;
}

void BarChart::clear_bars() {
  UiGuard lock(this);
  labels_.clear();
  values_.clear();
  colors_.clear();
  selected_ = -1;
  invalidate();
}

void BarChart::set_bar_value(int32_t index, uint64_t value) {
  UiGuard lock(this);
  uint64_t* v = index >= 0 ? values_.get(static_cast<uint64_t>(index)) : nullptr;
  if (!v) return;
  *v = value;
  invalidate();
}

uint64_t BarChart::scale_top() const {
  if (maximum_) return maximum_;
  uint64_t top = 1;
  for (const uint64_t v : values_.view()) top = v > top ? v : top;
  return top;
}

int32_t BarChart::axis_width() const {
  String text(allocator());
  text.append_uint(scale_top());
  return Painter::text_width(text.view()) + 6;
}

Rect2D BarChart::plot_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t x = kMargin + axis_width();
  const int32_t y = kMargin + Painter::kGlyphHeight / 2;
  const int32_t pw = w - x - kMargin, ph = h - y - kMargin - kLabelHeight;
  if (pw < 4 || ph < 4) return {0, 0, 0, 0};
  return {x, y, static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)};
}

Rect2D BarChart::column_rect(int32_t index) const {
  const Rect2D p = plot_rect();
  const int32_t n = bar_count();
  if (p.width == 0 || index < 0 || index >= n) return {0, 0, 0, 0};
  const int64_t pw = p.width;
  const int32_t x0 = p.x + static_cast<int32_t>(pw * index / n), x1 = p.x + static_cast<int32_t>(pw * (index + 1) / n);
  return {x0, p.y, static_cast<uint32_t>(x1 - x0), p.height};
}

uint32_t BarChart::bar_height(uint64_t value) const {
  const Rect2D p = plot_rect();
  if (value == 0 || p.width == 0) return 0;
  const uint32_t h = share(value, scale_top(), p.height);
  return h ? h : 1;
}

int32_t BarChart::bar_at(int32_t x, int32_t y) const {
  const Rect2D p = plot_rect();
  const int32_t n = bar_count();
  if (p.width == 0 || n == 0) return -1;
  if (x < p.x || x >= p.x + static_cast<int32_t>(p.width)) return -1;
  if (y < p.y || y >= p.y + static_cast<int32_t>(p.height) + kLabelHeight) return -1;
  int32_t i = static_cast<int32_t>(int64_t{x - p.x} * n / p.width);
  // Rounding in column_rect() can put a pixel in the neighbour: settle it against the real columns.
  while (i > 0 && x < column_rect(i).x) --i;
  while (i + 1 < n && x >= column_rect(i + 1).x) ++i;
  return i;
}

void BarChart::select(int32_t index) {
  if (index == selected_) return;
  selected_ = index;
  invalidate();
  fire(changed_);
}

void BarChart::set_selected_index(int32_t index) {
  UiGuard lock(this);
  select(index >= 0 && index < bar_count() ? index : -1);
}

void BarChart::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const Rect2D p = plot_rect();
  if (p.width == 0) return;
  const int32_t px = p.x, py = p.y, pw = static_cast<int32_t>(p.width), ph = static_cast<int32_t>(p.height);
  // Gridlines at every quarter, the axis labels at the bottom and the top.
  for (int32_t q = 0; q <= 4; ++q) {
    const int32_t y = py + ph - 1 - (ph - 1) * q / 4;
    painter.line(px, y, px + pw - 1, y, q == 0 ? t.dark : t.light);
  }
  String text(allocator());
  text.append_uint(scale_top());
  painter.text(kMargin, py - Painter::kGlyphHeight / 2, text.view(), t.text);
  painter.text(kMargin + axis_width() - 6 - Painter::kGlyphWidth, py + ph - 1 - Painter::kGlyphHeight / 2, "0", t.text);
  painter.line(px, py, px, py + ph - 1, t.dark);

  const int32_t n = bar_count();
  for (int32_t i = 0; i < n; ++i) {
    const Rect2D col = column_rect(i);
    const int32_t gap = static_cast<int32_t>(col.width) > 2 * kBarGap ? kBarGap : static_cast<int32_t>(col.width) / 4;
    const uint32_t bw = col.width > static_cast<uint32_t>(gap) ? col.width - static_cast<uint32_t>(gap) : 1;
    const uint32_t bh = bar_height(bar_value(i));
    uint32_t color = bar_color(i);
    if (i == selected_) color = lighten(color);
    if (bh) {
      const Rect2D bar{col.x + gap / 2, py + ph - 1 - static_cast<int32_t>(bh), bw, bh};
      painter.fill(bar, color);
      painter.outline(bar, t.dark);
    }
    // The label, clipped to the column.
    if (i == selected_) painter.fill({col.x, py + ph, col.width, static_cast<uint32_t>(kLabelHeight)}, t.selection);
    painter.push_frame({col.x, py + ph, col.width, static_cast<uint32_t>(kLabelHeight)});
    const int32_t tw = Painter::text_width(bar_label(i));
    const int32_t tx = tw < static_cast<int32_t>(col.width) ? (static_cast<int32_t>(col.width) - tw) / 2 : 0;
    painter.text(tx, (kLabelHeight - Painter::kGlyphHeight) / 2, bar_label(i), i == selected_ ? t.selection_text : t.text);
    painter.pop_frame();
  }
  if (focused()) painter.focus_frame(all);
}

void BarChart::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  select(bar_at(args.x, args.y));
}

bool BarChart::on_key(const KeyArgs& args) {
  const int32_t n = bar_count();
  if (n == 0) return false;
  if (args.usage == hardware::key::kRight) {
    select(selected_ + 1 < n ? selected_ + 1 : 0);
    return true;
  }
  if (args.usage == hardware::key::kLeft) {
    select(selected_ > 0 ? selected_ - 1 : n - 1);
    return true;
  }
  return false;
}

}  // namespace gui
