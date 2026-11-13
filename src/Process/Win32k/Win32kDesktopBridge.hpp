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

// The desktop's view of the Windows programs (desktop::IWindowsSession) over win32k. Attached, win32k redirects its top-level windows: each one draws on a window
// surface of its own, which the desktop draws straight into its picture (draw_window, under the system lock, with no copy in between), and the desktop's frames are
// the windows' frames (user::set_redirection). The window manager's observer and GDI's surface sink tell the bridge what changed; they run with the system lock held, so
// they only note it, in a fixed table guarded by that same lock (no allocation there; a full table notes "everything changed" instead).
// The desktop's keyboard speaks USB HID usages; here they become the virtual keys and set-1 scan codes of Windows (US layout) and, for a press, the character it types.
// Every other call takes the system lock itself.
#pragma once
#include "Desktop/WindowsSession.hpp"
#include "Process/Win32k/Win32kGdi.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Utility/UniquePointer.hpp"

namespace process::win32k {

// A key of the USB HID keyboard page as Windows names it: the virtual key (the left / right one for the modifiers), the set-1 scan code and the E0 prefix.
struct WindowsKey {
  uint32_t vk = 0, scan = 0;
  bool extended = false;
};
bool windows_key_of_usage(uint8_t usage, WindowsKey* out);      // false for a usage with no key of Windows

class DesktopBridge final : public desktop::IWindowsSession, public user::IWindowObserver, public gdi::IWindowSurfaceSink {
 public:
  // Stops the redirection (if attach() started it); the destructor does too.
  void detach();
  ~DesktopBridge() { detach(); }

  void attach(int32_t frame_border, int32_t frame_title) override;
  void set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) override;
  void take_changes(List<uint32_t>* changed, List<Repaint>* repainted) override;
  uint32_t take_drag(uint32_t* edge) override;
  void windows(List<uint32_t>* out) override;
  bool window_info(uint32_t window, desktop::WindowsWindowInfo* out, String* title) override;
  bool draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) override;
  void move_window(uint32_t window, const desktop::WindowsRect& rect) override;
  void activate_window(uint32_t window) override;
  void window_command(uint32_t window, Command command) override;
  void key(uint8_t hid_usage, bool pressed, uint8_t modifiers) override;
  void mouse_motion(int32_t dx, int32_t dy) override;
  void mouse_position(int32_t x, int32_t y) override;
  void mouse_button(uint32_t button, bool down) override;
  void mouse_wheel(int32_t notches) override;
  void cursor(int32_t* x, int32_t* y) override;
  bool cursor_confined() override;
  uint32_t capture_window() override;
  void pointer(Pointer* out) override;
  bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) override;
  // HiDPI: a DPI-aware program's window (its surface in device pixels) is drawn 1:1; an unaware one's is left to the desktop to enlarge.
  bool draw_window_scaled(uint32_t window, graphics::IRenderer2D& device, int32_t x, int32_t y, uint32_t scale, uint32_t* width, uint32_t* height) override;
  // The desktop's drag of a file over a program's window: taken by a window that accepts files (WS_EX_ACCEPTFILES) under the pointer, which gets
  // WM_DROPFILES with the file's Windows name (filesystem::layout::windows_view_of: C:\..., the user's files under C:\Users\Public\Desktop) when it
  // is let go. Text, and files Windows programs cannot see, are not taken.
  // A finger over a program's window: taken by the window under it (WM_POINTER*, or WM_TOUCH; Win32kUserTouch.cpp).
  bool touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) override;
  bool desktop_drag(uint32_t window, int32_t x, int32_t y, StringView text) override;
  bool desktop_drop() override;
  void desktop_drag_end() override;

  // (with the system lock held)
  void top_level_changed(uint32_t hwnd) override;
  void surface_changed(uint32_t window, const gdi::Rect& dirty) override;
  void drag_requested(uint32_t hwnd, uint32_t edge) override;

 private:
  struct Pending {
    uint32_t window = 0;
    bool changed = false, repaint = false;
    gdi::Rect dirty;
  };
  static constexpr uint32_t kMaxPending = 128;
  Pending* pending_slot(uint32_t window);
  bool fill_info(const user::TopLevelInfo& info, desktop::WindowsWindowInfo* out) const;
  gdi::Rect up(const gdi::Rect& r) const;        // desktop units -> win32k's device pixels
  gdi::Rect down(const gdi::Rect& r) const;      // and back (floor)      // (lock held) the window's entry, a new one, or null when the table is full (overflow_ is then set)

  Pending pending_[kMaxPending];
  uint32_t pending_count_ = 0;
  bool overflow_ = false;
  uint32_t drag_window_ = 0, drag_edge_ = 0;      // (guarded by the system lock too)
  // the desktop's drag (the desktop's input thread only): where it would be dropped, and the name (NUL-terminated, then a NUL)
  user::DropTarget drop_target_;
  bool drop_ok_ = false;
  UniquePointer<WideString> drop_names_;
  bool attached_ = false;
  // HiDPI (DPI virtualization): device pixels per desktop unit, and the desktop's frame in units (win32k gets it in device pixels)
  uint32_t scale_ = 100;      // (percent)
  int32_t frame_border_ = 0, frame_title_ = 0;
};

}  // namespace process::win32k
