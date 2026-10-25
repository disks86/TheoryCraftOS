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

#include "Gui/Controls/Tooltip.hpp"

namespace gui {

namespace {

// Calls `each` with every line of `text` (without its '\n'); an empty text has none.
template <class F>
void for_each_line(StringView text, F&& each) {
  if (text.empty()) return;
  uint64_t start = 0;
  for (uint64_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      each(text.substr(start, i - start));
      start = i + 1;
    }
  }
}

}  // namespace

bool Tooltip::set_text(StringView text) {
  UiGuard lock(this);
  const bool ok = text_.assign(text);
  if (!ok) text_.clear();
  if (text_.empty()) set_visible(false);
  invalidate();
  return ok;
}

uint32_t Tooltip::line_count() const {
  uint32_t n = 0;
  for_each_line(text_.view(), [&](StringView) { ++n; });
  return n;
}

Size Tooltip::box_size() const {
  int32_t widest = 0;
  uint32_t lines = 0;
  for_each_line(text_.view(), [&](StringView line) {
    const int32_t w = Painter::text_width(line);
    if (w > widest) widest = w;
    ++lines;
  });
  return {widest + 2 * kPadX + 2, static_cast<int32_t>(lines) * Painter::kGlyphHeight + 2 * kPadY + 2};
}

void Tooltip::place_inside(int32_t x, int32_t y) {
  const Size size = box_size();
  if (const Control* p = parent()) {
    const int32_t pw = static_cast<int32_t>(p->bounds().width), ph = static_cast<int32_t>(p->bounds().height);
    if (x > pw - size.width) x = pw - size.width;
    if (y > ph - size.height) y = ph - size.height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
  }
  bounds_ = {x, y, static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)};
}

void Tooltip::show_at(int32_t x, int32_t y) {
  UiGuard lock(this);
  if (text_.empty()) return;
  const Size size = box_size();
  int32_t bx = x + kPointerGap / 2, by = y + kPointerGap + 4;
  if (const Control* p = parent()) {
    // Flip to the other side of the pointer rather than cover it.
    if (bx + size.width > static_cast<int32_t>(p->bounds().width)) bx = x - kPointerGap / 2 - size.width;
    if (by + size.height > static_cast<int32_t>(p->bounds().height)) by = y - kPointerGap / 2 - size.height;
  }
  place_inside(bx, by);
  set_visible(true);
  invalidate();
}

void Tooltip::show_below(const Rect2D& anchor) {
  UiGuard lock(this);
  if (text_.empty()) return;
  const Size size = box_size();
  int32_t by = anchor.y + static_cast<int32_t>(anchor.height) + 2;
  if (const Control* p = parent()) {
    if (by + size.height > static_cast<int32_t>(p->bounds().height)) by = anchor.y - 2 - size.height;
  }
  place_inside(anchor.x, by);
  set_visible(true);
  invalidate();
}

void Tooltip::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, kBackground);
  painter.outline(all, t.dark);
  int32_t y = 1 + kPadY;
  for_each_line(text_.view(), [&](StringView line) {
    painter.text(1 + kPadX, y, line, t.text);
    y += Painter::kGlyphHeight;
  });
}

}  // namespace gui
