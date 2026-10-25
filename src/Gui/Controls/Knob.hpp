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

// A rotary knob: a round dial with a pointer that turns through 270 degrees (from 7:30 clockwise to 4:30) to pick an integer between a minimum and a
// maximum (default 0..100), with tick marks at both ends and every tenth of the sweep. Press and drag up or right to turn it up and down or left to turn
// it down: kDragPixels of travel cover the whole range, from the value at the press. Keys: arrows move by the step, Page Up/Down by the page, Home/End go to
// the ends; the mouse wheel moves by the step. No floating point is used (sine comes from a small table).
#pragma once
#include "Gui/Control.hpp"

namespace gui {

class Knob : public Control {
 public:
  explicit Knob(allocators::IAllocator& allocator) : Control(allocator) {}

  int32_t value() const { return value_; }
  int32_t minimum() const { return min_; }
  int32_t maximum() const { return max_; }
  // The value is moved into the new range. A maximum below the minimum is raised to it.
  void set_range(int32_t minimum, int32_t maximum);
  // Clamped to the range. The handler fires only when the value really changes.
  void set_value(int32_t value);
  // How far the arrow keys and the wheel move the value, and how far Page Up/Down do (both at least 1).
  void set_step(int32_t step, int32_t page) {
    UiGuard lock(this);
    step_ = step > 0 ? step : 1;
    page_ = page > 0 ? page : 1;
  }
  // True while the knob is held with the mouse.
  bool dragging() const { return dragging_; }
  void on_value_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // The pointer's angle in degrees clockwise from 12 o'clock for a value: -135 at the minimum, 135 at the maximum.
  int32_t angle_of(int32_t value) const;
  int32_t angle() const { return angle_of(value_); }
  // The dial's centre and radius in this control's coordinates (radius 0 if there is no room).
  Point center() const { return {static_cast<int32_t>(bounds_.width) / 2, static_cast<int32_t>(bounds_.height) / 2}; }
  int32_t radius() const;
  // The point `fraction` (0..1000) of the radius from the centre in the direction of `degrees` (clockwise from 12 o'clock).
  Point point_at(int32_t degrees, int32_t fraction) const;
  // Fixed-point sine of an angle in degrees, scaled by 1024 (any angle, negative too).
  static int32_t sine(int32_t degrees);

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  static constexpr int32_t kMargin = 3, kSweep = 270, kDragPixels = 150, kTicks = 10;

 protected:
  Size default_size() const override { return {48, 48}; }

 private:
  Handler changed_;
  int32_t min_ = 0, max_ = 100, value_ = 0;
  int32_t step_ = 1, page_ = 10;
  bool dragging_ = false;
  int32_t press_y_ = 0, press_x_ = 0, press_value_ = 0;
};

}  // namespace gui
