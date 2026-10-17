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

// A box that is ticked or not, with a caption to its right. Clicking the box or the caption (the click
// counts when the button is released over the control) or pressing Space toggles it.
class CheckBox : public Control {
 public:
  static constexpr int32_t kBox = 13, kGap = 6;

  explicit CheckBox(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}
  void set_text(StringView text) { UiGuard lock(this); text_.assign(text); invalidate(); }
  StringView text() const { return text_.view(); }
  bool checked() const { return checked_; }
  // The handler fires only when the state really changes.
  void set_checked(bool checked);
  void toggle() { if (enabled()) set_checked(!checked_); }
  void on_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {kBox + kGap + Painter::text_width(text_.view()) + 4, 20}; }

 private:
  bool inside(const MouseArgs& a) const { return contains({0, 0, bounds_.width, bounds_.height}, a.x, a.y); }

  String text_;
  Handler changed_;
  bool checked_ = false;
  bool held_ = false, pressed_ = false;
};

}  // namespace gui
