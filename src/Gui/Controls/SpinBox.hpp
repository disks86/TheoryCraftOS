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

// A number box: a text box for typing an integer with a pair of small buttons beside it that step the value up and down.
// Keys while it has the focus: Up / Down step, Page Up / Page Down step ten times as far; the mouse wheel steps too, and a click on
// the upper or lower button. The value stays between the minimum and maximum (typing a number outside them is corrected when
// the box loses the focus or Enter is pressed). Characters that are not part of a number are ignored.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/TextBox.hpp"

namespace gui {

class SpinBox : public Control {
 public:
  static constexpr int32_t kButtonWidth = 16;

  explicit SpinBox(allocators::IAllocator& allocator);

  int32_t value() const { return value_; }
  int32_t minimum() const { return min_; }
  int32_t maximum() const { return max_; }
  // The value is moved into the new range. A maximum below the minimum is raised to it.
  void set_range(int32_t minimum, int32_t maximum);
  // Clamped to the range. The handler fires only when the value really changes.
  void set_value(int32_t value);
  void set_step(int32_t step) { UiGuard lock(this); step_ = step > 0 ? step : 1; }
  void on_value_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  // The text box inside (for focusing it, or reading what is typed).
  TextBox& edit_box() { return edit_; }
  // Rewrites the box to show the value (what happens when the box loses the focus).
  void normalize();

  // The two buttons, in this control's coordinates (for painting and tests).
  Rect2D up_rect() const;
  Rect2D down_rect() const;

  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override { step_by(args.wheel > 0 ? 1 : args.wheel < 0 ? -1 : 0); }
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {80, 24}; }

 private:
  class Edit final : public TextBox {
   public:
    Edit(allocators::IAllocator& allocator, SpinBox& owner) : TextBox(allocator), owner_(owner) {}
    void on_mouse_wheel(const MouseArgs& args) override { owner_.on_mouse_wheel(args); }
    void on_focus_changed(bool focused) override {
      TextBox::on_focus_changed(focused);
      if (!focused) owner_.normalize();
    }

   private:
    SpinBox& owner_;
  };

  void step_by(int32_t direction);
  void on_text_changed();
  void show_value();
  static void text_changed(void* self, Control&);

  Edit edit_;
  int32_t value_ = 0, min_ = 0, max_ = 100, step_ = 1;
  bool updating_ = false;
  Handler changed_;
};

}  // namespace gui
