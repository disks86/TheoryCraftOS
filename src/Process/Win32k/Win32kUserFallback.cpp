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
// Weak stand-ins for the parts of the GDI and user-misc modules the window manager calls (Win32kGdi.hpp, Win32kUser.hpp). The real modules define the same functions as
// ordinary (strong) definitions, which replace these at link time; they exist so a build or a host test of the window manager alone links. They keep just enough state to be
// consistent (device contexts are numbered, regions hold rectangles) and answer the Windows 10 metrics the window manager needs.
#include "Process/Win32k/Win32kUser.hpp"

#define TCOS_WEAK __attribute__((weak))

namespace process::win32k {

namespace gdi {

namespace {
struct FallbackDc { uint32_t handle = 0; bool used = false; };
constexpr uint32_t kDcs = 256, kRegions = 128, kRegionRects = 32;
FallbackDc g_dcs[kDcs];
uint32_t g_dc_serial = 0;
struct FallbackRegion { uint32_t handle = 0; bool used = false; uint32_t count = 0; Rect rects[kRegionRects]; };
FallbackRegion g_regions[kRegions];
uint32_t g_region_serial = 0;
uint32_t g_width = 1024, g_height = 768;
}  // namespace

TCOS_WEAK uint32_t screen_width() { return g_width; }
TCOS_WEAK uint32_t screen_height() { return g_height; }

TCOS_WEAK uint32_t create_screen_dc(uint64_t pid, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip) {
  (void)pid; (void)origin_x; (void)origin_y; (void)clip;
  for (FallbackDc& d : g_dcs) {
    if (d.used) continue;
    d.used = true;
    d.handle = 0x01010000u + 0x10 * (++g_dc_serial);
    return d.handle;
  }
  return 0;
}
TCOS_WEAK bool update_screen_dc(uint32_t dc, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip) {
  (void)origin_x; (void)origin_y; (void)clip;
  return is_dc(dc);
}
TCOS_WEAK bool is_dc(uint32_t handle) {
  for (const FallbackDc& d : g_dcs) if (d.used && d.handle == handle) return true;
  return false;
}
TCOS_WEAK bool delete_dc(uint32_t dc) {
  for (FallbackDc& d : g_dcs) if (d.used && d.handle == dc) { d.used = false; return true; }
  return false;
}

TCOS_WEAK uint32_t create_region(uint64_t pid, ArrayView<const Rect> rects) {
  (void)pid;
  for (FallbackRegion& r : g_regions) {
    if (r.used) continue;
    r.used = true;
    r.handle = 0x04040000u + 0x10 * (++g_region_serial);
    r.count = 0;
    for (const Rect& x : rects) if (r.count < kRegionRects) r.rects[r.count++] = x;
    return r.handle;
  }
  return 0;
}
TCOS_WEAK bool region_rects(uint32_t region, List<Rect>* out) {
  for (const FallbackRegion& r : g_regions) {
    if (!r.used || r.handle != region) continue;
    for (uint32_t i = 0; i < r.count; ++i) out->push_back(r.rects[i]);
    return true;
  }
  return false;
}
TCOS_WEAK bool replace_region(uint32_t region, ArrayView<const Rect> rects) {
  for (FallbackRegion& r : g_regions) {
    if (!r.used || r.handle != region) continue;
    r.count = 0;
    for (const Rect& x : rects) if (r.count < kRegionRects) r.rects[r.count++] = x;
    return true;
  }
  return false;
}
TCOS_WEAK bool is_region(uint32_t handle) {
  for (const FallbackRegion& r : g_regions) if (r.used && r.handle == handle) return true;
  return false;
}

}  // namespace gdi

namespace user_misc {

TCOS_WEAK int32_t system_metrics(uint32_t index, uint32_t dpi) {
  (void)dpi;
  switch (index) {
    case 0: return 1024;      // SM_CXSCREEN
    case 1: return 768;       // SM_CYSCREEN
    case 2: case 3: return 17;      // SM_CXVSCROLL, SM_CYHSCROLL
    case 4: return 23;        // SM_CYCAPTION
    case 5: case 6: return 1;       // SM_CXBORDER, SM_CYBORDER
    case 7: case 8: return 3;       // SM_CXDLGFRAME, SM_CYDLGFRAME
    case 15: return 20;       // SM_CYMENU
    case 32: case 33: return 4;     // SM_CXFRAME, SM_CYFRAME
    case 45: case 46: return 2;     // SM_CXEDGE, SM_CYEDGE
    case 51: return 23;       // SM_CYSMCAPTION
    case 92: return 4;        // SM_CXPADDEDBORDER
    default: return 0;
  }
}
TCOS_WEAK uint32_t system_color(uint32_t index) { (void)index; return 0x00FFFFFFu; }
TCOS_WEAK uint32_t monitor_from_rect(const gdi::Rect& rect, uint32_t flags) { (void)rect; (void)flags; return 0x10001; }
TCOS_WEAK uint32_t monitor_from_point(int32_t x, int32_t y, uint32_t flags) { (void)x; (void)y; (void)flags; return 0x10001; }
TCOS_WEAK uint32_t dpi_for_monitor(uint32_t monitor) { (void)monitor; return 96; }
TCOS_WEAK gdi::Rect monitor_rect(uint32_t monitor, bool work_area) {
  (void)monitor;
  return gdi::Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height()) - (work_area ? 40 : 0)};
}
TCOS_WEAK bool is_menu(uint32_t handle) { (void)handle; return false; }
TCOS_WEAK void window_destroyed(uint32_t hwnd) { (void)hwnd; }
TCOS_WEAK bool attach_window_menu(uint32_t hwnd, uint32_t menu) { (void)hwnd; (void)menu; return false; }
TCOS_WEAK void raw_input_event(uint32_t type, bool injected, ConstByteView data) { (void)type; (void)injected; (void)data; }
TCOS_WEAK bool raw_input_no_legacy(uint64_t pid, uint32_t type) { (void)pid; (void)type; return false; }

}  // namespace user_misc

}  // namespace process::win32k
