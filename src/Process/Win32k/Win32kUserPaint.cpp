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
// Painting: the visible region of a window, device contexts of windows (through the GDI module), update regions, BeginPaint/EndPaint, RedrawWindow, scroll bar state.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }
constexpr uint32_t SM_CXVSCROLL = 2, SM_CYHSCROLL = 3;

void put32(uint8_t* raw, uint32_t o, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) raw[o + i] = uint8_t(v >> (8 * i)); }
uint32_t get32(const uint8_t* raw, uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The visible region

bool visible_region(const Window* w, bool client_area, bool clip_children, bool clip_siblings, List<Rect>* out) {
  out->clear();
  if (!is_visible(w)) return true;
  for (const Window* a = w->parent; a; a = a->parent) if (a->parent && (a->style & wn::WS_MINIMIZE)) return true;
  const Rect base = client_area ? client_rect_screen(w) : window_rect_screen(w);
  if (rect_empty(base)) return true;
  if (!out->push_back(base)) return false;
  for (const Window* cur = w; cur->parent; cur = cur->parent) {
    const Window* par = cur->parent;
    if (par->parent) region_intersect(*out, client_rect_screen(par));
    const bool top_level = par->parent == nullptr;
    if (top_level && redirected(cur)) {      // a redirected top-level draws on its own surface: other top-levels do not cover it (the desktop composites), its frame is the desktop's
      region_intersect(*out, surface_rect_screen(cur));
      break;
    }
    const bool clip = top_level || (cur == w ? clip_siblings : (cur->style & wn::WS_CLIPSIBLINGS) != 0);
    if (clip) {
      for (const Window* sib = par->first_child; sib && sib != cur; sib = sib->next) {
        if (!(sib->style & wn::WS_VISIBLE) || (sib->ex_style & wn::WS_EX_TRANSPARENT)) continue;
        region_subtract(*out, window_rect_screen(sib));
      }
    }
    if (out->count() == 0) return true;
  }
  if (clip_children) {
    for (const Window* c = w->first_child; c; c = c->next) if (c->style & wn::WS_VISIBLE) region_subtract(*out, window_rect_screen(c));
  }
  if (w->region != 0 && gdi::is_region(w->region)) {
    List<Rect> shape(S().alloc);
    if (gdi::region_rects(w->region, &shape)) {
      const Rect origin = window_rect_screen(w);
      List<Rect> clipped(S().alloc);
      const uint32_t k = window_scale(w);      // (a window region is in the window's coordinates)
      for (const Rect& r : shape) {
        const Rect placed = rect_offset(dpi_up(r, k), origin.left, origin.top);
        for (const Rect& v : *out) {
          const Rect i = rect_intersect(placed, v);
          if (!rect_empty(i)) clipped.push_back(i);
        }
      }
      region_copy(*out, clipped);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Device contexts of windows

namespace {

// The origin and clip of a window's DC: on the screen, or - for a window of a redirected top-level - on that top-level's surface (`surface` is then its id).
bool make_dc_geometry(Window* w, const WinDc& d, int32_t* ox, int32_t* oy, List<Rect>* clip, uint32_t* surface) {
  *surface = 0;
  if (d.window_dc) { const Rect r = window_rect_screen(w); *ox = r.left; *oy = r.top; }
  else client_origin(w, ox, oy);
  if (!visible_region(w, !d.window_dc, d.clip_children, d.clip_siblings, clip)) return false;
  Window* top = top_level_of(w);
  Window* mixed = top && redirected(top) ? dpi_drawing_root(w) : nullptr;
  if (mixed && mixed != top && mixed->dpi_mixed) {      // (a mixed DPI child's tree draws on that child's own surface, in its units)
    if (mixed->child_surface == 0) sync_mixed_surface(mixed);
    if (mixed->child_surface == 0) { clip->clear(); return true; }
    const uint32_t k = window_scale(mixed);
    const Rect mr = window_rect_screen(mixed);
    *ox = dpi_down(*ox - mr.left, k);
    *oy = dpi_down(*oy - mr.top, k);
    List<Rect> inside(S().alloc);
    for (const Rect& r : *clip) {
      const Rect i = rect_intersect(r, mr);
      if (!rect_empty(i)) inside.push_back(dpi_cover(rect_offset(i, -mr.left, -mr.top), k));
    }
    region_copy(*clip, inside);
    *surface = mixed->child_surface;
    return true;
  }
  if (top && redirected(top)) {
    if (top->surface == 0) sync_surface(top);
    if (top->surface == 0) { clip->clear(); return true; }      // (no surface yet - hidden or no memory: the DC draws nowhere)
    const uint32_t k = window_scale(top);      // (an unaware window's surface and DCs are in its 96-dpi units)
    const Rect s = surface_rect_in_window_units(top);
    *ox = dpi_down(*ox, k) - s.left;
    *oy = dpi_down(*oy, k) - s.top;
    for (Rect& r : *clip) {
      r = dpi_cover(r, k);      // (covering what it covered: the far edges rounded up)
      r = rect_offset(r, -s.left, -s.top);
    }
    *surface = top->surface;
  }
  return true;
}

uint32_t create_window_dc(Call& call, Window* w, bool window_dc, bool clip_children, bool clip_siblings, const List<Rect>* extra_clip, bool exclude) {
  State& s = S();
  WinDc d;
  d.hwnd = w->handle;
  d.pid = call.pid();
  d.window_dc = window_dc;
  d.clip_children = clip_children;
  d.clip_siblings = clip_siblings;
  int32_t ox = 0, oy = 0;
  uint32_t surface = 0;
  List<Rect> clip(s.alloc);
  if (!make_dc_geometry(w, d, &ox, &oy, &clip, &surface)) return 0;
  if (extra_clip) {
    if (exclude) {
      for (const Rect& r : *extra_clip) region_subtract(clip, rect_offset(r, ox, oy));
    } else {
      List<Rect> inter(s.alloc);
      for (const Rect& r : *extra_clip) {
        const Rect placed = rect_offset(r, ox, oy);
        for (const Rect& v : clip) {
          const Rect i = rect_intersect(placed, v);
          if (!rect_empty(i)) inter.push_back(i);
        }
      }
      region_copy(clip, inter);
    }
  }
  if (s.lock_update_window != 0 && s.lock_update_window == w->handle) clip.clear();
  const uint32_t dc = gdi::create_screen_dc(call.pid(), ox, oy, clip.view(), surface);
  if (dc == 0) return 0;
  d.dc = dc;
  if (!s.dcs.emplace(d)) { gdi::delete_dc(dc); return 0; }
  return dc;
}

}  // namespace

void release_window_dcs(uint32_t hwnd) {
  State& s = S();
  for (;;) {
    WinDc* d = nullptr;
    for (WinDc& x : s.dcs) if (x.hwnd == hwnd) { d = &x; break; }
    if (!d) return;
    gdi::delete_dc(d->dc);
    s.dcs.erase(d);
  }
}

void refresh_window_dcs(Window* w) {
  State& s = S();
  for (WinDc& d : s.dcs) {
    if (d.hwnd != w->handle) continue;
    int32_t ox = 0, oy = 0;
    uint32_t surface = 0;
    List<Rect> clip(s.alloc);
    if (make_dc_geometry(w, d, &ox, &oy, &clip, &surface)) gdi::update_screen_dc(d.dc, ox, oy, clip.view(), surface);
  }
}

bool dc_screen_offset_locked(Call& call, uint32_t dc, int32_t* dx, int32_t* dy) {
  if (!state_or_null()) return false;
  for (WinDc& d : S().dcs) {
    if (d.dc != dc) continue;
    Window* w = find_window(d.hwnd);
    Window* top = w ? top_level_of(w) : nullptr;
    if (!top || !redirected(top)) return false;      // (drawing on the screen itself: its coordinates are the screen's)
    const uint32_t kc = caller_scale(call);
    Window* mixed = dpi_drawing_root(w);
    if (mixed && mixed != top && mixed->dpi_mixed) {      // a mixed DPI child's own surface, in its units
      const Rect mr = window_rect_screen(mixed);
      *dx = dpi_down(mr.left, kc);
      *dy = dpi_down(mr.top, kc);
      return true;
    }
    const uint32_t kt = window_scale(top);
    const Rect s = surface_rect_in_window_units(top);      // (screen coordinates in the top-level's units)
    *dx = dpi_down(dpi_up(s.left, kt), kc);
    *dy = dpi_down(dpi_up(s.top, kt), kc);
    return true;
  }
  return false;
}

namespace {

// GetDC(NULL): the whole screen - for a DPI unaware caller on a scaled screen, a shadow of it in the caller's units (what it draws goes on to the screen, scaled up).
uint32_t screen_dc(Call& call) {
  const uint32_t k = caller_scale(call);
  if (k > 100) return gdi::create_screen_shadow_dc(call.pid(), k);
  const Rect all{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())};
  return gdi::create_screen_dc(call.pid(), 0, 0, ArrayView<const Rect>(&all, 1));
}

// HDC NtUserGetDC(HWND)
uint64_t nt_get_dc(Call& call) {
  if (call.arg(0) == 0) return screen_dc(call);
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  return create_window_dc(call, w, false, (w->style & wn::WS_CLIPCHILDREN) != 0, (w->style & wn::WS_CLIPSIBLINGS) != 0, nullptr, false);
}
uint64_t nt_get_window_dc(Call& call) {
  if (call.arg(0) == 0) return screen_dc(call);
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  return create_window_dc(call, w, true, (w->style & wn::WS_CLIPCHILDREN) != 0, (w->style & wn::WS_CLIPSIBLINGS) != 0, nullptr, false);
}
// HDC NtUserGetDCEx(HWND, HRGN clip, DWORD flags)
uint64_t nt_get_dc_ex(Call& call) {
  const uint32_t flags = call.arg32(2);
  if (call.arg(0) == 0) return screen_dc(call);
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  bool children = (flags & wn::DCX_CLIPCHILDREN) != 0, siblings = (flags & wn::DCX_CLIPSIBLINGS) != 0;
  if (flags & wn::DCX_USESTYLE) {
    children = children || (w->style & wn::WS_CLIPCHILDREN);
    siblings = siblings || (w->style & wn::WS_CLIPSIBLINGS);
  }
  List<Rect> region(S().alloc);
  const bool use_region = (flags & (wn::DCX_EXCLUDERGN | wn::DCX_INTERSECTRGN)) != 0 && call.arg32(1) != 0;
  if (use_region && !gdi::region_rects(call.arg32(1), &region)) return call.fail(kErrorInvalidHandle);
  Window* target = w;
  if ((flags & wn::DCX_PARENTCLIP) && w->parent && w->parent->parent) target = w->parent;
  const uint32_t dc = create_window_dc(call, target, (flags & wn::DCX_WINDOW) != 0, children, siblings, use_region ? &region : nullptr, (flags & wn::DCX_EXCLUDERGN) != 0);
  if (dc != 0) for (WinDc& d : S().dcs) if (d.dc == dc) d.hwnd = w->handle;
  return dc;
}

// INT NtUserReleaseDC(HWND, HDC) - or NtUserReleaseDC(HDC), as this system's user32 calls it (the window is not needed: a DC knows its window)
uint64_t nt_release_dc(Call& call) {
  State& s = S();
  uint32_t dc = call.arg32(1);
  uint32_t hwnd = call.arg32(0);
  if (!gdi::is_dc(dc) && gdi::is_dc(call.arg32(0))) { dc = call.arg32(0); hwnd = 0; }
  for (WinDc& d : s.dcs) {
    if (d.dc != dc) continue;
    if (d.pid != call.pid()) return 0;
    if (hwnd != 0 && d.hwnd != hwnd) return 0;
    const bool ok = gdi::delete_dc(dc);
    s.dcs.erase(&d);
    return ok ? 1 : 0;
  }
  // a DC of the whole screen (GetDC(NULL)) is not tracked here
  if (hwnd == 0 && gdi::is_screen_dc(dc) && gdi::dc_owner(dc) == call.pid()) return gdi::delete_dc(dc) ? 1 : 0;
  return 0;
}

// HWND NtUserWindowFromDC(HDC)
uint64_t nt_window_from_dc(Call& call) {
  const uint32_t dc = call.arg32(0);
  for (WinDc& d : S().dcs) if (d.dc == dc) return d.hwnd;
  // A DC of the whole screen (GetDC(NULL)) belongs to the desktop window, as on Windows: ANGLE takes such a DC as its display and checks it this way.
  if (gdi::is_screen_dc(dc)) return user::desktop_window();
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Update regions

void paint_region_changed(Window* w) {
  if (!w) return;
  if (Queue* q = find_queue(w->thread)) q->changed |= wn::QS_PAINT;
  wake_all();
}

void invalidate_window(Window* w, const Rect* r, bool erase) {
  const Rect whole{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top};
  const Rect area = r ? rect_intersect(*r, whole) : whole;
  if (!rect_empty(area)) region_add(w->update, area);
  if (erase) w->erase_pending = true;
  if (!r && erase) w->nc_paint_pending = true;
  paint_region_changed(w);
}

// The area of a window that is not clipped away from its children (no WS_CLIPCHILDREN) is also the children's: they are invalidated where they overlap it, so they are painted with the parent.
void invalidate_children(Window* w, const Rect& area) {
  if ((w->style & wn::WS_CLIPCHILDREN) || rect_empty(area)) return;
  int32_t wx = 0, wy = 0;
  client_origin(w, &wx, &wy);
  for (Window* c = w->first_child; c; c = c->next) {
    if (!(c->style & wn::WS_VISIBLE)) continue;
    int32_t cx = 0, cy = 0;
    client_origin(c, &cx, &cy);
    const Rect inside = rect_offset(area, wx - cx, wy - cy);      // (the area in the child's client coordinates)
    const Rect mine = rect_intersect(inside, Rect{0, 0, c->client.right - c->client.left, c->client.bottom - c->client.top});
    if (rect_empty(mine)) continue;
    invalidate_window(c, &mine, false);
    invalidate_children(c, mine);
  }
}

void validate_window(Window* w, const Rect* r) {
  if (!r) {
    w->update.clear();
    w->erase_pending = false;
    w->nc_paint_pending = false;
    w->paint_pending = false;
  } else {
    region_subtract(w->update, *r);
    if (w->update.count() == 0) { w->erase_pending = false; w->paint_pending = false; }
  }
}

namespace {

constexpr uint32_t kNoArg = 99;      // (no region argument: the call takes a rectangle only)
bool rects_from_args(Call& call, uint32_t rect_index, uint32_t rgn_index, List<Rect>* out, bool* whole) {
  *whole = false;
  if (rgn_index != kNoArg && call.arg(rgn_index) != 0) {
    if (!gdi::region_rects(call.arg32(rgn_index), out)) { call.set_last_error(kErrorInvalidHandle); return false; }
    return true;
  }
  if (call.arg(rect_index) != 0) {
    Rect r;
    if (!read_rect(call, call.arg(rect_index), &r)) { call.set_last_error(kErrorInvalidParameter); return false; }
    if (!rect_empty(r)) out->push_back(r);
    return true;
  }
  *whole = true;
  return true;
}

// BOOL NtUserInvalidateRect(HWND, const RECT*, BOOL erase)
uint64_t invalidate_impl(Call& call, bool is_rgn, bool validate) {
  if (call.arg(0) != 0 && !find_window(call.arg32(0))) return call.fail(kErrorInvalidWindowHandle);
  List<Rect> rects(S().alloc);
  bool whole = false;
  if (is_rgn) {
    if (call.arg(1) != 0) {
      if (!gdi::region_rects(call.arg32(1), &rects)) return call.fail(kErrorInvalidHandle);
    } else whole = true;
  } else if (!rects_from_args(call, 1, kNoArg, &rects, &whole)) return 0;
  {      // (an unaware caller's 96-dpi units into device pixels)
    const uint32_t k = caller_scale(call);
    if (k > 100) for (Rect& r : rects) r = dpi_up(r, k);
  }
  const bool erase = !validate && call.arg_bool(2);
  auto apply = [&](Window* w) {
    if (validate) {
      if (whole) validate_window(w, nullptr);
      else for (const Rect& r : rects) validate_window(w, &r);
    } else {
      if (whole) invalidate_window(w, nullptr, erase);
      else { for (const Rect& r : rects) invalidate_window(w, &r, false); if (erase && w->update.count()) w->erase_pending = true; }
      if (whole) invalidate_children(w, Rect{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top});
      else for (const Rect& r : rects) invalidate_children(w, r);
    }
  };
  if (call.arg(0) == 0) {
    Desktop* d = thread_desktop(call);
    if (d && d->root) for (Window& w : S().windows.items()) if (w.desktop == d && w.parent && w.parent->parent == nullptr) apply(&w);
    return 1;
  }
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  apply(w);
  return 1;
}
uint64_t nt_invalidate_rect(Call& call) { return invalidate_impl(call, false, false); }
uint64_t nt_invalidate_rgn(Call& call) { return invalidate_impl(call, true, false); }
uint64_t nt_validate_rect(Call& call) { return invalidate_impl(call, false, true); }
uint64_t nt_validate_rgn(Call& call) { return invalidate_impl(call, true, true); }

// a[0] = hwnd, a[1] = hdc; erase the background
}  // namespace

Upcall* queue_erase(Call& call, Op* op, Window* w, uint32_t* dc_out) {
  *dc_out = 0;
  List<Rect> area(S().alloc);
  region_copy(area, w->update);
  {      // (the DC is in the window's coordinates)
    const uint32_t k = window_scale(w);
    if (k > 100) for (Rect& r : area) r = dpi_cover(r, k);
  }
  const uint32_t dc = create_window_dc(call, w, false, (w->style & wn::WS_CLIPCHILDREN) != 0, (w->style & wn::WS_CLIPSIBLINGS) != 0, &area, false);
  *dc_out = dc;
  if (dc == 0) return nullptr;
  return send_proc_upcall(call, op, w, wn::WM_ERASEBKGND, dc, 0);
}

namespace {

// ---- GetUpdateRect / GetUpdateRgn

// a[0] = hwnd, a[1] = hdc (erase in progress)
uint64_t erase_done_step(Call& call, Op& op) {
  if (op.a[1] != 0) {
    gdi::delete_dc(uint32_t(op.a[1]));
    for (WinDc& d : S().dcs) if (d.dc == op.a[1]) { S().dcs.erase(&d); break; }
  }
  return op.a[2];
}

// BOOL NtUserGetUpdateRect(HWND, RECT*, BOOL erase)
uint64_t nt_get_update_rect(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const bool nonempty = w->update.count() != 0;
  if (call.arg(1) != 0) {
    const Rect bounds = dpi_cover(nonempty ? region_bounds(w->update) : Rect(), caller_scale(call));
    if (!write_rect(call, call.arg(1), bounds)) return call.fail(kErrorInvalidParameter);
  }
  if (call.arg_bool(2) && nonempty && w->erase_pending) {
    Op* op = op_begin(call, erase_done_step);
    if (!op) return nonempty;
    op->a[2] = nonempty;
    w->erase_pending = false;
    uint32_t dc = 0;
    queue_erase(call, op, w, &dc);
    op->a[1] = dc;
    return op_continue(call, *op);
  }
  return nonempty;
}

// INT NtUserGetUpdateRgn(HWND, HRGN, BOOL erase): NULLREGION 1, SIMPLEREGION 2, COMPLEXREGION 3, ERROR 0
uint64_t nt_get_update_rgn(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (!gdi::is_region(call.arg32(1))) return call.fail(kErrorInvalidHandle);
  {
    const uint32_t k = caller_scale(call);
    List<Rect> rects(S().alloc);
    region_copy(rects, w->update);
    if (k > 100) for (Rect& r : rects) r = dpi_cover(r, k);
    if (!gdi::replace_region(call.arg32(1), rects.view())) return 0;
  }
  const uint64_t type = w->update.count() == 0 ? 1 : w->update.count() == 1 ? 2 : 3;
  if (call.arg_bool(2) && w->update.count() != 0 && w->erase_pending) {
    Op* op = op_begin(call, erase_done_step);
    if (!op) return type;
    op->a[2] = type;
    w->erase_pending = false;
    uint32_t dc = 0;
    queue_erase(call, op, w, &dc);
    op->a[1] = dc;
    return op_continue(call, *op);
  }
  return type;
}

// ---- BeginPaint / EndPaint
// a[0] = hwnd, a[1] = PAINTSTRUCT*, a[2] = hdc, a[3] = erase result; phases: 0 WM_NCPAINT, 1 create the DC and WM_ERASEBKGND, 2 fill the PAINTSTRUCT
uint64_t begin_paint_step(Call& call, Op& op) {
  Window* w = find_window(uint32_t(op.a[0]));
  if (!w) return 0;
  if (op.phase == 0) {
    op.phase = 1;
    if (w->nc_paint_pending) {
      w->nc_paint_pending = false;
      if (send_proc_upcall(call, &op, w, wn::WM_NCPAINT, 1, 0) || op.waiting_on) return kUpcallDue;
    }
  }
  if (op.phase == 1) {
    op.phase = 2;
    List<Rect> area(S().alloc);
    region_copy(area, w->update);
    {      // (the DC is in the window's coordinates)
      const uint32_t k = window_scale(w);
      if (k > 100) for (Rect& r : area) r = dpi_cover(r, k);
    }
    const uint32_t dc = create_window_dc(call, w, false, (w->style & wn::WS_CLIPCHILDREN) != 0, (w->style & wn::WS_CLIPSIBLINGS) != 0, &area, false);
    if (dc == 0) return 0;
    op.a[2] = dc;
    op.a[3] = 0;
    if (w->erase_pending) {
      w->erase_pending = false;
      op.a[3] = 1;      // a procedure that does not erase leaves fErase TRUE
      if (send_proc_upcall(call, &op, w, wn::WM_ERASEBKGND, dc, 0) || op.waiting_on) return kUpcallDue;
    }
  }
  w = find_window(uint32_t(op.a[0]));
  if (!w) return 0;
  uint8_t raw[72] = {};
  const Rect bounds = dpi_cover(w->update.count() ? region_bounds(w->update) : Rect(), window_scale(w));
  const bool erased = op.a[3] == 1 && op.first_result != 0;
  put32(raw, 0, uint32_t(op.a[2]));
  put32(raw, 8, op.a[3] == 1 && !erased ? 1 : 0);
  put32(raw, 12, uint32_t(bounds.left)); put32(raw, 16, uint32_t(bounds.top)); put32(raw, 20, uint32_t(bounds.right)); put32(raw, 24, uint32_t(bounds.bottom));
  validate_window(w, nullptr);
  if (!call.write(op.a[1], ConstByteView(raw, 72))) {
    gdi::delete_dc(uint32_t(op.a[2]));
    for (WinDc& d : S().dcs) if (d.dc == op.a[2]) { S().dcs.erase(&d); break; }
    return call.fail(kErrorInvalidParameter);
  }
  return op.a[2];
}

// HDC NtUserBeginPaint(HWND, PAINTSTRUCT*)
uint64_t nt_begin_paint(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  Op* op = op_begin(call, begin_paint_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  op->a[1] = call.arg(1);
  return op_continue(call, *op);
}

// BOOL NtUserEndPaint(HWND, const PAINTSTRUCT*)
uint64_t nt_end_paint(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint64_t hdc = 0;
  if (!call.read_u64(call.arg(1), &hdc)) return call.fail(kErrorInvalidParameter);
  const uint32_t dc = uint32_t(hdc);
  for (WinDc& d : S().dcs) {
    if (d.dc != dc || d.hwnd != w->handle) continue;
    gdi::delete_dc(dc);
    S().dcs.erase(&d);
    return 1;
  }
  return 1;      // (EndPaint always answers TRUE)
}

// ---- RedrawWindow / UpdateWindow
// a[0] = hwnd, a[1] = flags; sends the paint messages after the regions were changed. phase 0: WM_NCPAINT/ERASE/PAINT for the window, then children.
void collect_paint_targets(Window* w, bool recurse, List<uint32_t>& out) {
  out.push_back(w->handle);
  if (!recurse) return;
  for (Window* c = w->first_child; c; c = c->next) collect_paint_targets(c, true, out);
}

uint64_t paint_all_step(Call& call, Op& op) {
  // sends WM_PAINT to each window with an update region once (children first when ALLCHILDREN), tracking visited windows in a list kept in the operation payload
  Window* root = find_window(uint32_t(op.a[0]));
  if (!root) return op.a[2];
  const uint32_t flags = uint32_t(op.a[1]);
  List<uint32_t> targets(S().alloc);
  collect_paint_targets(root, (flags & wn::RDW_ALLCHILDREN) != 0, targets);
  // visited windows are encoded as 4-byte entries in op.payload
  for (uint32_t h : targets) {
    bool visited = false;
    for (uint64_t i = 0; i + 4 <= op.payload.count(); i += 4) {
      const uint32_t v = get32(op.payload.view().data(), uint32_t(i));
      if (v == h) { visited = true; break; }
    }
    if (visited) continue;
    Window* w = find_window(h);
    uint8_t raw[4];
    put32(raw, 0, h);
    for (uint8_t b : raw) op.payload.push_back(b);
    if (!w || !is_visible(w)) continue;
    const bool erase_only = (flags & wn::RDW_ERASENOW) && !(flags & wn::RDW_UPDATENOW);
    if (erase_only) {
      if (!w->erase_pending) continue;
      w->erase_pending = false;
      uint32_t dc = 0;
      queue_erase(call, &op, w, &dc);
      if (dc != 0) { gdi::delete_dc(dc); for (WinDc& d : S().dcs) if (d.dc == dc) { S().dcs.erase(&d); break; } }
      if (op.outstanding || op.waiting_on) return kUpcallDue;
      continue;
    }
    if (w->update.count() == 0 && !w->paint_pending) continue;
    if (w->nc_paint_pending) { w->nc_paint_pending = false; send_proc_upcall(call, &op, w, wn::WM_NCPAINT, 1, 0); }
    w->paint_pending = false;
    send_proc_upcall(call, &op, w, wn::WM_PAINT, 0, 0);
    if (op.outstanding || op.waiting_on) return kUpcallDue;
  }
  return op.a[2];
}

// BOOL NtUserRedrawWindow(HWND, const RECT*, HRGN, UINT flags)
uint64_t nt_redraw_window(Call& call) {
  Desktop* d = thread_desktop(call);
  Window* w = call.arg(0) ? window_arg(call, 0) : d->root;
  if (!w) return 0;
  const uint32_t flags = call.arg32(3);
  List<Rect> rects(S().alloc);
  bool whole = false;
  if (call.arg(2) != 0) {
    if (!gdi::region_rects(call.arg32(2), &rects)) return call.fail(kErrorInvalidHandle);
  } else if (call.arg(1) != 0) {
    Rect r;
    if (!read_rect(call, call.arg(1), &r)) return call.fail(kErrorInvalidParameter);
    rects.push_back(r);
  } else whole = true;
  {
    const uint32_t k = caller_scale(call);
    if (k > 100) for (Rect& r : rects) r = dpi_up(r, k);
  }
  const bool children = (flags & wn::RDW_ALLCHILDREN) != 0;
  List<uint32_t> targets(S().alloc);
  collect_paint_targets(w, children, targets);
  for (uint32_t h : targets) {
    Window* t = find_window(h);
    if (!t) continue;
    // the region is in the client coordinates of `w`; for a descendant it is shifted into its own client area
    int32_t dx = 0, dy = 0;
    if (t != w) {
      int32_t wx, wy, tx, ty;
      client_origin(w, &wx, &wy);
      client_origin(t, &tx, &ty);
      dx = wx - tx;
      dy = wy - ty;
    }
    if (flags & wn::RDW_INVALIDATE) {
      if (whole) invalidate_window(t, nullptr, (flags & wn::RDW_ERASE) != 0);
      else {
        for (const Rect& r : rects) { const Rect shifted = rect_offset(r, dx, dy); invalidate_window(t, &shifted, false); }
        if ((flags & wn::RDW_ERASE) && t->update.count()) t->erase_pending = true;
      }
      if (flags & wn::RDW_FRAME) t->nc_paint_pending = true;
    } else if (flags & wn::RDW_VALIDATE) {
      if (whole) validate_window(t, nullptr);
      else for (const Rect& r : rects) { const Rect shifted = rect_offset(r, dx, dy); validate_window(t, &shifted); }
      if (flags & wn::RDW_NOFRAME) t->nc_paint_pending = false;
    }
    if (flags & wn::RDW_INTERNALPAINT) { t->paint_pending = true; paint_region_changed(t); }
    if (flags & wn::RDW_NOINTERNALPAINT) t->paint_pending = false;
    if ((flags & wn::RDW_ERASE) && !(flags & wn::RDW_INVALIDATE) && t->update.count()) t->erase_pending = true;
    if (flags & wn::RDW_NOERASE) t->erase_pending = false;
  }
  if (flags & (wn::RDW_UPDATENOW | wn::RDW_ERASENOW)) {
    Op* op = op_begin(call, paint_all_step);
    if (!op) return call.fail(kErrorNotEnoughMemory);
    op->a[0] = w->handle;
    op->a[1] = flags;
    op->a[2] = 1;
    return op_continue(call, *op);
  }
  return 1;
}

// BOOL NtUserUpdateWindow(HWND): sends WM_PAINT at once when the window has an update region
uint64_t nt_update_window(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  Op* op = op_begin(call, paint_all_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  op->a[1] = wn::RDW_UPDATENOW | wn::RDW_ALLCHILDREN;      // (the window and its children that have something to paint)
  op->a[2] = 1;
  return op_continue(call, *op);
}

// BOOL NtUserLockWindowUpdate(HWND)
uint64_t nt_lock_window_update(Call& call) {
  State& s = S();
  const uint64_t tid = thread_of(call);
  if (call.arg(0) == 0) {
    if (s.lock_update_window != 0 && s.lock_update_thread == tid) { s.lock_update_window = 0; s.lock_update_thread = 0; invalidate_screen_rect(Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())}, nullptr); }
    return 1;
  }
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (s.lock_update_window != 0 && s.lock_update_thread != tid) return 0;
  s.lock_update_window = w->handle;
  s.lock_update_thread = tid;
  return 1;
}

// ---- scroll bars

void set_scroll_style(Window* w, uint32_t bar, bool show) {
  const uint32_t bit = bar == wn::SB_HORZ ? wn::WS_HSCROLL : wn::WS_VSCROLL;
  const bool had = (w->style & bit) != 0;
  if (show == had) return;
  if (show) w->style |= bit; else w->style &= ~bit;
  compute_nc(w, &w->client);
  if (w->style & wn::WS_VISIBLE) { invalidate_window(w, nullptr, true); w->nc_paint_pending = true; }
}

// INT NtUserSetScrollInfo(HWND, INT bar, const SCROLLINFO*, BOOL redraw): SCROLLINFO { cbSize, fMask, nMin, nMax, nPage, nPos, nTrackPos } = 28 bytes
uint64_t nt_set_scroll_info(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t bar = call.arg32(1);
  uint8_t raw[28];
  if (!call.read(call.arg(2), ByteView(raw, 28)) || get32(raw, 0) != 28) return call.fail(kErrorInvalidParameter);
  if (bar > wn::SB_CTL) return call.fail(kErrorInvalidParameter);
  const uint32_t mask = get32(raw, 4);
  if (mask & ~0x101Fu & ~0x1000u) return call.fail(kErrorInvalidFlags);
  if (bar == wn::SB_CTL) return call.fail(kErrorInvalidParameter);      // the state of a scroll bar control belongs to the control (user32's class)
  ScrollBar& sb = w->scroll[bar];
  const int32_t old_pos = sb.pos;
  int32_t min = sb.min, max = sb.max;
  uint32_t page = uint32_t(sb.page);
  if (mask & wn::SIF_RANGE) {
    min = int32_t(get32(raw, 8));
    max = int32_t(get32(raw, 12));
    if (max < min) max = min;
    if (int64_t{max} - int64_t{min} > 0x7FFFFFFF) return call.fail(kErrorInvalidScrollbarRange);
  }
  if (mask & wn::SIF_PAGE) {
    page = get32(raw, 16);
    if (int32_t(page) < 0) page = 0;
    const int64_t span = int64_t{max} - int64_t{min} + 1;
    if (page > span) page = uint32_t(span);
  }
  int32_t pos = (mask & wn::SIF_POS) ? int32_t(get32(raw, 20)) : sb.pos;
  sb.min = min;
  sb.max = max;
  sb.page = int32_t(page);
  const int32_t top = max - (page > 0 ? int32_t(page) - 1 : 0);
  if (pos < min) pos = min;
  if (pos > top) pos = top < min ? min : top;
  sb.pos = pos;
  if (mask & wn::SIF_DISABLENOSCROLL) sb.disabled = (max - min < int32_t(page) || max == min) ? 3 : 0;
  const bool scrollable = max > min && int64_t{max} - min >= int64_t{page > 0 ? page : 0} && !(page > 0 && int64_t{max} - min + 1 <= page);
  sb.visible = scrollable;
  if (!(mask & wn::SIF_DISABLENOSCROLL) && (mask & (wn::SIF_RANGE | wn::SIF_PAGE))) set_scroll_style(w, bar, scrollable);
  return (mask & wn::SIF_RETURNPREV) ? uint64_t(uint32_t(old_pos)) : uint64_t(uint32_t(pos));
}

// BOOL NtUserShowScrollBar(HWND, INT bar, BOOL show)
uint64_t nt_show_scroll_bar(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t bar = call.arg32(1);
  const bool show = call.arg_bool(2);
  if (bar > wn::SB_BOTH) return call.fail(kErrorInvalidParameter);
  bool changed = false;
  if (bar == wn::SB_HORZ || bar == wn::SB_BOTH) { changed |= (((w->style & wn::WS_HSCROLL) != 0) != show); set_scroll_style(w, wn::SB_HORZ, show); }
  if (bar == wn::SB_VERT || bar == wn::SB_BOTH) { changed |= (((w->style & wn::WS_VSCROLL) != 0) != show); set_scroll_style(w, wn::SB_VERT, show); }
  return 1;
}

// BOOL NtUserEnableScrollBar(HWND, UINT bar, UINT flags)
uint64_t nt_enable_scroll_bar(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t bar = call.arg32(1), flags = call.arg32(2);
  if (bar > wn::SB_BOTH || flags > 3) return call.fail(kErrorInvalidParameter);
  bool changed = false;
  for (uint32_t b = 0; b < 2; ++b) {
    if (!(bar == b || bar == wn::SB_BOTH)) continue;
    if (w->scroll[b].disabled != flags) { w->scroll[b].disabled = flags; changed = true; }
  }
  return changed;
}

}  // namespace

void register_paint_services() {
  register_handler(WindowsSyscallNumber::NtUserGetDC, nt_get_dc);
  register_handler(WindowsSyscallNumber::NtUserGetWindowDC, nt_get_window_dc);
  register_handler(WindowsSyscallNumber::NtUserGetDCEx, nt_get_dc_ex);
  register_handler(WindowsSyscallNumber::NtUserReleaseDC, nt_release_dc);
  register_handler(WindowsSyscallNumber::NtUserWindowFromDC, nt_window_from_dc);
  register_handler(WindowsSyscallNumber::NtUserInvalidateRect, nt_invalidate_rect);
  register_handler(WindowsSyscallNumber::NtUserInvalidateRgn, nt_invalidate_rgn);
  register_handler(WindowsSyscallNumber::NtUserValidateRect, nt_validate_rect);
  register_handler(WindowsSyscallNumber::NtUserValidateRgn, nt_validate_rgn);
  register_handler(WindowsSyscallNumber::NtUserGetUpdateRect, nt_get_update_rect);
  register_handler(WindowsSyscallNumber::NtUserGetUpdateRgn, nt_get_update_rgn);
  register_handler(WindowsSyscallNumber::NtUserBeginPaint, nt_begin_paint);
  register_handler(WindowsSyscallNumber::NtUserEndPaint, nt_end_paint);
  register_handler(WindowsSyscallNumber::NtUserRedrawWindow, nt_redraw_window);
  register_handler(WindowsSyscallNumber::NtUserUpdateWindow, nt_update_window);
  register_handler(WindowsSyscallNumber::NtUserLockWindowUpdate, nt_lock_window_update);
  register_handler(WindowsSyscallNumber::NtUserSetScrollInfo, nt_set_scroll_info);
  register_handler(WindowsSyscallNumber::NtUserShowScrollBar, nt_show_scroll_bar);
  register_handler(WindowsSyscallNumber::NtUserEnableScrollBar, nt_enable_scroll_bar);
}

}  // namespace process::win32k::user
