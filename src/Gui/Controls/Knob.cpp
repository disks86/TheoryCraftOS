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

#include "Gui/Controls/Knob.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {

// sin(0), sin(5), ... sin(90) degrees, times 1024.
constexpr int32_t kSine5[] = {0, 89, 178, 265, 350, 433, 512, 587, 658, 724, 784, 839, 887, 928, 962, 989, 1008, 1020, 1024};
constexpr int32_t kSineSteps = sizeof(kSine5) / sizeof(kSine5[0]);

int32_t quarter_sine(int32_t degrees) {  // 0..180
  if (degrees > 90) degrees = 180 - degrees;
  const int32_t i = degrees / 5, f = degrees % 5;
  const int32_t next = i + 1 < kSineSteps ? i + 1 : i;
  return kSine5[i] + (kSine5[next] - kSine5[i]) * f / 5;
}

}  // namespace

int32_t Knob::sine(int32_t degrees) {
  int32_t m = degrees % 360;
  if (m < 0) m += 360;
  return m <= 180 ? quarter_sine(m) : -quarter_sine(m - 180);
}

int32_t Knob::radius() const {
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t r = (w < h ? w : h) / 2 - kMargin;
  return r > 0 ? r : 0;
}

int32_t Knob::angle_of(int32_t value) const {
  const int64_t span = int64_t{max_} - min_;
  if (span <= 0) return -kSweep / 2;
  const int64_t v = value < min_ ? min_ : (value > max_ ? max_ : value);
  return -kSweep / 2 + static_cast<int32_t>(((v - min_) * kSweep * 2 + span) / (span * 2));  // rounded
}

Point Knob::point_at(int32_t degrees, int32_t fraction) const {
  const Point c = center();
  const int64_t reach = int64_t{radius()} * fraction;  // radius * 1000 at most
  return {c.x + static_cast<int32_t>(reach * sine(degrees) / (1000 * 1024)), c.y - static_cast<int32_t>(reach * sine(degrees + 90) / (1000 * 1024))};
}

void Knob::set_range(int32_t minimum, int32_t maximum) {
  UiGuard lock(this);
  min_ = minimum;
  max_ = maximum < minimum ? minimum : maximum;
  invalidate();
  set_value(value_);
}

void Knob::set_value(int32_t value) {
  UiGuard lock(this);
  if (value < min_) value = min_;
  if (value > max_) value = max_;
  if (value == value_) return;
  value_ = value;
  invalidate();
  fire(changed_);
}

void Knob::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  const int32_t r = radius();
  if (r < 4) return;
  const Point c = center();
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  // The tick marks sit outside the dial.
  for (int32_t i = 0; i <= kTicks; ++i) {
    const int32_t degrees = -kSweep / 2 + kSweep * i / kTicks;
    const Point a = point_at(degrees, 900), b = point_at(degrees, 1000);
    painter.line(a.x, a.y, b.x, b.y, (i == 0 || i == kTicks) ? ink : t.shadow);
  }
  // The dial: a dark rim, the face, and a lit edge towards the top left.
  const int32_t body = r * 80 / 100;
  graphics::IRenderer2D& renderer = painter.renderer();
  const int32_t ox = painter.origin_x(), oy = painter.origin_y();
  renderer.fill_circle(c.x + ox, c.y + oy, static_cast<uint32_t>(body), t.dark);
  renderer.fill_circle(c.x + ox, c.y + oy, static_cast<uint32_t>(body > 1 ? body - 1 : 1), t.shadow);
  renderer.fill_circle(c.x + ox - 1, c.y + oy - 1, static_cast<uint32_t>(body > 3 ? body - 3 : 1), t.highlight);
  renderer.fill_circle(c.x + ox, c.y + oy, static_cast<uint32_t>(body > 4 ? body - 4 : 1), t.face);
  // The pointer, two pixels thick.
  const int32_t degrees = angle();
  const Point tip = point_at(degrees, 720);
  painter.line(c.x, c.y, tip.x, tip.y, ink);
  painter.line(c.x + 1, c.y, tip.x + 1, tip.y, ink);
  painter.line(c.x, c.y + 1, tip.x, tip.y + 1, ink);
  if (focused()) painter.focus_frame({0, 0, bounds_.width, bounds_.height});
}

void Knob::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  dragging_ = true;
  press_x_ = args.x;
  press_y_ = args.y;
  press_value_ = value_;
  invalidate();
}

void Knob::on_mouse_move(const MouseArgs& args) {
  if (!dragging_) return;
  const int64_t span = int64_t{max_} - min_;
  const int64_t travel = int64_t{press_y_ - args.y} + (args.x - press_x_);
  int64_t target = press_value_ + travel * span / kDragPixels;
  if (target < min_) target = min_;
  if (target > max_) target = max_;
  set_value(static_cast<int32_t>(target));
}

void Knob::on_mouse_up(const MouseArgs&) {
  dragging_ = false;
  invalidate();
}

void Knob::on_mouse_wheel(const MouseArgs& args) {
  if (args.wheel > 0) set_value(value_ + step_);
  else if (args.wheel < 0) set_value(value_ - step_);
}

bool Knob::on_key(const KeyArgs& args) {
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
