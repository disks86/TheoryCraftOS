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

#include "Gui/Controls/Pagination.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/String.hpp"

namespace gui {

void Pagination::set_page_count(int32_t count) {
  UiGuard lock(this);
  page_count_ = count < 1 ? 1 : count;
  invalidate();
  set_current_page(current_);
}

void Pagination::set_current_page(int32_t page) {
  UiGuard lock(this);
  if (page < 0) page = 0;
  if (page >= page_count_) page = page_count_ - 1;
  if (page == current_) return;
  current_ = page;
  invalidate();
  fire(changed_);
}

int32_t Pagination::entries(ArrayView<int32_t> out) const {
  const int32_t n = page_count_;
  int32_t count = 0;
  auto put = [&](int32_t value) {
    if (static_cast<uint64_t>(count) < out.size()) out[static_cast<uint64_t>(count)] = value;
    ++count;
  };
  if (n <= kMaxEntries) {
    for (int32_t p = 0; p < n; ++p) put(p);
  } else if (current_ <= 3) {
    for (int32_t p = 0; p <= 4; ++p) put(p);
    put(kGap);
    put(n - 1);
  } else if (current_ >= n - 4) {
    put(0);
    put(kGap);
    for (int32_t p = n - 5; p < n; ++p) put(p);
  } else {
    put(0);
    put(kGap);
    for (int32_t p = current_ - 1; p <= current_ + 1; ++p) put(p);
    put(kGap);
    put(n - 1);
  }
  return count < static_cast<int32_t>(out.size()) ? count : static_cast<int32_t>(out.size());
}

int32_t Pagination::entry_width(int32_t entry) const {
  if (entry < 0) return kButtonWidth;
  String text(allocator());
  text.append_uint(static_cast<uint64_t>(entry) + 1);
  const int32_t w = Painter::text_width(text.view()) + 12;
  return w > kButtonWidth ? w : kButtonWidth;
}

// Left edge of the position-th entry: after the previous button and every entry before it.
int32_t Pagination::entry_x(int32_t position) const {
  int32_t e[kMaxEntries];
  const int32_t n = entries(ArrayView<int32_t>(e, kMaxEntries));
  int32_t x = kMargin + kButtonWidth + kSpacing;
  for (int32_t i = 0; i < position && i < n; ++i) x += entry_width(e[i]) + kSpacing;
  return x;
}

Rect2D Pagination::previous_rect() const { return {kMargin, kMargin, static_cast<uint32_t>(kButtonWidth), static_cast<uint32_t>(kButtonHeight)}; }

Rect2D Pagination::entry_rect(int32_t position) const {
  int32_t e[kMaxEntries];
  const int32_t n = entries(ArrayView<int32_t>(e, kMaxEntries));
  if (position < 0 || position >= n) return {0, 0, 0, 0};
  return {entry_x(position), kMargin, static_cast<uint32_t>(entry_width(e[position])), static_cast<uint32_t>(kButtonHeight)};
}

Rect2D Pagination::next_rect() const {
  int32_t e[kMaxEntries];
  const int32_t n = entries(ArrayView<int32_t>(e, kMaxEntries));
  return {entry_x(n), kMargin, static_cast<uint32_t>(kButtonWidth), static_cast<uint32_t>(kButtonHeight)};
}

int32_t Pagination::row_width() const {
  const Rect2D n = next_rect();
  return n.x + static_cast<int32_t>(n.width) + kMargin;
}

int32_t Pagination::button_at(int32_t x, int32_t y) const {
  if (contains(previous_rect(), x, y)) return kPrevious;
  if (contains(next_rect(), x, y)) return kNext;
  int32_t e[kMaxEntries];
  const int32_t n = entries(ArrayView<int32_t>(e, kMaxEntries));
  for (int32_t i = 0; i < n; ++i) {
    if (contains(entry_rect(i), x, y)) return e[i];
  }
  return kNone;
}

void Pagination::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  auto draw = [&](const Rect2D& r, StringView label, bool pressed, bool dim) {
    painter.fill(r, pressed ? t.field : t.face);
    painter.bevel(r, pressed);
    painter.text_centered(r, label, dim ? t.disabled_text : ink);
  };
  draw(previous_rect(), "<", false, current_ == 0);
  int32_t e[kMaxEntries];
  const int32_t n = entries(ArrayView<int32_t>(e, kMaxEntries));
  for (int32_t i = 0; i < n; ++i) {
    const Rect2D r = entry_rect(i);
    if (e[i] == kGap) {
      painter.text_centered(r, "...", ink);
      continue;
    }
    String text(allocator());
    text.append_uint(static_cast<uint64_t>(e[i]) + 1);
    draw(r, text.view(), e[i] == current_, false);
  }
  draw(next_rect(), ">", false, current_ == page_count_ - 1);
  if (focused()) painter.focus_frame({0, 0, bounds_.width, bounds_.height});
}

void Pagination::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const int32_t b = button_at(args.x, args.y);
  if (b == kPrevious) set_current_page(current_ - 1);
  else if (b == kNext) set_current_page(current_ + 1);
  else if (b >= 0) set_current_page(b);
}

bool Pagination::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kLeft: set_current_page(current_ - 1); return true;
    case key::kRight: set_current_page(current_ + 1); return true;
    case 0x4A: set_current_page(0); return true;                // Home
    case 0x4D: set_current_page(page_count_ - 1); return true;  // End
    default: return false;
  }
}

}  // namespace gui
