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

// DPI virtualization (see Win32kUserInternal.hpp). The device scale comes from the desktop (DesktopBridge::set_display: its monitors' scale): win32k's
// screen, monitors, windows and cursor are in device pixels. A window remembers whether the thread that made it was DPI aware (Window::dpi_virtual);
// what reaches an unaware program or its windows is divided by the scale, what comes from them multiplied. The system DPI is 96 x the scale, which aware
// programs see (thread_dpi); unaware ones see 96.
#include "Process/Win32k/Win32kUserInternal.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Process/Win32k/Win32kUserMisc.hpp"

namespace process::win32k::user {

namespace {
uint32_t g_device_scale = 100;      // (percent)
}  // namespace

uint32_t device_scale() { return g_device_scale; }

void set_device_scale(uint32_t scale) {
  g_device_scale = graphics::clamp_scale_percent(scale);
  user_misc::set_system_dpi(96 * g_device_scale / 100);
}

uint32_t window_scale(const Window* w) { return w && w->dpi_virtual ? g_device_scale : 100; }

bool thread_unaware(Call& call) { return user_misc::dpi_awareness(call.tid(), call.pid()) == 0; }

uint32_t caller_scale(Call& call) { return g_device_scale > 100 && thread_unaware(call) ? g_device_scale : 100; }
uint32_t caller_dpi_scale(Call& call) { return caller_scale(call); }
gdi::Rect rect_for_caller(Call& call, const gdi::Rect& r) { return dpi_down(r, caller_scale(call)); }

namespace {
struct LockGuard {
  ObjectManager* objects;
  LockGuard() : objects(system_objects()) { if (objects) objects->sync().lock(); }
  ~LockGuard() { if (objects) objects->sync().unlock(); }
};
}  // namespace

uint32_t device_scale_locked() { return g_device_scale; }

void set_device_scale_from_desktop(uint32_t scale) {
  List<uint32_t> tops(*system_allocator());
  uint32_t before = 100, after = 100;
  {
    LockGuard g;
    before = g_device_scale;
    set_device_scale(scale);
    after = g_device_scale;
    if (before == after || !state_or_null()) return;
    const Desktop* d = default_desktop();
    if (d && d->root)
      for (Window* c = d->root->first_child; c; c = c->next) tops.push_back(c->handle);
  }
  // each top-level keeps its place in desktop units: its device rectangle times after / before (through the program, as a move of the desktop's would)
  for (uint32_t hwnd : tops.view()) {
    gdi::Rect r;
    {
      LockGuard g;
      Window* w = find_window(hwnd);
      if (!w) continue;
      r = window_rect_screen(w);
    }
    auto conv = [&](int32_t v) { return static_cast<int32_t>(int64_t{v} * after / before); };
    move_top_level(hwnd, {conv(r.left), conv(r.top), conv(r.right), conv(r.bottom)});
  }
}

}  // namespace process::win32k::user
