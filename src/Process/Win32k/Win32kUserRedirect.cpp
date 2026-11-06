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

void drag_requested(const Window* top, uint32_t edge) {
  if (g_redirect.on && g_redirect.observer && on_desktop(top)) g_redirect.observer->drag_requested(top->handle, edge);
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
  const uint64_t icon = w->icon_small ? w->icon_small : w->icon_big ? w->icon_big : w->cls ? (w->cls->icon_small ? w->cls->icon_small : w->cls->icon) : 0;
  out->icon = uint32_t(icon);
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

void pointer_cursor(PointerCursor* out) {
  LockGuard g;
  *out = PointerCursor();
  if (!state_or_null()) return;
  if (user_misc::cursor_show_count() < 0) { out->hidden = true; return; }
  uint32_t hit = 0;
  const Window* w = window_under_cursor(&hit);
  if (!w) return;
  switch (hit) {      // the borders of a window that draws them itself: the sizing arrows (OCR_SIZEWE, OCR_SIZENS, OCR_SIZENWSE, OCR_SIZENESW)
    case 10: case 11: out->system_id = 32644; return;
    case 12: case 15: out->system_id = 32645; return;
    case 13: case 17: out->system_id = 32642; return;
    case 14: case 16: out->system_id = 32643; return;
    default: break;
  }
  uint32_t cursor = 0;
  if (!user_misc::thread_cursor(w->thread, &cursor)) cursor = w->cls ? uint32_t(w->cls->cursor) : 0;      // (the class cursor, as WM_SETCURSOR's default sets it)
  else if (cursor == 0) { out->hidden = true; return; }      // SetCursor(NULL)
  out->cursor = cursor;
}

bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) {
  LockGuard g;
  return user_misc::cursor_picture(cursor, system_id, argb, width, height, hot_x, hot_y);
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
  // the client area in surface coordinates (clipped to the surface); the picture is stretched over it (nearest pixel), or copied a row at a time when the sizes agree
  const Rect origin = surface_rect_screen(top);
  const Rect client = client_rect_screen(w);
  const int64_t cw = int64_t{client.right} - client.left, ch = int64_t{client.bottom} - client.top;
  const int64_t cx = int64_t{client.left} - origin.left, cy = int64_t{client.top} - origin.top;
  if (cw <= 0 || ch <= 0 || width == 0 || height == 0 || pitch < uint64_t{width} * 4) return true;
  const int64_t x0 = cx < 0 ? -cx : 0, y0 = cy < 0 ? -cy : 0;
  const int64_t x1 = cx + cw > int64_t{sw} ? int64_t{sw} - cx : cw, y1 = cy + ch > int64_t{sh} ? int64_t{sh} - cy : ch;
  if (x1 <= x0 || y1 <= y0) return true;
  const bool same_size = cw == int64_t{width} && ch == int64_t{height};
  const uint64_t n = uint64_t(x1 - x0);
  for (int64_t y = y0; y < y1; ++y) {
    ByteView dst;
    if (!surface.try_subview((uint64_t(cy + y) * sw + uint64_t(cx + x0)) * 4, n * 4, &dst)) break;
    const uint64_t sy = same_size ? uint64_t(y) : uint64_t(y) * height / uint64_t(ch);
    ConstByteView row;
    if (!pixels.try_subview(sy * pitch, uint64_t{width} * 4, &row)) break;
    if (same_size && !rgba) {      // (B, G, R, A: the surface's own order, one copy per row)
      dst.copy_from(row.subview(uint64_t(x0) * 4, n * 4));
      continue;
    }
    for (uint64_t i = 0; i < n; ++i) {
      const uint64_t sx = same_size ? uint64_t(x0) + i : (uint64_t(x0) + i) * width / uint64_t(cw);
      const ConstByteView p = row.subview(sx * 4, 4);
      dst[i * 4] = rgba ? p[2] : p[0];
      dst[i * 4 + 1] = p[1];
      dst[i * 4 + 2] = rgba ? p[0] : p[2];
      dst[i * 4 + 3] = p[3];
    }
  }
  gdi::window_surface_damaged(top->surface, gdi::Rect{int32_t(cx + x0), int32_t(cy + y0), int32_t(cx + x1), int32_t(cy + y1)});
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
