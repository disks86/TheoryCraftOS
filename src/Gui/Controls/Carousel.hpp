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

// A carousel: a stack of slides of which one is shown, with an arrow button on each side to step to the previous and next slide and a row of dots
// below, one per slide (the current one filled). Slides are ordinary controls (usually Panels) that the caller owns, like every control; add_slide() puts
// one in the carousel, sets it to fill the slide area and hides it unless it is current. A click on an arrow steps, a click on a dot jumps to that slide,
// Left/Right step and Home/End go to the ends while the carousel has the focus. By default stepping wraps around from the last slide to the first and
// back; set_wrap(false) stops at the ends (the arrow there is greyed). The toolkit has no timers, so automatic advance is driven by the host:
// set_interval() with a time in milliseconds and call tick() with the time that passed; it steps to the next slide each time the interval is up, and
// stands still while the pointer is over the carousel.
#pragma once
#include "Gui/Control.hpp"

namespace gui {

class Carousel : public Control {
 public:
  static constexpr int32_t kArrowWidth = 28, kDotsHeight = 22, kDotSize = 8, kDotSpacing = 16;

  explicit Carousel(allocators::IAllocator& allocator) : Control(allocator), slides_(allocator) {}

  // Adds a slide after the others; the first one added is current. Returns its index or -1.
  int32_t add_slide(Control* slide);
  int32_t slide_count() const { return static_cast<int32_t>(slides_.count()); }
  Control* slide(int32_t index) const { return index >= 0 && static_cast<uint64_t>(index) < slides_.count() ? slides_[static_cast<uint64_t>(index)] : nullptr; }

  int32_t current_slide() const { return current_; }  // -1 while there are no slides
  // Shows another slide (out of range does nothing). The handler fires when the slide changes.
  void set_current_slide(int32_t index);
  void next();
  void previous();
  bool can_step(int32_t direction) const;   // false at an end with wrapping off, or with fewer than two slides
  void set_wrap(bool wrap) { UiGuard lock(this); wrap_ = wrap; invalidate(); }
  bool wrap() const { return wrap_; }
  void on_slide_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // Automatic advance: 0 (the default) switches it off.
  void set_interval(uint32_t milliseconds) { UiGuard lock(this); interval_ = milliseconds; elapsed_ = 0; }
  uint32_t interval() const { return interval_; }
  void tick(uint32_t elapsed_milliseconds);

  // Geometry (for painting and tests), in this control's coordinates.
  Rect2D slide_rect() const;
  Rect2D previous_rect() const;
  Rect2D next_rect() const;
  Rect2D dot_rect(int32_t index) const;
  // The dot at a point (the dot's square, widened to the spacing), or -1.
  int32_t dot_at(int32_t x, int32_t y) const;

  bool focusable() const override { return true; }
  Rect2D client_rect() const override { return slide_rect(); }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {320, 200}; }

 private:
  void step(int32_t direction);

  List<Control*> slides_;
  int32_t current_ = -1;
  bool wrap_ = true;
  uint32_t interval_ = 0, elapsed_ = 0;
  Handler changed_;
};

}  // namespace gui
