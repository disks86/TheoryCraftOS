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

#pragma once
#include "Gui/Control.hpp"

namespace gui {

// A track with a thumb that is dragged to pick an integer between a minimum and a maximum
// (default 0..100). Horizontal sliders grow to the right, vertical ones upwards. A click on the
// track moves the thumb there and starts a drag. Keys: arrows move by the step, Page Up/Down by the
// page, Home/End go to the ends; the mouse wheel moves by the step.
class Slider : public Control {
 public:
  explicit Slider(allocators::IAllocator& allocator, Orientation orientation = Orientation::Horizontal)
      : Control(allocator), orientation_(orientation) {}

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
  Orientation orientation() const { return orientation_; }
  // True while the thumb is held with the mouse.
  bool dragging() const { return dragging_; }
  void on_value_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  static constexpr int32_t kThumbLength = 11, kThumbThickness = 21;

  // The thumb's rectangle in this control's coordinates (for painting and tests).
  Rect2D thumb_rect() const;

 protected:
  Size default_size() const override {
    return orientation_ == Orientation::Horizontal ? Size{120, kThumbThickness + 3} : Size{kThumbThickness + 3, 120};
  }

 private:
  int32_t length() const { return static_cast<int32_t>(orientation_ == Orientation::Horizontal ? bounds_.width : bounds_.height); }
  int32_t travel() const;  // pixels the thumb can move
  int32_t position_of(int32_t value) const;
  int32_t value_at(int32_t pixel) const;  // the value whose thumb starts at `pixel`
  void drag_to(const MouseArgs& args);

  Orientation orientation_;
  int32_t min_ = 0, max_ = 100, value_ = 0;
  int32_t step_ = 1, page_ = 10;
  Handler changed_;
  bool dragging_ = false;
  int32_t grab_ = 0;  // where in the thumb it was picked up
};

}  // namespace gui
