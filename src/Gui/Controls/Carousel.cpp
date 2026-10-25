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

#include "Gui/Controls/Carousel.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {

// The window marks only the deepest control under the pointer as hovered, so a pointer over a slide's content counts for the carousel too.
bool subtree_hovered(const Control& control) {
  if (control.hovered()) return true;
  for (uint32_t i = 0; i < control.child_count(); ++i) {
    if (subtree_hovered(*control.child(i))) return true;
  }
  return false;
}

}  // namespace

int32_t Carousel::add_slide(Control* slide) {
  UiGuard lock(this);
  if (!slide) return -1;
  if (!slides_.push_back(slide)) return -1;
  if (!add(slide)) {
    slides_.remove_at(slides_.count() - 1);
    return -1;
  }
  slide->set_dock(DockStyle::Fill);
  const int32_t index = static_cast<int32_t>(slides_.count()) - 1;
  slide->set_visible(index == 0);
  if (current_ < 0) current_ = 0;
  invalidate();
  return index;
}

void Carousel::set_current_slide(int32_t index) {
  UiGuard lock(this);
  if (index < 0 || index >= slide_count() || index == current_) return;
  current_ = index;
  elapsed_ = 0;
  for (uint64_t i = 0; i < slides_.count(); ++i) slides_[i]->set_visible(static_cast<int32_t>(i) == current_);
  invalidate();
  fire(changed_);
}

bool Carousel::can_step(int32_t direction) const {
  const int32_t n = slide_count();
  if (n < 2) return false;
  return wrap_ || (direction < 0 ? current_ > 0 : current_ < n - 1);
}

void Carousel::step(int32_t direction) {
  UiGuard lock(this);
  if (!can_step(direction)) return;
  const int32_t n = slide_count();
  set_current_slide(((current_ + direction) % n + n) % n);
}

void Carousel::next() { step(1); }
void Carousel::previous() { step(-1); }

void Carousel::tick(uint32_t elapsed_milliseconds) {
  UiGuard lock(this);
  if (interval_ == 0 || slide_count() < 2 || subtree_hovered(*this)) return;
  // Saturating: a huge elapsed time must not wrap the counter back below the interval.
  elapsed_ = elapsed_milliseconds > UINT32_MAX - elapsed_ ? UINT32_MAX : elapsed_ + elapsed_milliseconds;
  if (elapsed_ < interval_) return;
  elapsed_ = 0;
  if (can_step(1)) next();
  else if (!wrap_) set_current_slide(0);   // the end of a non-wrapping carousel starts over
}

Rect2D Carousel::slide_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kArrowWidth, h = static_cast<int32_t>(bounds_.height) - kDotsHeight;
  return {kArrowWidth, 0, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

Rect2D Carousel::previous_rect() const {
  const int32_t h = static_cast<int32_t>(bounds_.height) - kDotsHeight;
  return {0, 0, static_cast<uint32_t>(kArrowWidth), static_cast<uint32_t>(h > 0 ? h : 0)};
}

Rect2D Carousel::next_rect() const {
  const int32_t h = static_cast<int32_t>(bounds_.height) - kDotsHeight;
  return {static_cast<int32_t>(bounds_.width) - kArrowWidth, 0, static_cast<uint32_t>(kArrowWidth), static_cast<uint32_t>(h > 0 ? h : 0)};
}

Rect2D Carousel::dot_rect(int32_t index) const {
  const int32_t n = slide_count();
  if (index < 0 || index >= n) return {0, 0, 0, 0};
  const int32_t row = static_cast<int32_t>(bounds_.height) - kDotsHeight;
  const int32_t total = (n - 1) * kDotSpacing + kDotSize;
  const int32_t x0 = (static_cast<int32_t>(bounds_.width) - total) / 2;
  return {x0 + index * kDotSpacing, row + (kDotsHeight - kDotSize) / 2, static_cast<uint32_t>(kDotSize), static_cast<uint32_t>(kDotSize)};
}

int32_t Carousel::dot_at(int32_t x, int32_t y) const {
  for (int32_t i = 0; i < slide_count(); ++i) {
    const Rect2D d = dot_rect(i);
    const Rect2D hit{d.x - (kDotSpacing - kDotSize) / 2, static_cast<int32_t>(bounds_.height) - kDotsHeight, static_cast<uint32_t>(kDotSpacing), static_cast<uint32_t>(kDotsHeight)};
    if (contains(hit, x, y)) return i;
  }
  return -1;
}

void Carousel::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  // The arrows: a triangle on a raised button, grey when that way is closed.
  auto arrow = [&](const Rect2D& r, int32_t direction) {
    if (r.width == 0 || r.height == 0) return;
    painter.fill(r, t.face);
    painter.bevel(r, false);
    const uint32_t color = can_step(direction) ? ink : t.disabled_text;
    const int32_t cx = r.x + static_cast<int32_t>(r.width) / 2, cy = r.y + static_cast<int32_t>(r.height) / 2;
    for (int32_t k = 0; k < 5; ++k) {
      const int32_t x = direction > 0 ? cx - 2 + k : cx + 2 - k;
      painter.line(x, cy - 4 + k, x, cy + 4 - k, color);
    }
  };
  arrow(previous_rect(), -1);
  arrow(next_rect(), 1);
  // The dots: filled for the current slide, outlined for the others.
  graphics::IRenderer2D& renderer = painter.renderer();
  for (int32_t i = 0; i < slide_count(); ++i) {
    const Rect2D d = dot_rect(i);
    const int32_t cx = d.x + kDotSize / 2, cy = d.y + kDotSize / 2;
    renderer.fill_circle(cx + painter.origin_x(), cy + painter.origin_y(), kDotSize / 2, ink);
    if (i != current_) renderer.fill_circle(cx + painter.origin_x(), cy + painter.origin_y(), kDotSize / 2 - 2, t.face);
  }
  if (focused()) painter.focus_frame({0, 0, bounds_.width, bounds_.height});
}

void Carousel::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (contains(previous_rect(), args.x, args.y)) {
    previous();
  } else if (contains(next_rect(), args.x, args.y)) {
    next();
  } else {
    const int32_t dot = dot_at(args.x, args.y);
    if (dot >= 0) set_current_slide(dot);
  }
}

bool Carousel::on_key(const KeyArgs& args) {
  using namespace hardware;
  if (!focused() || slide_count() == 0) return false;
  switch (args.usage) {
    case key::kLeft: previous(); return true;
    case key::kRight: next(); return true;
    case 0x4A: set_current_slide(0); return true;                    // Home
    case 0x4D: set_current_slide(slide_count() - 1); return true;    // End
    default: return false;
  }
}

}  // namespace gui
