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

// What the desktop's "Windows" window (WindowsScreen.hpp) needs of the Windows programs: their screen and their input. The kernel implements it over win32k
// (src/Process/Win32k/Win32kDesktopBridge.cpp); the desktop's tests with a fake.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace desktop {

class IWindowsSession {
 public:
  // True when the screen changed since the last call (and forgets it).
  virtual bool take_screen_changed() = 0;
  virtual uint32_t screen_width() = 0;
  virtual uint32_t screen_height() = 0;
  // Makes the screen this size (what was drawn is lost: the desktop does it once, before the programs start, to fit the screen in its window).
  virtual bool set_screen_size(uint32_t width, uint32_t height) = 0;
  // Copies the screen (0xAARRGGBB, opaque, rows top to bottom, pitch = width) into `out`; false when `out` holds fewer than width * height pixels.
  virtual bool copy_screen(ArrayView<uint32_t> out, uint32_t* width, uint32_t* height) = 0;
  // Input, in the screen's coordinates.
  virtual void key(uint8_t hid_usage, bool pressed, uint8_t modifiers) = 0;   // modifiers: hardware::modifier bits held (for the character a press types)
  virtual void mouse_motion(int32_t dx, int32_t dy) = 0;
  virtual void mouse_position(int32_t x, int32_t y) = 0;
  virtual void mouse_button(uint32_t button, bool down) = 0;                  // 1 left, 2 right, 3 middle
  virtual void mouse_wheel(int32_t notches) = 0;                              // + away from the user
  virtual void cursor(int32_t* x, int32_t* y) = 0;                            // where the Windows cursor is
  virtual bool cursor_confined() = 0;                                         // a program keeps the cursor in a rectangle (ClipCursor): the pointer stays with it

 protected:
  IWindowsSession() = default;
  ~IWindowsSession() = default;
};

}  // namespace desktop
