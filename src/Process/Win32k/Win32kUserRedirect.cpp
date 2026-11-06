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

#include "Process/Win32k/Win32kUserInternal.hpp"

// Redirection: the kernel's desktop shows every visible top-level window as one of its own windows. A redirected top-level draws (with its whole tree) on a window
// surface of its own, which the desktop copies into its picture as it is; screen coordinates are the desktop's. The desktop draws the caption and frame of a window that
// has a caption (RedirectConfig::frame_border / frame_title), so its surface covers the inside of that frame. Top-levels neither clip nor repaint one another: the desktop
// composites them. The observer (the desktop's bridge) is told of every change of a top-level; what the user does to the desktop's window comes back through
// move_top_level / activate_top_level / command_top_level.
namespace process::win32k::user {

namespace {

State& S() { return state(); }

struct LockGuard {
  ObjectManager* objects;
  LockGuard() : objects(system_objects()) { if (objects) objects->sync().lock(); }
  ~LockGuard() { if (objects) objects->sync().unlock(); }
};

// A top-level window of the desktop (not a message-only window, whose parent is another root).
bool on_desktop(const Window* w) {
  const Desktop* d = default_desktop();
  return w && d && w->parent && w->parent == d->root;
}

bool has_frame(const Window* w) { return (w->style & wn::WS_CAPTION) == wn::WS_CAPTION; }

// Every window of a tree, `w` first.
template <typename F>
void each_in_tree(Window* w, F&& f) {
  f(w);
  for (Window* c = w->first_child; c; c = c->next) each_in_tree(c, f);
}

RedirectConfig g_redirect;

}  // namespace

RedirectConfig& redirect_config() { return g_redirect; }

bool redirected(const Window* w) { return g_redirect.on && state_or_null() && on_desktop(w); }

Rect surface_rect_screen(const Window* top) {
  Rect r = window_rect_screen(top);
  if (has_frame(top) && !(top->style & wn::WS_MINIMIZE)) {
    const RedirectConfig& s = g_redirect;
    r.left += s.frame_border;
    r.right -= s.frame_border;
    r.top += s.frame_border + s.frame_title;
    r.bottom -= s.frame_border;
  }
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}

void sync_surface(Window* top) {
  if (!redirected(top)) return;
  const Rect r = surface_rect_screen(top);
  const uint32_t w = uint32_t(r.right - r.left), h = uint32_t(r.bottom - r.top);
  if (!is_visible(top) || w == 0 || h == 0) return;      // (the surface stays, at its last size, while hidden or minimized)
  if (top->surface == 0) {
    top->surface = gdi::create_window_surface(top->handle, w, h);
  } else {
    uint32_t cw = 0, ch = 0;
    if (gdi::window_surface_size(top->surface, &cw, &ch) && cw == w && ch == h) return;
    gdi::resize_window_surface(top->surface, w, h);
  }
  each_in_tree(top, [](Window* x) { refresh_window_dcs(x); });      // (the DCs of the tree now point at the surface, origins and clips in its coordinates)
  each_in_tree(top, [](Window* x) { if (is_visible(x)) { invalidate_window(x, nullptr, true); x->nc_paint_pending = true; } });      // a new surface is blank: all of it is painted
}

void drop_surface(Window* top) {
  if (top->surface == 0) return;
  gdi::destroy_window_surface(top->surface);
  top->surface = 0;
}

void notify_top_level(const Window* w) {
  if (!g_redirect.on || !g_redirect.observer || !state_or_null() || !w) return;
  const Window* top = w;
  while (top && top->parent && !on_desktop(top)) top = top->parent;
  if (on_desktop(top)) g_redirect.observer->top_level_changed(top->handle);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The desktop's side

void set_redirection(bool on, IWindowObserver* observer, int32_t frame_border, int32_t frame_title) {
  LockGuard g;
  g_redirect.on = on;
  g_redirect.observer = on ? observer : nullptr;
  if (on) {
    g_redirect.frame_border = frame_border;
    g_redirect.frame_title = frame_title;
  }
}

bool top_level_info(uint32_t hwnd, TopLevelInfo* out, WideString* title) {
  LockGuard g;
  *out = TopLevelInfo();
  if (!state_or_null()) return false;
  Window* w = find_window(hwnd);
  if (!on_desktop(w) || w->destroying) return false;
  out->exists = true;
  out->visible = is_visible(w);
  out->minimized = (w->style & wn::WS_MINIMIZE) != 0;
  out->maximized = (w->style & wn::WS_MAXIMIZE) != 0;
  out->caption = has_frame(w);
  out->sizable = (w->style & wn::WS_THICKFRAME) != 0;
  out->minimize_box = (w->style & 0x00020000u) != 0 && (w->style & wn::WS_SYSMENU);      // WS_MINIMIZEBOX (shares its bit with WS_GROUP on a top-level)
  out->maximize_box = (w->style & 0x00010000u) != 0 && (w->style & wn::WS_SYSMENU);      // WS_MAXIMIZEBOX (shares its bit with WS_TABSTOP)
  out->tool = (w->ex_style & wn::WS_EX_TOOLWINDOW) != 0;
  out->topmost = (w->ex_style & wn::WS_EX_TOPMOST) != 0;
  out->popup = (w->style & wn::WS_POPUP) != 0;
  out->no_activate = (w->ex_style & wn::WS_EX_NOACTIVATE) != 0;
  out->active = foreground_window() == hwnd;
  out->window = window_rect_screen(w);
  out->surface = surface_rect_screen(w);
  out->surface_id = w->surface;
  out->owner = w->owner ? w->owner->handle : 0;
  out->pid = w->pid;
  if (title) {
    title->clear();
    title->append(w->text.view());
  }
  return true;
}

void top_levels(List<uint32_t>* out) {
  LockGuard g;
  out->clear();
  if (!g_redirect.on || !state_or_null()) return;
  const Desktop* d = default_desktop();
  if (!d || !d->root) return;
  for (Window* c = d->root->first_child; c; c = c->next) if (is_visible(c) && !c->destroying) out->push_back(c->handle);
}

bool move_top_level(uint32_t hwnd, const gdi::Rect& r) {
  LockGuard g;
  if (!state_or_null()) return false;
  Window* w = find_window(hwnd);
  if (!on_desktop(w)) return false;
  if (rect_equal(window_rect_screen(w), r)) return true;
  const int32_t cx = r.right - r.left, cy = r.bottom - r.top;
  if (cx < 0 || cy < 0 || cx > 0xFFFF || cy > 0xFFFF || r.left < -32768 || r.left > 32767 || r.top < -32768 || r.top > 32767) return false;
  const uint64_t packed = uint64_t(uint16_t(int16_t(r.left))) | uint64_t(uint16_t(int16_t(r.top))) << 16 | uint64_t(uint16_t(cx)) << 32 | uint64_t(uint16_t(cy)) << 48;
  return post_message(hwnd, 0x112 /* WM_SYSCOMMAND */, kScTcosPlace, packed);
}

bool activate_top_level(uint32_t hwnd) {
  LockGuard g;
  if (!state_or_null()) return false;
  if (hwnd == 0) {
    clear_foreground_internal();
    return true;
  }
  Window* w = find_window(hwnd);
  if (!on_desktop(w)) return false;
  if (foreground_window() == hwnd) return true;
  return set_foreground_internal(w);
}

uint32_t top_level_surface_locked(uint32_t hwnd) {
  if (!state_or_null()) return 0;
  const Window* w = find_window(hwnd);
  return on_desktop(w) ? w->surface : 0;
}

bool window_redirected_locked(uint32_t hwnd) {
  if (!state_or_null()) return false;
  const Window* w = find_window(hwnd);
  while (w && w->parent && !on_desktop(w)) w = w->parent;
  return redirected(w);
}

bool present_to_window_locked(uint32_t hwnd, ConstByteView pixels, uint32_t width, uint32_t height, uint32_t pitch, bool rgba) {
  if (!state_or_null()) return false;
  Window* w = find_window(hwnd);
  if (!w) return false;
  Window* top = w;
  while (top->parent && !on_desktop(top)) top = top->parent;
  if (!redirected(top)) return false;
  if (!is_visible(w) || (top->style & wn::WS_MINIMIZE)) return true;      // (nothing shows: the frame is dropped, as a hidden window's would be)
  sync_surface(top);
  uint32_t sw = 0, sh = 0;
  const ByteView surface = gdi::window_surface_bytes(top->surface, &sw, &sh);
  if (surface.empty()) return true;
  // the client area in surface coordinates, and the part of the picture that fits it
  const Rect origin = surface_rect_screen(top);
  const Rect client = client_rect_screen(w);
  const int32_t cx = client.left - origin.left, cy = client.top - origin.top;
  const int64_t x0 = cx < 0 ? -int64_t{cx} : 0, y0 = cy < 0 ? -int64_t{cy} : 0;
  int64_t cols = int64_t{width} < client.right - client.left ? int64_t{width} : client.right - client.left;
  int64_t rows = int64_t{height} < client.bottom - client.top ? int64_t{height} : client.bottom - client.top;
  if (int64_t{cx} + cols > int64_t{sw}) cols = int64_t{sw} - cx;
  if (int64_t{cy} + rows > int64_t{sh}) rows = int64_t{sh} - cy;
  if (cols <= x0 || rows <= y0 || pitch < uint64_t{width} * 4) return true;
  const uint64_t n = uint64_t(cols - x0);
  for (int64_t y = y0; y < rows; ++y) {
    ConstByteView src;
    ByteView dst;
    if (!pixels.try_subview(uint64_t(y) * pitch + uint64_t(x0) * 4, n * 4, &src) || !surface.try_subview((uint64_t(cy + y) * sw + uint64_t(cx + x0)) * 4, n * 4, &dst)) break;
    if (!rgba) {      // (B, G, R, A: the surface's own order, one copy per row)
      dst.copy_from(src);
      continue;
    }
    for (uint64_t i = 0; i < n * 4; i += 4) {
      dst[i] = src[i + 2];
      dst[i + 1] = src[i + 1];
      dst[i + 2] = src[i];
      dst[i + 3] = src[i + 3];
    }
  }
  gdi::window_surface_damaged(top->surface, gdi::Rect{int32_t(cx + x0), int32_t(cy + y0), int32_t(cx + cols), int32_t(cy + rows)});
  return true;
}

bool command_top_level(uint32_t hwnd, uint32_t sc) {
  LockGuard g;
  if (!state_or_null()) return false;
  Window* w = find_window(hwnd);
  if (!on_desktop(w)) return false;
  return post_message(hwnd, 0x112 /* WM_SYSCOMMAND */, sc, 0);
}

}  // namespace process::win32k::user
