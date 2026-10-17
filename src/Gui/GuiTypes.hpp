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

// Small value types shared by the GUI toolkit.
#pragma once
#include <stdint.h>
#include "Graphics/GraphicsTypes.hpp"

namespace gui {

using graphics::Rect2D;

struct Point {
  int32_t x = 0, y = 0;
};

struct Size {
  int32_t width = 0, height = 0;
};

struct Insets {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  static constexpr Insets all(int32_t v) { return {v, v, v, v}; }
};

// How a control is placed in its parent's client area (see Control::layout()).
enum class DockStyle : uint8_t {
  None,    // keeps the bounds it was given (absolute position in the parent)
  Top,     // full width, its own height, against the top edge of what is left
  Bottom,  // full width, its own height, against the bottom edge
  Left,    // full height, its own width, against the left edge
  Right,   // full height, its own width, against the right edge
  Fill,    // everything that is left
};

enum class Orientation : uint8_t { Horizontal, Vertical };

enum class MouseButton : uint8_t { None, Left, Right, Middle };

// Coordinates are relative to the control that receives the event.
struct MouseArgs {
  int32_t x = 0, y = 0;
  MouseButton button = MouseButton::None;
  int32_t wheel = 0;
};

struct KeyArgs {
  uint8_t usage = 0;      // USB HID usage (Hardware/Keyboard/KeyCodes.hpp)
  char character = 0;     // what the key types with the modifiers held, 0 if nothing
  uint8_t modifiers = 0;  // modifier:: bits
};

class Control;

// A callback without std::function: a plain function and the pointer it is given back.
//
// Where it runs: a control that fires a handler (a click, a changed text, a chosen menu item) hands it to
// its Window, and when the window has a handler thread (Window::start_handler_thread()) the call runs
// there - never on the input thread and never under the UI lock - so a slow handler cannot freeze
// painting or input. Handlers of one window run one at a time in the order they were fired.
// A handler may change controls from its thread: every property setter is thread-safe. Without a handler
// thread, or with `inline_call` set (the toolkit's own plumbing), the handler runs immediately in the
// firing thread instead.
struct Handler {
  void (*function)(void* context, Control& sender) = nullptr;
  void* context = nullptr;
  bool inline_call = false;  // run immediately in the firing thread, even if the window has a handler thread

  void operator()(Control& sender) const {
    if (function) function(context, sender);
  }
  explicit operator bool() const { return function != nullptr; }
  static Handler immediate(void (*function)(void*, Control&), void* context) { return {function, context, true}; }
};

inline bool contains(const Rect2D& r, int32_t x, int32_t y) {
  return x >= r.x && y >= r.y && x < r.x + static_cast<int64_t>(r.width) && y < r.y + static_cast<int64_t>(r.height);
}

inline Rect2D intersect(const Rect2D& a, const Rect2D& b) {
  const int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
  const int64_t ax1 = a.x + static_cast<int64_t>(a.width), bx1 = b.x + static_cast<int64_t>(b.width);
  const int64_t ay1 = a.y + static_cast<int64_t>(a.height), by1 = b.y + static_cast<int64_t>(b.height);
  const int64_t x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
  return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0,
          y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
}

}  // namespace gui
