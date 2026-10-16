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
#include "Utility/String.hpp"

namespace gui {

// A push button: pressed with the left mouse button (the click happens when it is released over the
// button) or with Space/Enter while it has the focus.
class Button : public Control {
 public:
  explicit Button(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}
  void set_text(StringView text) { text_.assign(text); invalidate(); }
  StringView text() const { return text_.view(); }
  void on_click(Handler handler) { click_ = handler; }
  // Does what a click does (also used by keyboard handling and by tests).
  void click() { if (enabled()) click_(*this); }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_leave() override {}
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {Painter::text_width(text_.view()) + 24, 24}; }

 private:
  bool inside(const MouseArgs& a) const { return contains({0, 0, bounds_.width, bounds_.height}, a.x, a.y); }

  String text_;
  Handler click_;
  bool held_ = false;     // the left button went down on us and is still down
  bool pressed_ = false;  // held_ and the pointer is over us
};

}  // namespace gui
