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

// The calculator application: a display and a keypad (digits, + - * /, =, C, Back, +/-, decimal point). Operators are applied as they are
// typed, left to right, like a pocket calculator. Everything works from the keyboard: the digits and the operator keys, Enter or = for equals,
// Backspace, Escape (or Delete) to clear, F9 to change the sign; Tab reaches the keypad's buttons and Space presses the one with the focus.
// Edit > Copy (Ctrl+C) puts the number shown on the clipboard and Edit > Paste (Ctrl+V) takes a number from it; the display's context menu has
// the same two. Like every program window it can be a tab of a tabbed window, dragged out into its own window and back.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/String.hpp"

namespace desktop {

// The display: the number, right-aligned in a sunken field. It is where the focus rests (the frame shows it there), so that the keys typed are
// seen to go to the number and not to one of the keypad's buttons.
class CalculatorDisplay final : public gui::Control {
 public:
  explicit CalculatorDisplay(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}
  void set_text(StringView text) { gui::UiGuard lock(this); text_.assign(text); invalidate(); }
  StringView text() const { return text_.view(); }
  bool focusable() const override { return true; }
  void paint(gui::Painter& painter) override;

 private:
  String text_;
};

// A key of the keypad. Space presses the one with the focus, as on every button; Enter does not: in a calculator Enter is always "=".
class KeypadButton final : public gui::Button {
 public:
  explicit KeypadButton(allocators::IAllocator& allocator) : Button(allocator) {}
  bool on_key(const gui::KeyArgs& args) override;
};

class Calculator : public gui::Frame {
 public:
  static constexpr uint32_t kKeys = 19;

  explicit Calculator(allocators::IAllocator& allocator);

  // What the display shows ("0" at the start, "Cannot divide by zero" after a division by zero).
  StringView display() const { return entry_.view(); }

  // The keypad's actions (also what the keyboard does).
  void press_digit(char digit);   // '0'..'9'
  void press_point();
  void press_operator(char op);   // '+', '-', '*' or '/'
  void press_equals();
  void press_clear();
  void press_backspace();
  void press_negate();
  // Edit > Copy and Paste: the number shown goes to the clipboard; a number on the clipboard becomes the number shown, as if it had been typed.
  // False when there is nothing to copy (the display shows a message), no clipboard, or what is on it is not a number.
  bool copy_display();
  bool paste_number();

  gui::MenuBar& menu_bar() { return menu_; }
  CalculatorDisplay& display_control() { return display_; }
  gui::ContextMenu& display_menu() { return display_menu_; }
  gui::NoticeDialog& about_dialog() { return about_; }
  // The button of a key: a digit, '.', '+', '-', '*', '/', '=', 'C' (clear), 'B' (back) or 'N' (change sign); nullptr for anything else.
  gui::Button* key_button(char key);

  bool on_key(const gui::KeyArgs& args) override;
  void layout() override;

 private:
  static void on_button(void* self, gui::Control& sender);
  static void on_copy(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_menu_opening(void* self, gui::Control&);
  void press(char key);
  void apply_pending();
  void show_value(double value);
  void refresh();

  gui::MenuBar menu_;
  CalculatorDisplay display_;
  KeypadButton buttons_[kKeys];
  gui::ContextMenu display_menu_;
  gui::NoticeDialog about_;
  gui::MenuItem* copy_item_ = nullptr;
  gui::MenuItem* menu_copy_item_ = nullptr;
  gui::MenuItem* paste_item_ = nullptr;
  gui::MenuItem* menu_paste_item_ = nullptr;
  String entry_;          // what the display shows
  double accumulator_ = 0;
  char pending_ = 0;      // the operator waiting for its right-hand side
  bool fresh_ = true;     // the next digit starts a new number
  bool error_ = false;
};

}  // namespace desktop
