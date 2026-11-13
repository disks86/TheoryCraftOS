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

// What the desktop needs of the Windows programs to show their windows as its own (Win32Windows.hpp) and hand them its input. The kernel implements it over win32k
// (src/Process/Win32k/Win32kDesktopBridge.cpp); the desktop's tests with a fake.
//
// One coordinate space: win32k's screen is the desktop's window manager area (attach() makes it so), so a window's rectangle is where the desktop's frame for it is,
// and the program's picture of the inside of that frame (its surface) is drawn by the desktop straight from the kernel's memory, with no copy in between.
#pragma once
#include <stdint.h>
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Hardware/Touch/ITouch.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

struct WindowsRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

// A top-level window of a Windows program as the desktop shows it.
struct WindowsWindowInfo {
  bool exists = false, visible = false, minimized = false, maximized = false, active = false;
  bool caption = false;                  // the desktop draws its title bar and frame (else the program draws all of it)
  bool sizable = false, minimize_box = false, maximize_box = false;
  bool tool = false, topmost = false;    // a tool window (no taskbar button), above the others
  bool popup = false;                    // no caption and not activatable from the desktop's side (menus, tooltips, drop-downs)
  bool owned = false;                    // it has an owner window (a dialog): no taskbar button of its own
  uint32_t icon = 0;                     // the icon to show (a handle cursor_picture takes), 0: none
  WindowsRect window;                    // the whole window = the desktop's frame
  WindowsRect surface;                   // the program's picture inside it
};

class IWindowsSession {
 public:
  enum class Command : uint8_t { Close, Minimize, Maximize, Restore };

  // Starts showing the programs' windows as the desktop's: windows with a caption get the desktop's frame (border and title bar heights: the surface of such a window
  // starts that far in). Done once, before set_display.
  virtual void attach(int32_t frame_border, int32_t frame_title) = 0;
  // Makes win32k's screen the desktop's window manager area (width x height) and its monitors these (rectangles and work areas in that area, the primary first;
  // at most 8). Done at the start and whenever the desktop's monitors change.
  struct Monitor {
    WindowsRect rect, work;
  };
  virtual void set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) = 0;
  // What changed since the last call (and forgets it): the windows whose state or place changed (shown, hidden, moved, sized, retitled, restyled, activated,
  // destroyed), and those whose picture changed (with the part, in surface coordinates).
  struct Repaint {
    uint32_t window = 0;
    WindowsRect dirty;
  };
  virtual void take_changes(List<uint32_t>* changed, List<Repaint>* repainted) = 0;
  // A program asked for its window to be moved (edge 9) or sized (edge 1 .. 8: left, right, top, top left, top right, bottom, bottom left, bottom right) by the
  // user with the mouse (a custom title bar or border): the window, and forgets it; 0 when none.
  virtual uint32_t take_drag(uint32_t* edge) = 0;
  // A program asked for its window's window menu (Wayland's xdg_toplevel.show_window_menu: a client that draws its own title bar) at (x, y) of the window's
  // client area since the last call: that window (0: none asked). The desktop opens the frame's menu there.
  virtual uint32_t take_window_menu(int32_t* x, int32_t* y) { (void)x, (void)y; return 0; }
  // The visible top-level windows, bottom of the stack first.
  virtual void windows(List<uint32_t>* out) = 0;
  // False when the window is gone.
  virtual bool window_info(uint32_t window, WindowsWindowInfo* out, String* title) = 0;      // (the title in UTF-8)
  // Draws the window's surface with its top left at (x, y) of `renderer` (the renderer's clip applies), from the kernel's memory as it is, and says its size.
  // False when there is none.
  virtual bool draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) = 0;
  // What the user did to the desktop's frame: the program is asked (it hears of it as it would on Windows) and the result comes back as a change.
  virtual void move_window(uint32_t window, const WindowsRect& rect) = 0;
  virtual void activate_window(uint32_t window) = 0;      // 0: none of the Windows programs' windows is active (a desktop window is)
  virtual void window_command(uint32_t window, Command command) = 0;

  // The cursor the programs show where the Windows cursor is: hidden, a shape of the system's (`system_id`: OCR_NORMAL 32512, OCR_IBEAM 32513, ...), or `cursor`, a
  // handle whose picture cursor_picture gives (the desktop keeps it while the handle stays the same).
  struct Pointer {
    bool hidden = false;
    uint32_t cursor = 0, system_id = 32512;
  };
  virtual void pointer(Pointer* out) = 0;
  // The picture of a cursor: a system shape (`system_id` set, `argb` empty) or 0xAARRGGBB pixels (straight alpha, top row first) with the hot spot.
  virtual bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) = 0;

  // Input, in the screen's coordinates.
  virtual void key(uint8_t hid_usage, bool pressed, uint8_t modifiers) = 0;   // modifiers: hardware::modifier bits held (for the character a press types)
  virtual void mouse_motion(int32_t dx, int32_t dy) = 0;
  virtual void mouse_position(int32_t x, int32_t y) = 0;
  virtual void mouse_button(uint32_t button, bool down) = 0;                  // 1 left, 2 right, 3 middle
  virtual void mouse_wheel(int32_t notches) = 0;                              // + away from the user
  virtual void mouse_left() {}
  // A finger on the screen, in the programs' screen coordinates (as the mouse's). Down names the session's window under it; the session answers
  // whether it takes the contact (its program reads touches: Wayland's wl_touch). A contact taken at Down gets its Motion and its Up (or Cancel) wherever
  // they are; one refused moves the pointer instead (the desktop emulates a mouse with it). By default none is taken.
  virtual bool touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) {
    (void)phase, (void)id, (void)window, (void)x, (void)y;
    return false;
  }                                                // the pointer went to a window that is not one of the programs' (nothing holds it)
  virtual void cursor(int32_t* x, int32_t* y) = 0;                            // where the Windows cursor is
  virtual bool cursor_confined() = 0;                                         // a program keeps the cursor in a rectangle (ClipCursor): the pointer stays with it
  virtual uint32_t capture_window() = 0;                                      // the window that captured the mouse (SetCapture), 0: none

 protected:
  IWindowsSession() = default;
  ~IWindowsSession() = default;
};

}  // namespace desktop
