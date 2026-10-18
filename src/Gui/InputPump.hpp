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
// to the window.
#pragma once
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"

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
  // Delivers everything the devices have right now; returns how many events there were.
  uint32_t pump();
  Point cursor() const { return {x_, y_}; }

 private:
  Window& window_;
  hardware::IKeyboard* keyboard_;
  hardware::IMouse* mouse_;
  int32_t x_ = 0, y_ = 0;
  Confine confine_ = nullptr;
  void* confine_context_ = nullptr;
  uint8_t buttons_ = 0;
  uint8_t modifiers_ = 0;  // held after the last key event delivered
};

}  // namespace gui
