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
State& S() { return state(); }
uint32_t g_device_scale = 100;      // (percent)
}  // namespace

uint32_t device_scale() { return g_device_scale; }

void set_device_scale(uint32_t scale) {
  g_device_scale = graphics::clamp_scale_percent(scale);
  user_misc::set_system_dpi(96 * g_device_scale / 100);
}

// A system-aware context carries the system DPI of when it was set (normalize_context): the program keeps working at that DPI, as on Windows. When the
// device scale has grown since, its coordinates are virtualized by the ratio, as an unaware program's are by the scale. (When the scale has shrunk, it
// works in device pixels: virtualization only enlarges.)
uint32_t system_aware_scale(uint32_t context) {
  if ((context & 0x0F) != 1) return 100;
  const uint32_t dpi = (context >> 8) & 0x1FF;
  if (dpi == 0) return 100;
  const uint32_t k = 96 * g_device_scale / dpi;
  return k > 100 ? graphics::clamp_scale_percent(k) : 100;
}

uint32_t window_scale(const Window* w) {
  if (!w) return 100;
  return w->dpi_virtual ? g_device_scale : system_aware_scale(w->dpi_context);
}

bool thread_unaware(Call& call) { return user_misc::dpi_awareness(call.tid(), call.pid()) == 0; }

uint32_t caller_scale(Call& call) {
  if (thread_unaware(call)) return g_device_scale;
  return system_aware_scale(user_misc::dpi_context_of(call.tid(), call.pid()));
}

uint32_t window_working_dpi(const Window* w) {
  if (!w || w->dpi_virtual) return 96;
  if ((w->dpi_context & 0x0F) == 1 && ((w->dpi_context >> 8) & 0x1FF) != 0) return (w->dpi_context >> 8) & 0x1FF;
  return 96 * g_device_scale / 100;
}

uint32_t dpi_kind(const Window* w) {
  if (w->dpi_virtual) return 1;
  return (w->dpi_context & 0x0F) == 1 ? 0x10000u | ((w->dpi_context >> 8) & 0x1FF) : 0;
}
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
  // each top-level keeps its place in desktop units: its device rectangle times after / before. A per-monitor aware window is asked, as on Windows
  // (WM_DPICHANGED with that rectangle suggested, DefWindowProc applying it when the program does not); any other is moved through the program, as a
  // move of the desktop's would be (an unaware one's units stay what they were; a system-aware one keeps its device size, as Windows leaves it).
  for (uint32_t hwnd : tops.view()) {
    gdi::Rect r;
    bool per_monitor = false;
    {
      LockGuard g;
      Window* w = find_window(hwnd);
      if (!w) continue;
      r = window_rect_screen(w);
      per_monitor = !w->dpi_virtual && (w->dpi_context & 0x0F) == 2;
    }
    auto conv = [&](int32_t v) { return static_cast<int32_t>(int64_t{v} * after / before); };
    const gdi::Rect to{conv(r.left), conv(r.top), conv(r.right), conv(r.bottom)};
    if (!per_monitor) {
      move_top_level(hwnd, to);
      continue;
    }
    const int32_t cx = to.right - to.left, cy = to.bottom - to.top;
    if (cx < 0 || cy < 0 || cx > 0xFFFF || cy > 0xFFFF || to.left < -32768 || to.left > 32767 || to.top < -32768 || to.top > 32767) continue;
    const uint64_t packed = uint64_t(uint16_t(int16_t(to.left))) | uint64_t(uint16_t(int16_t(to.top))) << 16 | uint64_t(uint16_t(cx)) << 32 | uint64_t(uint16_t(cy)) << 48;
    LockGuard g;
    post_message(hwnd, 0x112 /* WM_SYSCOMMAND */, kScTcosDpi, packed);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Mixed DPI hosting. With SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED) a thread may put a child of one awareness into a window of another; as
// on Windows, the system scales it on its own. Such a child (a "mixed child") gets a surface of its own in its coordinates, which its tree's DCs draw
// on; whenever it or the top-level's surface under it changes, win32k scales its pixels into the top-level's surface (nearest sampling: up for an
// unaware child of an aware window, down for the other way). With default hosting a child takes the awareness of the tree it is in.

Window* dpi_drawing_root(Window* w) {
  for (Window* x = w; x && x->parent; x = x->parent) {
    if (x->dpi_mixed) return x;
    if (!x->parent->parent) return x;      // (the top-level)
  }
  return w;
}

bool adopt_tree_awareness(Window* w, Window* parent, bool mixed_allowed) {
  Window* root = dpi_drawing_root(parent);
  if (!root) return true;
  if (dpi_kind(w) == dpi_kind(root)) {
    if (w->dpi_mixed) drop_mixed_surface(w);
    w->dpi_mixed = false;
    return true;
  }
  if (!mixed_allowed) {
    if (w->parent) return false;      // (an existing window, SetParent: refused)
    w->dpi_virtual = root->dpi_virtual;      // (a new child, not linked yet: it is of the tree's awareness)
    w->dpi_context = root->dpi_context;
    w->dpi_mixed = false;
    return true;
  }
  w->dpi_mixed = true;
  return true;
}

namespace {

// A mixed child's surface rectangle: its window rectangle, in its own units.
Rect mixed_rect_units(const Window* m) {
  const Rect r = window_rect_screen(m);
  const uint32_t k = window_scale(m);
  return {dpi_down(r.left, k), dpi_down(r.top, k), dpi_down(r.left, k) + graphics::device_to_units_up(r.right - r.left, k),
          dpi_down(r.top, k) + graphics::device_to_units_up(r.bottom - r.top, k)};
}

template <typename F>
void each_window_in(Window* w, F&& f) {
  f(w);
  for (Window* c = w->first_child; c; c = c->next) each_window_in(c, f);
}

// The part `area` (top-level surface coordinates) of mixed child m's pixels, scaled into the top-level's surface, inside what of m shows there.
void composite_mixed(Window* m, Window* top, const Rect& area) {
  if (!m->child_surface || !top->surface || !is_visible(m)) return;
  uint32_t mw = 0, mh = 0, tw = 0, th = 0;
  const ArrayView<uint32_t> src = gdi::window_surface_pixels(m->child_surface, &mw, &mh);
  const ArrayView<uint32_t> dst = gdi::window_surface_pixels(top->surface, &tw, &th);
  if (src.empty() || dst.empty()) return;
  const uint32_t kt = window_scale(top), km = window_scale(m);
  const Rect torigin = surface_rect_in_window_units(top);      // (the top-level's surface (0, 0), in its units on the screen)
  const Rect mr = window_rect_screen(m);
  List<Rect> shows(S().alloc);      // what of m shows, in the top-level's surface coordinates
  if (!visible_region(m, false, false, true, &shows)) return;
  for (const Rect& v0 : shows.view()) {
    Rect v = rect_offset(dpi_cover(v0, kt), -torigin.left, -torigin.top);
    v = rect_intersect(v, area);
    v = rect_intersect(v, Rect{0, 0, int32_t(tw), int32_t(th)});
    if (rect_empty(v)) continue;
    for (int32_t y = v.top; y < v.bottom; ++y) {
      const int32_t py = dpi_up(y + torigin.top, kt);      // (the device row of the top-level's pixel)
      const int32_t sy = dpi_down(py - mr.top, km);
      if (sy < 0 || sy >= int32_t(mh)) continue;
      for (int32_t x = v.left; x < v.right; ++x) {
        const int32_t px = dpi_up(x + torigin.left, kt);
        const int32_t sx = dpi_down(px - mr.left, km);
        if (sx < 0 || sx >= int32_t(mw)) continue;
        dst[uint64_t(y) * tw + uint64_t(x)] = src[uint64_t(sy) * mw + uint64_t(sx)];
      }
    }
  }
}

}  // namespace

void sync_mixed_surface(Window* m) {
  if (!m || !m->dpi_mixed) return;
  Window* top = top_level_of(m);
  if (!top || !redirected(top)) return;
  const Rect r = mixed_rect_units(m);
  const uint32_t w = uint32_t(r.right - r.left), h = uint32_t(r.bottom - r.top);
  if (!is_visible(m) || w == 0 || h == 0) return;
  if (m->child_surface == 0) {
    m->child_surface = gdi::create_window_surface(m->handle, w, h);
  } else {
    uint32_t cw = 0, ch = 0;
    if (gdi::window_surface_size(m->child_surface, &cw, &ch) && cw == w && ch == h) return;
    gdi::resize_window_surface(m->child_surface, w, h);
  }
  each_window_in(m, [](Window* x) { refresh_window_dcs(x); });
  each_window_in(m, [](Window* x) { if (is_visible(x)) { invalidate_window(x, nullptr, true); x->nc_paint_pending = true; } });
}

void drop_mixed_surface(Window* m) {
  if (!m || m->child_surface == 0) return;
  gdi::destroy_window_surface(m->child_surface);
  m->child_surface = 0;
  each_window_in(m, [](Window* x) { refresh_window_dcs(x); });
}

void mixed_tree_changed(Window* top) {
  if (!top) return;
  each_window_in(top, [](Window* x) {
    if (x->dpi_mixed && is_visible(x)) {
      sync_mixed_surface(x);
      invalidate_window(x, nullptr, true);      // (it paints again, which puts it into the top-level's surface where it now is)
    }
  });
}

int32_t window_dpi_hosting(uint32_t hwnd) {
  const Window* w = state_or_null() ? find_window(hwnd) : nullptr;
  return w ? w->dpi_hosting : -1;
}

bool mixed_damage_locked(uint32_t window, const gdi::Rect& dirty, uint32_t* top_out, gdi::Rect* top_dirty) {
  if (!state_or_null()) return false;
  Window* w = find_window(window);
  if (!w) return false;
  if (w->dpi_mixed && w->child_surface) {      // the mixed child drew: its pixels into the top-level's surface
    Window* top = top_level_of(w);
    if (!top || !top->surface) return false;
    const uint32_t kt = window_scale(top), km = window_scale(w);
    const Rect torigin = surface_rect_in_window_units(top);
    const Rect mr = window_rect_screen(w);
    const Rect device{dpi_up(dirty.left, km) + mr.left, dpi_up(dirty.top, km) + mr.top, dpi_up(dirty.right, km) + mr.left, dpi_up(dirty.bottom, km) + mr.top};
    const Rect area = rect_offset(dpi_cover(device, kt), -torigin.left, -torigin.top);
    composite_mixed(w, top, area);
    *top_out = top->handle;
    *top_dirty = area;
    return true;
  }
  if (w->parent && !w->parent->parent && w->surface) {      // the top-level drew: its mixed children go back over what it drew
    each_window_in(w, [&](Window* x) { if (x != w && x->dpi_mixed) composite_mixed(x, w, dirty); });
  }
  return false;
}

}  // namespace process::win32k::user
