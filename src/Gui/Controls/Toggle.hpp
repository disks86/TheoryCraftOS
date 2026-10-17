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

// An on/off switch: a small track with a knob that sits at the left when off and at the right (on a
// selection-coloured track) when on, with an optional caption to its right. A click (released over the
// control) or Space flips it. Unlike a CheckBox it takes effect at once, like a light switch.
class Toggle : public Control {
 public:
  static constexpr int32_t kTrackWidth = 36, kTrackHeight = 18, kGap = 8;

  explicit Toggle(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}
  void set_text(StringView text) { UiGuard lock(this); text_.assign(text); invalidate(); }
  StringView text() const { return text_.view(); }
  bool on() const { return on_; }
  // The handler fires only when the state really changes.
  void set_on(bool on);
  void flip() { if (enabled()) set_on(!on_); }
  void on_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override {
    return {kTrackWidth + (text_.size() > 0 ? kGap + Painter::text_width(text_.view()) + 4 : 0), kTrackHeight + 4};
  }

 private:
  bool inside(const MouseArgs& a) const { return contains({0, 0, bounds_.width, bounds_.height}, a.x, a.y); }

  String text_;
  Handler changed_;
  bool on_ = false;
  bool held_ = false, pressed_ = false;
};

}  // namespace gui
