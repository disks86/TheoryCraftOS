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

#include "Gui/Controls/ScrollBar.hpp"

namespace gui {

void ScrollBar::set_metrics(int32_t total, int32_t page) {
  UiGuard lock(this);
  total_ = total > 0 ? total : 0;
  page_ = page > 0 ? page : 0;
  invalidate();
  set_value(value_);
}

void ScrollBar::set_value(int32_t value) {
  UiGuard lock(this);
  if (value < 0) value = 0;
  if (value > maximum_value()) value = maximum_value();
  if (value == value_) return;
  value_ = value;
  invalidate();
  fire(changed_);
}

int32_t ScrollBar::thumb_length() const {
  const int32_t track = track_length();
  if (total_ <= page_ || total_ == 0) return 0;
  int64_t length = int64_t{track} * page_ / total_;
  if (length < kMinThumb) length = kMinThumb;
  return length > track ? track : static_cast<int32_t>(length);
}

int32_t ScrollBar::thumb_offset() const {
  const int32_t room = track_length() - thumb_length();
  const int32_t range = maximum_value();
  return range > 0 && room > 0 ? static_cast<int32_t>(int64_t{value_} * room / range) : 0;
}

Rect2D ScrollBar::thumb_rect() const {
  const int32_t len = thumb_length();
  if (len == 0) return {0, 0, 0, 0};
  const int32_t start = kThickness + thumb_offset();
  if (orientation_ == Orientation::Vertical) return {0, start, bounds_.width, static_cast<uint32_t>(len)};
  return {start, 0, static_cast<uint32_t>(len), bounds_.height};
}

ScrollBar::Part ScrollBar::part_at(const MouseArgs& args) const {
  const int32_t p = along(args);
  if (p < 0 || p >= length()) return Part::None;
  if (p < kThickness) return Part::Before;
  if (p >= length() - kThickness) return Part::After;
  const Rect2D thumb = thumb_rect();
  if (thumb.width == 0 || thumb.height == 0) return Part::None;  // nothing to scroll
  const int32_t start = kThickness + thumb_offset();
  if (p < start) return Part::TrackBefore;
  if (p >= start + thumb_length()) return Part::TrackAfter;
  return Part::Thumb;
}

void ScrollBar::draw_arrow(Painter& painter, const Rect2D& button, bool toward_start, bool pressed) {
  const Theme& t = painter.theme();
  painter.fill(button, t.face);
  painter.bevel(button, pressed);
  const int32_t cx = button.x + static_cast<int32_t>(button.width) / 2 + (pressed ? 1 : 0);
  const int32_t cy = button.y + static_cast<int32_t>(button.height) / 2 + (pressed ? 1 : 0);
  const uint32_t color = enabled() && total_ > page_ ? t.text : t.disabled_text;
  for (int32_t i = 0; i < 4; ++i) {  // a triangle, four lines thick, pointing the way
    const int32_t d = toward_start ? i : 3 - i;
    if (orientation_ == Orientation::Vertical) painter.line(cx - 3 + d, cy - 2 + i, cx + 3 - d, cy - 2 + i, color);
    else painter.line(cx - 2 + i, cy - 3 + d, cx - 2 + i, cy + 3 - d, color);
  }
}

void ScrollBar::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const bool vertical = orientation_ == Orientation::Vertical;
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.light);
  const Rect2D first = vertical ? Rect2D{0, 0, bounds_.width, static_cast<uint32_t>(kThickness)} : Rect2D{0, 0, static_cast<uint32_t>(kThickness), bounds_.height};
  const Rect2D last = vertical ? Rect2D{0, h - kThickness, bounds_.width, static_cast<uint32_t>(kThickness)}
                               : Rect2D{w - kThickness, 0, static_cast<uint32_t>(kThickness), bounds_.height};
  draw_arrow(painter, first, true, pressed_ == Part::Before);
  draw_arrow(painter, last, false, pressed_ == Part::After);
  const Rect2D thumb = thumb_rect();
  if (thumb.width > 0 && thumb.height > 0) {
    painter.fill(thumb, t.face);
    painter.bevel(thumb, false);
  }
}

void ScrollBar::drag_to(const MouseArgs& args) {
  const int32_t room = track_length() - thumb_length();
  const int32_t range = maximum_value();
  if (room <= 0 || range <= 0) return;
  int32_t offset = along(args) - kThickness - grab_;
  if (offset < 0) offset = 0;
  if (offset > room) offset = room;
  set_value(static_cast<int32_t>((int64_t{offset} * range + room / 2) / room));
}

void ScrollBar::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  pressed_ = part_at(args);
  switch (pressed_) {
    case Part::Before: set_value(value_ - step_); break;
    case Part::After: set_value(value_ + step_); break;
    case Part::TrackBefore: set_value(value_ - (page_ > step_ ? page_ : step_)); break;
    case Part::TrackAfter: set_value(value_ + (page_ > step_ ? page_ : step_)); break;
    case Part::Thumb: grab_ = along(args) - kThickness - thumb_offset(); break;
    case Part::None: break;
  }
  invalidate();
}

void ScrollBar::on_mouse_move(const MouseArgs& args) {
  if (pressed_ == Part::Thumb) drag_to(args);
}

void ScrollBar::on_mouse_up(const MouseArgs&) {
  pressed_ = Part::None;
  invalidate();
}

void ScrollBar::on_mouse_wheel(const MouseArgs& args) {
  if (args.wheel > 0) set_value(value_ - 3 * step_);  // wheel up goes towards the start
  else if (args.wheel < 0) set_value(value_ + 3 * step_);
}

}  // namespace gui
