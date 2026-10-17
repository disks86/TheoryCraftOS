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

#include "Gui/Controls/Slider.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

int32_t Slider::travel() const {
  const int32_t t = length() - kThumbLength;
  return t > 0 ? t : 0;
}

// Thumb start (distance from the left, or from the top) for a value.
int32_t Slider::position_of(int32_t value) const {
  const int64_t span = int64_t{max_} - min_;
  if (span <= 0) return 0;
  const int32_t along = static_cast<int32_t>((int64_t{value} - min_) * travel() / span);
  return orientation_ == Orientation::Horizontal ? along : travel() - along;
}

int32_t Slider::value_at(int32_t pixel) const {
  const int32_t t = travel();
  if (t <= 0) return min_;
  if (pixel < 0) pixel = 0;
  if (pixel > t) pixel = t;
  const int32_t along = orientation_ == Orientation::Horizontal ? pixel : t - pixel;
  const int64_t span = int64_t{max_} - min_;
  return static_cast<int32_t>(min_ + (int64_t{along} * span + t / 2) / t);  // rounded
}

Rect2D Slider::thumb_rect() const {
  const int32_t p = position_of(value_);
  if (orientation_ == Orientation::Horizontal) {
    const int32_t y = (static_cast<int32_t>(bounds_.height) - kThumbThickness) / 2;
    return {p, y, static_cast<uint32_t>(kThumbLength), static_cast<uint32_t>(kThumbThickness)};
  }
  const int32_t x = (static_cast<int32_t>(bounds_.width) - kThumbThickness) / 2;
  return {x, p, static_cast<uint32_t>(kThumbThickness), static_cast<uint32_t>(kThumbLength)};
}

void Slider::set_range(int32_t minimum, int32_t maximum) {
  UiGuard lock(this);
  min_ = minimum;
  max_ = maximum < minimum ? minimum : maximum;
  invalidate();
  set_value(value_);
}

void Slider::set_value(int32_t value) {
  UiGuard lock(this);
  if (value < min_) value = min_;
  if (value > max_) value = max_;
  if (value == value_) return;
  value_ = value;
  invalidate();
  fire(changed_);
}

void Slider::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const bool horizontal = orientation_ == Orientation::Horizontal;
  // The groove: a thin sunken channel through the middle.
  const Rect2D groove = horizontal ? Rect2D{kThumbLength / 2, h / 2 - 2, static_cast<uint32_t>(w > kThumbLength ? w - kThumbLength : 0), 4}
                                   : Rect2D{w / 2 - 2, kThumbLength / 2, 4, static_cast<uint32_t>(h > kThumbLength ? h - kThumbLength : 0)};
  painter.fill(groove, enabled() ? t.field : t.face);
  painter.bevel(groove, true);
  const Rect2D thumb = thumb_rect();
  painter.fill(thumb, t.face);
  painter.bevel(thumb, false);
  // A centre line on the thumb.
  if (horizontal) painter.line(thumb.x + thumb.width / 2, thumb.y + 4, thumb.x + thumb.width / 2, thumb.y + thumb.height - 5, enabled() ? t.shadow : t.light);
  else painter.line(thumb.x + 4, thumb.y + thumb.height / 2, thumb.x + thumb.width - 5, thumb.y + thumb.height / 2, enabled() ? t.shadow : t.light);
  if (focused()) painter.focus_frame({0, 0, bounds_.width, bounds_.height});
}

void Slider::drag_to(const MouseArgs& args) {
  const int32_t pixel = (orientation_ == Orientation::Horizontal ? args.x : args.y) - grab_;
  set_value(value_at(pixel));
}

void Slider::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const Rect2D thumb = thumb_rect();
  const bool on_thumb = contains(thumb, args.x, args.y);
  // On the thumb, keep the point that was picked up under the pointer; elsewhere centre the thumb there.
  grab_ = on_thumb ? (orientation_ == Orientation::Horizontal ? args.x - thumb.x : args.y - thumb.y) : kThumbLength / 2;
  dragging_ = true;
  if (!on_thumb) drag_to(args);
  invalidate();
}

void Slider::on_mouse_move(const MouseArgs& args) {
  if (dragging_) drag_to(args);
}

void Slider::on_mouse_up(const MouseArgs&) {
  dragging_ = false;
  invalidate();
}

void Slider::on_mouse_wheel(const MouseArgs& args) {
  if (args.wheel > 0) set_value(value_ + step_);
  else if (args.wheel < 0) set_value(value_ - step_);
}

bool Slider::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kRight:
    case key::kUp:
      set_value(value_ + step_);
      return true;
    case key::kLeft:
    case key::kDown:
      set_value(value_ - step_);
      return true;
    case 0x4B:  // Page Up
      set_value(value_ + page_);
      return true;
    case 0x4E:  // Page Down
      set_value(value_ - page_);
      return true;
    case 0x4A:  // Home
      set_value(min_);
      return true;
    case 0x4D:  // End
      set_value(max_);
      return true;
    default:
      return false;
  }
}

}  // namespace gui
