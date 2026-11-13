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

// Feeds a Window from the keyboard and mouse drivers: key presses become KeyArgs (with the
// character the key types, and the modifier keys that were held when it went down - taken from the order of the key events, not
// from the keyboard's state at the moment they are read, so a quick Ctrl+A is still Ctrl+A), mouse movement and buttons become window-coordinate events. A relative
// mouse moves a cursor position kept here (clamped to the window); an absolute one (tablet) is scaled
// to the window. Touches (set_touch) go to the touch tap first; a contact it does not take moves the pointer as a mouse would (the first such finger
// down is the left button where it is, until it lifts), so the desktop's own windows and programs that know nothing of touches work with fingers.
#pragma once
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"
#include "Hardware/Pen/IPen.hpp"
#include "Hardware/Touch/ITouch.hpp"

namespace gui {

class InputPump {
 public:
  // Either device may be null.
  InputPump(Window& window, hardware::IKeyboard* keyboard, hardware::IMouse* mouse)
      : window_(window), keyboard_(keyboard), mouse_(mouse), modifiers_(keyboard ? keyboard->modifiers() : 0) {
    x_ = static_cast<int32_t>(window.bounds().width / 2);
    y_ = static_cast<int32_t>(window.bounds().height / 2);
    window_.mouse_move(x_, y_);  // the cursor starts in the middle, not at the corner
  }

  // A function that keeps the pointer where it may be (on a monitor, when the screen is several): given the position the pointer is about to be at,
  // it moves it to the nearest allowed place. The window's own edge always limits it too.
  using Confine = void (*)(void* context, int32_t* x, int32_t* y);
  void set_confine(Confine confine, void* context) {
    confine_ = confine;
    confine_context_ = context;
  }
  // Taps: told every device event before it becomes window events - key releases too, and a relative mouse's motion as it was (the Windows programs' bridge needs both).
  // The mouse tap gets the position the pointer is about to move to (window coordinates, kept inside the window and confined) and may change it.
  using KeyTap = void (*)(void* context, const hardware::KeyEvent& event, uint8_t modifiers);
  using MouseTap = void (*)(void* context, const hardware::MouseEvent& event, int32_t* x, int32_t* y);
  void set_taps(KeyTap key, MouseTap mouse, void* context) {
    key_tap_ = key;
    mouse_tap_ = mouse;
    tap_context_ = context;
  }
  // The touch devices (null: none), and the tap that is offered each touch event first, at its place in window coordinates: true when it took it (a
  // program's window that takes touches), else the contact moves the pointer. A contact is offered on every event (the tap keeps what it took).
  using TouchTap = bool (*)(void* context, const hardware::TouchEvent& event, int32_t x, int32_t y);
  void set_touch(hardware::ITouch* touch, TouchTap tap, void* context) {
    touch_ = touch;
    touch_tap_ = tap;
    touch_context_ = context;
  }
  // The pens (null: none), and the tap offered each pen event first (true: a program's window took it). Events it does not take move the pointer as
  // hardware::PenMouse would (hovering moves it, the tip is the left button - the eraser end's the middle one - and a barrel button the right one).
  using PenTap = bool (*)(void* context, const hardware::PenEvent& event, int32_t x, int32_t y);
  void set_pen(hardware::IPen* pen, PenTap tap, void* context) {
    pen_ = pen;
    pen_tap_ = tap;
    pen_context_ = context;
  }
  // Delivers everything the devices have right now; returns how many events there were.
  uint32_t pump();
  Point cursor() const { return {x_, y_}; }
  // Puts the pointer somewhere (kept inside the window and the confine function's places): for a window that changed its size.
  void set_cursor(int32_t x, int32_t y);

 private:
  void deliver(const hardware::MouseEvent& m);

  Window& window_;
  hardware::IKeyboard* keyboard_;
  hardware::IMouse* mouse_;
  int32_t x_ = 0, y_ = 0;
  Confine confine_ = nullptr;
  void* confine_context_ = nullptr;
  KeyTap key_tap_ = nullptr;
  MouseTap mouse_tap_ = nullptr;
  void* tap_context_ = nullptr;
  uint8_t buttons_ = 0;
  uint8_t modifiers_ = 0;  // held after the last key event delivered
  hardware::ITouch* touch_ = nullptr;
  TouchTap touch_tap_ = nullptr;
  void* touch_context_ = nullptr;
  bool emulating_ = false;       // a contact nobody took is the left button
  uint32_t emulated_id_ = 0;
  hardware::IPen* pen_ = nullptr;
  PenTap pen_tap_ = nullptr;
  void* pen_context_ = nullptr;
  bool pen_in_range_ = false;    // the pen as the pointer (what was last delivered for it)
  uint8_t pen_buttons_ = 0;
  uint32_t pen_x_ = 0, pen_y_ = 0, pen_max_x_ = 0, pen_max_y_ = 0;
};

}  // namespace gui
