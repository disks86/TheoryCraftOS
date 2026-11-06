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

// The desktop's view of the Windows programs (desktop::IWindowsSession) over win32k: the screen surface of GDI (told of every change through the screen sink) and the
// window manager's input (user::inject_*). The desktop's keyboard speaks USB HID usages; here they become the virtual keys and set-1 scan codes of Windows (US layout)
// and, for a press, the character it types. Every call takes the system lock itself; screen_changed only notes the change (the lock is held there).
#pragma once
#include "Desktop/WindowsSession.hpp"
#include "Process/Win32k/Win32kGdi.hpp"

namespace process::win32k {

// A key of the USB HID keyboard page as Windows names it: the virtual key (the left / right one for the modifiers), the set-1 scan code and the E0 prefix.
struct WindowsKey {
  uint32_t vk = 0, scan = 0;
  bool extended = false;
};
bool windows_key_of_usage(uint8_t usage, WindowsKey* out);      // false for a usage with no key of Windows

class DesktopBridge final : public desktop::IWindowsSession, public gdi::IScreenSink {
 public:
  // Becomes win32k's screen sink (detach() or the destructor stops it).
  void attach();
  void detach();
  ~DesktopBridge() { detach(); }

  bool take_screen_changed() override { return __atomic_exchange_n(&changed_, false, __ATOMIC_ACQ_REL); }
  uint32_t screen_width() override;
  uint32_t screen_height() override;
  bool set_screen_size(uint32_t width, uint32_t height) override;
  bool copy_screen(ArrayView<uint32_t> out, uint32_t* width, uint32_t* height) override;
  void key(uint8_t hid_usage, bool pressed, uint8_t modifiers) override;
  void mouse_motion(int32_t dx, int32_t dy) override;
  void mouse_position(int32_t x, int32_t y) override;
  void mouse_button(uint32_t button, bool down) override;
  void mouse_wheel(int32_t notches) override;
  void cursor(int32_t* x, int32_t* y) override;
  bool cursor_confined() override;

  void screen_changed(const gdi::Rect& dirty) override;

 private:
  bool changed_ = false;
  bool attached_ = false;
};

}  // namespace process::win32k
