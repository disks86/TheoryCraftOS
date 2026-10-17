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

// A scroll bar: an arrow button at each end, a track between them and a thumb whose length shows how much
// of the content is in view. It describes a window of `page` units onto `total` units of content; the
// value is where the window starts, from 0 to total - page. Arrows move by the step, a click on the track by
// a page, the thumb is dragged, and the mouse wheel moves by three steps. Not focusable (the control it
// scrolls handles the keys).
class ScrollBar : public Control {
 public:
  static constexpr int32_t kThickness = 16, kMinThumb = 12;

  explicit ScrollBar(allocators::IAllocator& allocator, Orientation orientation = Orientation::Vertical)
      : Control(allocator), orientation_(orientation) {}

  // The content is `total` units long and `page` of them show at once. The value stays inside the new range.
  void set_metrics(int32_t total, int32_t page);
  int32_t total() const { return total_; }
  int32_t page() const { return page_; }
  // The largest value: total - page, or 0 when everything fits.
  int32_t maximum_value() const { return total_ > page_ ? total_ - page_ : 0; }
  int32_t value() const { return value_; }
  // Clamped to 0 .. maximum_value(). The handler fires only when the value really changes.
  void set_value(int32_t value);
  void set_step(int32_t step) { UiGuard lock(this); step_ = step > 0 ? step : 1; }
  void on_value_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  Orientation orientation() const { return orientation_; }

  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;

  // The thumb in this control's coordinates (empty when everything fits); for painting and tests.
  Rect2D thumb_rect() const;

 protected:
  Size default_size() const override {
    return orientation_ == Orientation::Vertical ? Size{kThickness, 100} : Size{100, kThickness};
  }

 private:
  enum class Part : uint8_t { None, Before, After, TrackBefore, TrackAfter, Thumb };

  int32_t length() const { return static_cast<int32_t>(orientation_ == Orientation::Vertical ? bounds_.height : bounds_.width); }
  int32_t track_length() const { const int32_t t = length() - 2 * kThickness; return t > 0 ? t : 0; }
  int32_t thumb_length() const;
  int32_t thumb_offset() const;  // from the start of the track
  Part part_at(const MouseArgs& args) const;
  int32_t along(const MouseArgs& args) const { return orientation_ == Orientation::Vertical ? args.y : args.x; }
  void drag_to(const MouseArgs& args);
  void draw_arrow(Painter& painter, const Rect2D& button, bool toward_start, bool pressed);

  Orientation orientation_;
  int32_t total_ = 0, page_ = 0, value_ = 0, step_ = 1;
  Handler changed_;
  Part pressed_ = Part::None;
  int32_t grab_ = 0;
};

}  // namespace gui
