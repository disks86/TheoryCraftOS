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

// The calculator application: a display and a keypad (digits, + - * /, =, C, Backspace, +/-, decimal point). Operators are applied as they are
// typed, left to right, like a pocket calculator. The keyboard works too (digits, the operator keys, Enter or = for equals, Backspace, Escape to
// clear). Like every program window it can be a tab of a tabbed window, dragged out into its own window and back.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/String.hpp"

namespace desktop {

class Calculator : public gui::Frame {
 public:
  explicit Calculator(allocators::IAllocator& allocator);

  // What the display shows ("0" at the start, "Error" after a division by zero).
  StringView display() const { return entry_.view(); }

  // The keypad's actions (also what the keyboard does).
  void press_digit(char digit);   // '0'..'9'
  void press_point();
  void press_operator(char op);   // '+', '-', '*' or '/'
  void press_equals();
  void press_clear();
  void press_backspace();
  void press_negate();

  bool on_key(const gui::KeyArgs& args) override;
  void layout() override;

 private:
  static constexpr uint32_t kKeys = 19;
  static void on_button(void* self, gui::Control& sender);
  void press(char key);
  void apply_pending();
  void show_value(double value);
  void refresh();

  gui::Label display_;
  gui::Button buttons_[kKeys];
  String entry_;          // what the display shows
  double accumulator_ = 0;
  char pending_ = 0;      // the operator waiting for its right-hand side
  bool fresh_ = true;     // the next digit starts a new number
  bool error_ = false;
};

}  // namespace desktop
