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

#include "Gui/Controls/ColorPalette.hpp"

namespace gui {

namespace {
const uint32_t kClassic[28] = {
    0xFF000000, 0xFF808080, 0xFF800000, 0xFF808000, 0xFF008000, 0xFF008080, 0xFF000080, 0xFF800080, 0xFF808040, 0xFF004040, 0xFF0080FF, 0xFF004080, 0xFF4000FF, 0xFF804000,
    0xFFFFFFFF, 0xFFC0C0C0, 0xFFFF0000, 0xFFFFFF00, 0xFF00FF00, 0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF, 0xFFFFFF80, 0xFF00FF80, 0xFF80FFFF, 0xFF8080FF, 0xFFFF0080, 0xFFFF8040};
}

ColorPalette::ColorPalette(allocators::IAllocator& allocator) : Control(allocator), colors_(allocator) {
  // Row by row: two rows of 14, the dark colors above and their light partners below.
  for (uint32_t c : kClassic) colors_.push_back(c);
}

bool ColorPalette::set_colors(const uint32_t* colors, uint32_t count) {
  UiGuard lock(this);
  List<uint32_t> next(allocator());
  if (!next.reserve(count)) return false;
  for (uint32_t i = 0; i < count; ++i) next.push_back(colors[i] | 0xFF000000u);
  colors_.clear();
  for (uint32_t c : next) colors_.push_back(c);
  invalidate();
  return true;
}

void ColorPalette::set_rows(uint32_t rows) {
  UiGuard lock(this);
  rows_ = rows ? rows : 1;
  invalidate();
}

void ColorPalette::set_primary(uint32_t argb) {
  UiGuard lock(this);
  argb |= 0xFF000000u;
  if (argb == primary_) return;
  primary_ = argb;
  invalidate();
  fire(changed_);
}

void ColorPalette::set_secondary(uint32_t argb) {
  UiGuard lock(this);
  argb |= 0xFF000000u;
  if (argb == secondary_) return;
  secondary_ = argb;
  invalidate();
  fire(changed_);
}

void ColorPalette::swap_colors() {
  UiGuard lock(this);
  if (primary_ == secondary_) return;
  const uint32_t p = primary_;
  primary_ = secondary_;
  secondary_ = p;
  invalidate();
  fire(changed_);
}

Rect2D ColorPalette::swatch_rect(uint32_t index) const {
  const uint32_t per_row = columns() ? columns() : 1, row = index / per_row, column = index % per_row;
  const int32_t left = indicator_ ? 2 * kPad + kIndicator + kPad : kPad;
  return {left + static_cast<int32_t>(column) * (kSwatch + kGap), kPad + static_cast<int32_t>(row) * (kSwatch + kGap),
          static_cast<uint32_t>(kSwatch), static_cast<uint32_t>(kSwatch)};
}

int32_t ColorPalette::swatch_at(int32_t x, int32_t y) const {
  for (uint32_t i = 0; i < colors_.count(); ++i)
    if (contains(swatch_rect(i), x, y)) return static_cast<int32_t>(i);
  return -1;
}

void ColorPalette::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  // The two colors in use: the secondary behind, the primary in front.
  if (indicator_) {
    const Rect2D sec = secondary_rect(), pri = primary_rect();
    painter.fill(sec, secondary_);
    painter.bevel(sec, true);
    painter.fill(pri, primary_);
    painter.bevel(pri, true);
  }
  for (uint32_t i = 0; i < colors_.count(); ++i) {
    const Rect2D r = swatch_rect(i);
    painter.fill(r, colors_[i]);
    painter.bevel(r, true);
  }
}

void ColorPalette::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left && args.button != MouseButton::Right) return;
  const int32_t swatch = swatch_at(args.x, args.y);
  if (swatch >= 0) {
    if (args.button == MouseButton::Left) set_primary(colors_[static_cast<uint32_t>(swatch)]);
    else set_secondary(colors_[static_cast<uint32_t>(swatch)]);
  } else if (indicator_ && contains(indicator_rect(), args.x, args.y)) {
    swap_colors();
  }
}

}  // namespace gui
