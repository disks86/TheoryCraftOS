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

class RadioButton;

// The buttons that exclude one another: checking one unchecks the others of its group. A group is not a
// control; it just links the buttons that were given to it with RadioButton::set_group(), in that order,
// and wherever they sit in the tree. It must outlive its buttons (declare it before them).
class RadioGroup {
 public:
  RadioGroup() = default;
  RadioGroup(const RadioGroup&) = delete;
  RadioGroup& operator=(const RadioGroup&) = delete;

  uint32_t count() const;
  RadioButton* at(uint32_t index) const;
  // The checked button, or nullptr if none is.
  RadioButton* selected() const;
  // Its position in the group, or -1.
  int32_t selected_index() const;
  // Checks the button at `index` (does nothing if there is none).
  void select(uint32_t index);

 private:
  friend class RadioButton;
  RadioButton* first_ = nullptr;
};

// A round button with a caption that is part of a group of which at most one is checked. Clicking
// (released over the control) or Space checks it - it cannot be unchecked by the user, only by checking
// another one. Arrow keys move the check to the previous / next button of the group, and the focus with it.
class RadioButton : public Control {
 public:
  static constexpr int32_t kCircle = 13, kGap = 6;

  explicit RadioButton(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}
  ~RadioButton() { set_group(nullptr); }

  void set_text(StringView text) { UiGuard lock(this); text_.assign(text); invalidate(); }
  StringView text() const { return text_.view(); }
  // Joins a group (leaving the one it was in). Without a group it works on its own: once checked it stays so.
  void set_group(RadioGroup* group);
  RadioGroup* group() const { return group_; }
  bool checked() const { return checked_; }
  // Checking unchecks the rest of the group (their handlers fire too); unchecking is allowed here, for code.
  void set_checked(bool checked);
  // Fires whenever this button's own state changes, checked or unchecked.
  void on_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {kCircle + kGap + Painter::text_width(text_.view()) + 4, 20}; }

 private:
  friend class RadioGroup;
  bool inside(const MouseArgs& a) const { return contains({0, 0, bounds_.width, bounds_.height}, a.x, a.y); }
  void step(int direction);

  String text_;
  Handler changed_;
  RadioGroup* group_ = nullptr;
  RadioButton* next_ = nullptr;  // the next button of the group
  bool checked_ = false;
  bool held_ = false, pressed_ = false;
};

}  // namespace gui
