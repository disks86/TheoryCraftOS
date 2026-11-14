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
// Window geometry: the non-client area, SetWindowPos and its relatives (move, show, placement, deferred positioning), z-order, hit testing services.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }

// GetSystemMetrics indices.
constexpr uint32_t SM_CXVSCROLL = 2, SM_CYHSCROLL = 3, SM_CYCAPTION = 4, SM_CXFRAME = 32, SM_CYFRAME = 33, SM_CXDLGFRAME = 7, SM_CYDLGFRAME = 8, SM_CYMENU = 15, SM_CXEDGE = 45,
                   SM_CYEDGE = 46, SM_CYSMCAPTION = 51, SM_CXPADDEDBORDER = 92, SM_CXBORDER = 5, SM_CYBORDER = 6;

void put32(uint8_t* raw, uint32_t o, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) raw[o + i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* raw, uint32_t o, uint64_t v) { put32(raw, o, uint32_t(v)); put32(raw, o + 4, uint32_t(v >> 32)); }
uint32_t get32(const uint8_t* raw, uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; }
uint64_t get64(const uint8_t* raw, uint32_t o) { return uint64_t(get32(raw, o)) | uint64_t(get32(raw, o + 4)) << 32; }

struct Margins { int32_t left = 0, top = 0, right = 0, bottom = 0; };

// How far the window rectangle reaches past the client rectangle for a style (AdjustWindowRectEx; the algorithm of Windows NT with the metrics of Windows 10).
Margins nc_margins(uint32_t style, uint32_t ex_style, bool has_menu, uint32_t dpi, bool padded = true, const Window* top = nullptr) {
  Margins m;
  if (redirect_config().on && (style & wn::WS_CAPTION) == wn::WS_CAPTION && !(style & wn::WS_CHILD)) {
    // the kernel's desktop draws the frame of a top-level with a caption: its sizes (a menu bar and a client edge are still the window's own);
    // a top-level on a monitor at another scale has it at that monitor's
    int32_t border = 0, title = 0;
    frame_of(top, &border, &title);
    m.left = m.right = m.bottom = border;
    m.top = border + title;
    (void)padded;
    if (has_menu) m.top += user_misc::system_metrics(SM_CYMENU, dpi);
    if (ex_style & wn::WS_EX_CLIENTEDGE) {
      m.left += user_misc::system_metrics(SM_CXEDGE, 96);
      m.right += user_misc::system_metrics(SM_CXEDGE, 96);
      m.top += user_misc::system_metrics(SM_CYEDGE, 96);
      m.bottom += user_misc::system_metrics(SM_CYEDGE, 96);
    }
    return m;
  }
  int32_t adjust = 0;
  if ((ex_style & (wn::WS_EX_STATICEDGE | wn::WS_EX_DLGMODALFRAME)) == wn::WS_EX_STATICEDGE) adjust = 1;
  else if ((ex_style & wn::WS_EX_DLGMODALFRAME) || (style & (wn::WS_THICKFRAME | wn::WS_DLGFRAME))) adjust = 2;
  int32_t xpad = 0, ypad = 0;
  if (style & wn::WS_THICKFRAME) {
    adjust += user_misc::system_metrics(SM_CXFRAME, dpi) - user_misc::system_metrics(SM_CXDLGFRAME, dpi);
    if (padded) xpad = ypad = user_misc::system_metrics(SM_CXPADDEDBORDER, dpi);
  }
  if ((style & (wn::WS_BORDER | wn::WS_DLGFRAME)) || (ex_style & wn::WS_EX_DLGMODALFRAME)) ++adjust;
  m.left = m.right = adjust + xpad;
  m.top = m.bottom = adjust + ypad;
  if ((style & wn::WS_CAPTION) == wn::WS_CAPTION) {      // (the caption height scaled, plus one pixel that is not)
    const int32_t extra = int32_t(dpi / 96) > 1 ? int32_t(dpi / 96) - 1 : 0;
    m.top += user_misc::system_metrics((ex_style & wn::WS_EX_TOOLWINDOW) ? SM_CYSMCAPTION : SM_CYCAPTION, dpi) - extra;
  }
  if (has_menu) m.top += user_misc::system_metrics(SM_CYMENU, dpi);
  if (ex_style & wn::WS_EX_CLIENTEDGE) {
    m.left += user_misc::system_metrics(SM_CXEDGE, 96);      // (the client edge is not scaled)
    m.right += user_misc::system_metrics(SM_CXEDGE, 96);
    m.top += user_misc::system_metrics(SM_CYEDGE, 96);
    m.bottom += user_misc::system_metrics(SM_CYEDGE, 96);
  }
  return m;
}

uint32_t window_dpi_of(const Window* w) {
  (void)w;
  return 96;
}

}  // namespace

bool adjust_window_rect(gdi::Rect* rect, uint32_t style, uint32_t ex_style, bool has_menu, uint32_t dpi) {
  // AdjustWindowRectEx (dpi 0) leaves out the padded border of a sizing frame; the ForDpi form has it
  const Margins m = nc_margins(style, ex_style, has_menu && !(style & wn::WS_CHILD), dpi == 0 ? 96 : dpi, dpi != 0);
  rect->left = int32_t(uint32_t(rect->left) - uint32_t(m.left));
  rect->top = int32_t(uint32_t(rect->top) - uint32_t(m.top));
  rect->right = int32_t(uint32_t(rect->right) + uint32_t(m.right));
  rect->bottom = int32_t(uint32_t(rect->bottom) + uint32_t(m.bottom));
  return true;
}

// The client rectangle a window rectangle leaves (WM_NCCALCSIZE's default).
void compute_nc(Window* w, Rect* client_out) {
  if (w->parent == nullptr) { *client_out = w->bounds; return; }
  const uint32_t dpi = window_dpi_of(w);
  const bool menu = w->menu != 0 && !(w->style & wn::WS_CHILD);
  const Margins m = (w->style & wn::WS_MINIMIZE) ? Margins() : nc_margins(w->style, w->ex_style, menu, dpi, false, w);      // (programs are not DPI aware here: no padded border)
  Rect c = w->bounds;
  c.left = int32_t(uint32_t(c.left) + uint32_t(m.left));
  c.top = int32_t(uint32_t(c.top) + uint32_t(m.top));
  c.right = int32_t(uint32_t(c.right) - uint32_t(m.right));
  c.bottom = int32_t(uint32_t(c.bottom) - uint32_t(m.bottom));
  if (w->style & wn::WS_VSCROLL) {
    const int32_t sb = user_misc::system_metrics(SM_CXVSCROLL, dpi);
    if (w->ex_style & wn::WS_EX_LEFTSCROLLBAR) c.left += sb; else c.right -= sb;
  }
  if (w->style & wn::WS_HSCROLL) c.bottom -= user_misc::system_metrics(SM_CYHSCROLL, dpi);
  if (c.right < c.left) c.right = c.left;
  if (c.bottom < c.top) c.bottom = c.top;
  *client_out = c;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Z-order

namespace {
// The windows `w` owns go directly above it, recursively (apply_zorder has placed `w`).
void apply_owned_zorder(Window* w, uint32_t depth = 0) {
  Window* parent = w->parent;
  if (!parent || depth > 16) return;
  Window* owned[64];
  uint32_t n = 0;
  for (Window* c = parent->first_child; c && n < 64; c = c->next) if (c->owner == w && c != w) owned[n++] = c;
  for (uint32_t i = n; i > 0; --i) {
    Window* o = owned[i - 1];
    if (!(o->ex_style & wn::WS_EX_TOPMOST) && (w->ex_style & wn::WS_EX_TOPMOST)) continue;
    unlink_window(o);
    link_window(o, parent, w->prev, false);
  }
  for (uint32_t i = 0; i < n; ++i) apply_owned_zorder(owned[i], depth + 1);
}
}  // namespace

void apply_zorder(Window* w, uint32_t after) {
  Window* parent = w->parent;
  if (!parent) return;
  unlink_window(w);
  Window* insert_after = nullptr;
  bool at_end = false;
  auto topmost_end = [&]() -> Window* {
    Window* last = nullptr;
    for (Window* c = parent->first_child; c && (c->ex_style & wn::WS_EX_TOPMOST); c = c->next) last = c;
    return last;
  };
  const bool top_level = parent->parent == nullptr;
  if (after == kHwndTopmost && top_level) { w->ex_style |= wn::WS_EX_TOPMOST; insert_after = nullptr; }
  else if (after == kHwndNotopmost && top_level) { w->ex_style &= ~wn::WS_EX_TOPMOST; insert_after = topmost_end(); }
  else if (after == kHwndTop) { insert_after = (top_level && !(w->ex_style & wn::WS_EX_TOPMOST)) ? topmost_end() : nullptr; }
  else if (after == kHwndBottom) {
    at_end = true;
    if (top_level) w->ex_style &= ~wn::WS_EX_TOPMOST;
  } else if (Window* sibling = find_window(after)) {
    if (sibling->parent == parent && sibling != w) {
      insert_after = sibling;
      if (top_level) {      // a window placed behind a non-topmost window stops being topmost; one placed behind a topmost window becomes one
        if (sibling->ex_style & wn::WS_EX_TOPMOST) w->ex_style |= wn::WS_EX_TOPMOST; else w->ex_style &= ~wn::WS_EX_TOPMOST;
      }
    } else if (sibling == w) { insert_after = w->prev; }
  }
  link_window(w, parent, insert_after, at_end);
  if (!top_level) return;
  if (!at_end) apply_owned_zorder(w);      // the windows it owns stay above it (a dialog above its main window), theirs above them in turn
  notify_top_level(w);
}

void invalidate_screen_rect(const Rect& screen, const Window* except_tree) {
  Desktop* d = default_desktop();
  if (!d || !d->root) return;
  if (except_tree && redirect_config().on) {
    // the desktop composites the top-levels: what a window uncovers is only ever in its own top-level (a top-level uncovers nothing of another)
    const Window* walk = except_tree;
    while (walk->parent && walk->parent != d->root) walk = walk->parent;
    if (walk == except_tree || walk->parent != d->root) return;
    Window* top = find_window(walk->handle);      // (the same window, as the state holds it)
    if (!top) return;
    struct Within {
      const Rect& area;
      const Window* except;
      void visit(Window* parent) {
        for (Window* c = parent->first_child; c; c = c->next) {
          if (c == except || !(c->style & wn::WS_VISIBLE)) continue;
          const Rect hit = rect_intersect(area, client_rect_screen(c));
          if (!rect_empty(hit)) {
            int32_t ox, oy;
            client_origin(c, &ox, &oy);
            const Rect local = rect_offset(hit, -ox, -oy);
            invalidate_window(c, &local, true);
          }
          visit(c);
        }
      }
    } within{screen, except_tree};
    const Rect hit = rect_intersect(screen, client_rect_screen(top));
    if (!rect_empty(hit)) {
      int32_t ox, oy;
      client_origin(top, &ox, &oy);
      const Rect local = rect_offset(hit, -ox, -oy);
      invalidate_window(top, &local, true);
    }
    within.visit(top);
    return;
  }
  struct Walker {
    const Rect& area;
    const Window* except;
    void visit(Window* parent) {
      for (Window* c = parent->first_child; c; c = c->next) {
        if (c == except || !(c->style & wn::WS_VISIBLE)) continue;
        const Rect cl = client_rect_screen(c);
        const Rect hit = rect_intersect(area, cl);
        if (!rect_empty(hit)) {
          int32_t ox, oy;
          client_origin(c, &ox, &oy);
          const Rect local = rect_offset(hit, -ox, -oy);
          c->update.count();      // (touch)
          invalidate_window(c, &local, true);
        }
        visit(c);
      }
    }
  } walker{screen, except_tree};
  walker.visit(d->root);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// SetWindowPos and the operations that share it
//
// a[0] = hwnd, a[1] = hwndInsertAfter, a[2] = x | y << 32, a[3] = cx | cy << 32, a[4] = flags, a[5] = show command (0xFFFF: none),
// a[8] = options (1: defer list, 2: answer the visibility ShowWindow reports), a[9] = item index, a[10] = HDWP, a[11] = previous visibility
// phases: 0 WM_WINDOWPOSCHANGING, 1 apply + WM_NCCALCSIZE, 2 WM_SHOWWINDOW + WM_WINDOWPOSCHANGED + paint, 3 next item or answer

namespace {

constexpr uint32_t kNoShow = kSwpNoShow;

Rect iconic_rect(Window* w) {
  const int32_t iw = 160, ih = 28;
  if (w->placement.min_x != -1 || w->placement.min_y != -1) return Rect{w->placement.min_x, w->placement.min_y, w->placement.min_x + iw, w->placement.min_y + ih};
  const int32_t sh = int32_t(gdi::screen_height());
  return Rect{0, sh - ih, iw, sh};
}

// The rectangle of a maximized window: the work area (the whole monitor for a popup, the client area of the parent for a child) grown by what AdjustWindowRectEx adds for the style without WS_BORDER.
Rect maximized_rect(Window* w) {
  const bool popup = (w->style & wn::WS_POPUP) != 0;
  const Rect at = w->parent ? window_rect_screen(w) : Rect{};
  Rect area = user_misc::monitor_rect(user_misc::monitor_from_rect(gdi::Rect{at.left, at.top, at.right, at.bottom}, 2), !popup);      // (the monitor the window is mostly on)
  if ((w->style & wn::WS_CHILD) && !popup && w->parent && w->parent->parent) area = Rect{0, 0, w->parent->client.right - w->parent->client.left, w->parent->client.bottom - w->parent->client.top};
  if (redirected(w) && (w->style & wn::WS_CAPTION) == wn::WS_CAPTION) return area;      // (the desktop's maximized frame fills the work area, frame and all)
  gdi::Rect r{area.left, area.top, area.right, area.bottom};
  adjust_window_rect(&r, w->style & ~wn::WS_BORDER, w->ex_style, false, 0);
  return Rect{r.left, r.top, r.right, r.bottom};
}

// Translates a ShowWindow command into style bits, the new rectangle and SetWindowPos flags. False: nothing to do (the answer is just the old visibility).
bool apply_show_cmd(Window* w, uint32_t cmd, uint32_t* flags, Rect* rect, bool* activate) {
  const bool was_visible = (w->style & wn::WS_VISIBLE) != 0;
  *activate = false;
  *flags = wn::SWP_NOZORDER | wn::SWP_NOACTIVATE;
  const Rect old = w->bounds;
  Rect target = w->bounds;
  bool frame_change = false;
  auto save_normal = [&]() {
    if (!(w->style & (wn::WS_MINIMIZE | wn::WS_MAXIMIZE))) { w->placement.normal = w->bounds; w->placement.has_normal = true; }
  };
  switch (cmd) {
    case wn::SW_HIDE:
      if (!was_visible) return false;
      *flags |= wn::SWP_HIDEWINDOW | wn::SWP_NOSIZE | wn::SWP_NOMOVE;
      break;
    case wn::SW_SHOWNORMAL: case wn::SW_RESTORE: case wn::SW_SHOWDEFAULT: case wn::SW_SHOW: case wn::SW_SHOWNA: case wn::SW_SHOWNOACTIVATE: {
      const bool restore = cmd == wn::SW_SHOWNORMAL || cmd == wn::SW_RESTORE || cmd == wn::SW_SHOWDEFAULT;
      if (cmd == wn::SW_SHOW && was_visible) return false;      // (SW_SHOW on a visible window changes nothing: no message is sent)
      if (restore && (w->style & (wn::WS_MINIMIZE | wn::WS_MAXIMIZE))) {
        const bool was_min = (w->style & wn::WS_MINIMIZE) != 0;
        w->style &= ~(wn::WS_MINIMIZE | wn::WS_MAXIMIZE);
        if (was_min && w->placement.restore_maximized) {
          w->style |= wn::WS_MAXIMIZE;
          target = maximized_rect(w);
        } else {
          target = w->placement.has_normal ? w->placement.normal : w->bounds;
        }
        frame_change = true;
      }
      *flags |= wn::SWP_SHOWWINDOW;
      if (cmd == wn::SW_SHOW || cmd == wn::SW_SHOWNORMAL || cmd == wn::SW_RESTORE || cmd == wn::SW_SHOWDEFAULT) { *flags &= ~wn::SWP_NOACTIVATE; *activate = true; }
      break;
    }
    case wn::SW_SHOWMINIMIZED: case wn::SW_MINIMIZE: case wn::SW_SHOWMINNOACTIVE: case wn::SW_FORCEMINIMIZE: {
      if (!(w->style & wn::WS_MINIMIZE)) {
        w->placement.restore_maximized = (w->style & wn::WS_MAXIMIZE) != 0;
        save_normal();
        w->style = (w->style & ~wn::WS_MAXIMIZE) | wn::WS_MINIMIZE;
        target = iconic_rect(w);
        frame_change = true;
      }
      *flags |= wn::SWP_SHOWWINDOW;
      if (cmd == wn::SW_SHOWMINIMIZED) { *flags &= ~wn::SWP_NOACTIVATE; *activate = true; }
      break;
    }
    case wn::SW_SHOWMAXIMIZED: {
      if (!(w->style & wn::WS_MAXIMIZE)) {
        if (!(w->style & wn::WS_MINIMIZE)) save_normal();
        w->style = (w->style & ~wn::WS_MINIMIZE) | wn::WS_MAXIMIZE;
        w->placement.restore_maximized = false;
        target = maximized_rect(w);
        frame_change = true;
      }
      *flags |= wn::SWP_SHOWWINDOW;
      *flags &= ~wn::SWP_NOACTIVATE;
      *activate = true;
      break;
    }
    default:
      return false;
  }
  if (frame_change) *flags |= wn::SWP_FRAMECHANGED;
  if (rect_equal(old, target)) *flags |= wn::SWP_NOMOVE | wn::SWP_NOSIZE;
  *rect = target;
  return true;
}

void read_windowpos_payload(const List<uint8_t>& p, uint32_t* after, int32_t* x, int32_t* y, int32_t* cx, int32_t* cy, uint32_t* flags) {
  if (p.count() < 48) return;
  const uint8_t* d = p.view().data();
  *after = uint32_t(get64(d, 8));
  *x = int32_t(get32(d, 16));
  *y = int32_t(get32(d, 20));
  *cx = int32_t(get32(d, 24));
  *cy = int32_t(get32(d, 28));
  *flags = get32(d, 32);
}

// (the WINDOWPOS in the window's coordinates: an unaware window's are its 96-dpi units)
void make_windowpos(const Window* w, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags, List<uint8_t>& out) {
  const uint32_t k = window_scale(w);
  uint8_t raw[48] = {};
  put64(raw, 0, w->handle);
  put64(raw, 8, after);
  put32(raw, 16, uint32_t(dpi_down(x, k)));
  put32(raw, 20, uint32_t(dpi_down(y, k)));
  put32(raw, 24, uint32_t(dpi_down(cx, k)));
  put32(raw, 28, uint32_t(dpi_down(cy, k)));
  put32(raw, 32, flags);
  out.clear();
  for (uint8_t b : raw) out.push_back(b);
}

void load_item(Op& op, uint32_t index) {
  State& s = S();
  DeferPos* d = s.defers.find(uint32_t(op.a[10]));
  if (!d || index >= d->items.count()) return;
  const DeferPos::Item& it = d->items[index];
  op.a[0] = it.hwnd;
  op.a[1] = it.after;
  op.a[2] = uint32_t(it.x) | uint64_t{uint32_t(it.y)} << 32;
  op.a[3] = uint32_t(it.cx) | uint64_t{uint32_t(it.cy)} << 32;
  op.a[4] = it.flags;
  op.a[5] = kNoShow;
  op.phase = 0;
}

// The item is done: the next one of a deferred list is loaded (returns true: keep going), else the service's answer is in *answer.
bool swp_next_item(Op& op, uint64_t* answer) {
  if (op.a[8] & 1) {
    ++op.a[9];
    DeferPos* d = S().defers.find(uint32_t(op.a[10]));
    if (d && op.a[9] < d->items.count()) {
      load_item(op, uint32_t(op.a[9]));
      return true;
    }
    if (d) S().defers.destroy(d);
  }
  *answer = (op.a[8] & 2) ? op.a[11] : 1;
  return false;
}

uint64_t swp_step(Call& call, Op& op) {
  for (;;) {
    Window* w = find_window(uint32_t(op.a[0]));
    if (!w || !w->parent) {
      uint64_t answer = 0;
      if ((op.a[8] & 1) && swp_next_item(op, &answer)) continue;
      return (op.a[8] & 1) ? answer : 0;
    }
    int32_t x = int32_t(uint32_t(op.a[2])), y = int32_t(uint32_t(op.a[2] >> 32)), cx = int32_t(uint32_t(op.a[3])), cy = int32_t(uint32_t(op.a[3] >> 32));
    uint32_t flags = uint32_t(op.a[4]);
    uint32_t after = uint32_t(op.a[1]);
    if (op.phase == 0) {
      op.phase = 1;
      if (op.a[5] != kNoShow) {
        Rect target;
        bool activate = false;
        uint32_t show_flags = 0;
        op.a[11] = (w->style & wn::WS_VISIBLE) != 0;
        if (!apply_show_cmd(w, uint32_t(op.a[5]), &show_flags, &target, &activate)) return op.a[11];
        flags = show_flags;
        after = 0;
        x = target.left; y = target.top; cx = target.right - target.left; cy = target.bottom - target.top;
        op.a[4] = flags; op.a[1] = after;
        op.a[2] = uint32_t(x) | uint64_t{uint32_t(y)} << 32;
        op.a[3] = uint32_t(cx) | uint64_t{uint32_t(cy)} << 32;
        op.a[5] = kNoShow;
        if (activate) flags |= 0;      // (activation happens with SWP_SHOWWINDOW below unless NOACTIVATE)
      }
      if (!(flags & wn::SWP_NOSENDCHANGING)) {
        const Rect cur = w->bounds;
        const int32_t px = (flags & wn::SWP_NOMOVE) ? cur.left : x, py = (flags & wn::SWP_NOMOVE) ? cur.top : y;
        const int32_t pw = (flags & wn::SWP_NOSIZE) ? cur.right - cur.left : cx, ph = (flags & wn::SWP_NOSIZE) ? cur.bottom - cur.top : cy;
        List<uint8_t> pos(S().alloc);
        make_windowpos(w, after, px, py, pw, ph, flags, pos);
        op.payload.clear();
        if (queue_payload_upcall(call, &op, w, wn::WM_WINDOWPOSCHANGING, 0, pos, 0xFFFF, 0, 0) || op.waiting_on) return kUpcallDue;
      }
    }
    if (op.phase == 1) {
      op.phase = 2;
      Window* w2 = find_window(uint32_t(op.a[0]));
      if (!w2) return 0;
      w = w2;
      // the procedure may have changed the proposed position
      if (op.payload.count() >= 48) {
        uint32_t new_after = after, new_flags = flags;
        int32_t nx = x, ny = y, ncx = cx, ncy = cy;
        const Rect cur = w->bounds;
        int32_t cur_cx = cur.right - cur.left, cur_cy = cur.bottom - cur.top;
        read_windowpos_payload(op.payload, &new_after, &nx, &ny, &ncx, &ncy, &new_flags);
        {      // back in device pixels: a value the procedure left as it was keeps its exact device value
          const uint32_t k = window_scale(w);
          const int32_t px = (flags & wn::SWP_NOMOVE) ? cur.left : x, py = (flags & wn::SWP_NOMOVE) ? cur.top : y;
          const int32_t pw = (flags & wn::SWP_NOSIZE) ? cur.right - cur.left : cx, ph = (flags & wn::SWP_NOSIZE) ? cur.bottom - cur.top : cy;
          auto back = [&](int32_t got, int32_t sent) { return got == dpi_down(sent, k) ? sent : dpi_up(got, k); };
          nx = back(nx, px);
          ny = back(ny, py);
          ncx = back(ncx, pw);
          ncy = back(ncy, ph);
        }
        if (flags & wn::SWP_NOMOVE) { nx = x; ny = y; }
        if (flags & wn::SWP_NOSIZE) { ncx = cx; ncy = cy; }
        (void)cur_cx; (void)cur_cy;
        x = nx; y = ny; cx = ncx; cy = ncy; after = new_after; flags = new_flags | (flags & wn::SWP_NOSENDCHANGING);
        op.a[4] = flags; op.a[1] = after;
        op.a[2] = uint32_t(x) | uint64_t{uint32_t(y)} << 32;
        op.a[3] = uint32_t(cx) | uint64_t{uint32_t(cy)} << 32;
      }
      op.payload.clear();
      // apply
      const Rect old_screen = window_rect_screen(w);
      const bool was_visible = (w->style & wn::WS_VISIBLE) != 0;
      const Rect old_bounds = w->bounds;
      Rect nb = old_bounds;
      if (!(flags & wn::SWP_NOMOVE)) { nb.right = int32_t(uint32_t(nb.right) - uint32_t(nb.left) + uint32_t(x)); nb.left = x; nb.bottom = int32_t(uint32_t(nb.bottom) - uint32_t(nb.top) + uint32_t(y)); nb.top = y; }
      if (!(flags & wn::SWP_NOSIZE)) {
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        nb.right = int32_t(uint32_t(nb.left) + uint32_t(cx));
        nb.bottom = int32_t(uint32_t(nb.top) + uint32_t(cy));
      }
      w->bounds = nb;
      if (!(flags & wn::SWP_NOZORDER)) apply_zorder(w, after);
      if (flags & wn::SWP_SHOWWINDOW) w->style |= wn::WS_VISIBLE;
      if (flags & wn::SWP_HIDEWINDOW) w->style &= ~wn::WS_VISIBLE;
      op.a[6] = 0;
      if (((flags & wn::SWP_SHOWWINDOW) && !was_visible) || ((flags & wn::SWP_HIDEWINDOW) && was_visible)) op.a[6] |= (flags & wn::SWP_SHOWWINDOW) ? 4 : 8;
      // the client rectangle: WM_NCCALCSIZE when the size or frame changed
      const bool size_changed = !rect_equal(nb, old_bounds) && (nb.right - nb.left != old_bounds.right - old_bounds.left || nb.bottom - nb.top != old_bounds.bottom - old_bounds.top);
      const bool moved = nb.left != old_bounds.left || nb.top != old_bounds.top;
      Rect client;
      compute_nc(w, &client);
      const int32_t old_cl_w = w->client.right - w->client.left, old_cl_h = w->client.bottom - w->client.top;
      if (size_changed || (flags & wn::SWP_FRAMECHANGED)) {
        w->client = client;
        (void)old_cl_w; (void)old_cl_h;
        if (w->wndproc != 0) {
          List<uint8_t> payload(S().alloc);
          uint8_t raw[104] = {};
          const uint32_t k = window_scale(w);      // (in the window's coordinates)
          auto wr = [&](uint32_t o, const Rect& r0) {
            const Rect r = dpi_down(r0, k);
            put32(raw, o, uint32_t(r.left)); put32(raw, o + 4, uint32_t(r.top)); put32(raw, o + 8, uint32_t(r.right)); put32(raw, o + 12, uint32_t(r.bottom));
          };
          wr(0, nb); wr(16, old_bounds);
          Rect old_client = rect_offset(w->client, 0, 0);
          wr(32, old_client);
          wr(0, nb);
          put64(raw, 48, 0);
          List<uint8_t> pos(S().alloc);
          make_windowpos(w, after, nb.left, nb.top, nb.right - nb.left, nb.bottom - nb.top, flags, pos);
          for (uint32_t i = 0; i < 56; ++i) payload.push_back(raw[i]);
          for (uint8_t b : pos) payload.push_back(b);
          if (queue_payload_upcall(call, &op, w, wn::WM_NCCALCSIZE, 1, payload, 48, 56, 0) || op.waiting_on) op.a[6] |= 2;      // (the result is read in phase 2)
        }
      } else if (moved) {
        // a move keeps the client size
        const int32_t dx = nb.left - old_bounds.left, dy = nb.top - old_bounds.top;
        w->client = rect_offset(w->client, dx, dy);
      }
      refresh_window_dcs(w);      // (device contexts held by the program follow the window)
      if (redirected(w)) sync_surface(w);      // (a redirected top-level's surface follows its size)
      if (Window* t = top_level_of(w); t && redirected(t)) mixed_tree_changed(t);      // (mixed DPI children follow the tree: painted again where they are)
      notify_top_level(w);
      // paint bookkeeping: what the window covered or now covers must be redrawn (a redirected top-level that only moved needs nothing: the desktop composites it)
      const Rect new_screen = window_rect_screen(w);
      const bool composited_move = redirected(w) && !size_changed && !(flags & wn::SWP_FRAMECHANGED) && was_visible == ((w->style & wn::WS_VISIBLE) != 0);
      if (!(flags & wn::SWP_NOREDRAW) && !composited_move) {
        if (was_visible && (!rect_equal(old_screen, new_screen) || (flags & wn::SWP_HIDEWINDOW))) invalidate_screen_rect(old_screen, w);
        if (flags & wn::SWP_HIDEWINDOW) { /* nothing more */ }
        else if ((w->style & wn::WS_VISIBLE) && (!was_visible || size_changed || (flags & wn::SWP_FRAMECHANGED))) {
          invalidate_window(w, nullptr, true);
          w->nc_paint_pending = true;
        }
        if ((w->style & wn::WS_VISIBLE) && !rect_equal(old_screen, new_screen) && was_visible) invalidate_screen_rect(new_screen, w);
      }
      if ((flags & wn::SWP_HIDEWINDOW) && was_visible) {
        // a hidden window gives up focus/activation
        if (Queue* q = find_queue(w->thread)) {
          InputState& in = input_of(*q);
          if (in.focus && (in.focus == w || is_child_of(w, in.focus))) set_focus_internal(*q, nullptr);
          if (in.capture == w) in.capture = nullptr;
        }
      }
      if (op.outstanding || op.waiting_on) return kUpcallDue;
    }
    if (op.phase == 2) {
      op.phase = 3;
      Window* w2 = find_window(uint32_t(op.a[0]));
      if (!w2) return 0;
      w = w2;
      if ((op.a[6] & 2) && op.payload.count() >= 16) {
        const uint8_t* p = op.payload.view().data();
        const Rect got{int32_t(get32(p, 0)), int32_t(get32(p, 4)), int32_t(get32(p, 8)), int32_t(get32(p, 12))};
        const uint32_t k = window_scale(w);
        if (!rect_equal(got, dpi_down(w->client, k))) w->client = dpi_up(got, k);      // (left as it was: the exact device rectangle stays)
      }
      op.payload.clear();
      const bool now_visible = (w->style & wn::WS_VISIBLE) != 0;
      // WM_SHOWWINDOW when the visibility changed through this call
      if (op.a[6] & 12) upcall_to_self(call, &op, w->handle, wn::WM_SHOWWINDOW, (op.a[6] & 4) ? 1 : 0, 0);
      List<uint8_t> pos(S().alloc);
      make_windowpos(w, after, w->bounds.left, w->bounds.top, w->bounds.right - w->bounds.left, w->bounds.bottom - w->bounds.top, flags, pos);
      queue_payload_upcall(call, &op, w, wn::WM_WINDOWPOSCHANGED, 0, pos, 0xFFFF, 0, 0);
      if (!(flags & wn::SWP_NOACTIVATE) && now_visible && is_top_level(w) && !(w->ex_style & wn::WS_EX_NOACTIVATE) && (flags & wn::SWP_SHOWWINDOW)) activate_window_internal(call, w, true);
      if (op.outstanding || op.waiting_on) return kUpcallDue;
    }
    // phase 3: next item or the answer
    uint64_t answer = 0;
    if (swp_next_item(op, &answer)) continue;
    return answer;
  }
}

}  // namespace

uint64_t swp_op_begin(Call& call, const SwpArgs& args, uint32_t show_cmd, bool want_visible_result, bool defer_hdwp) {
  if (args.w && args.w->parent && args.w->parent == args.w->desktop->message_root && !defer_hdwp) return 1;      // (a message-only window is not moved, restacked or told: the call succeeds)
  Op* op = op_begin(call, swp_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = args.w ? args.w->handle : 0;
  op->a[1] = args.after;
  op->a[2] = uint32_t(args.x) | uint64_t{uint32_t(args.y)} << 32;
  op->a[3] = uint32_t(args.cx) | uint64_t{uint32_t(args.cy)} << 32;
  op->a[4] = args.flags;
  op->a[5] = show_cmd;
  op->a[8] = (defer_hdwp ? 1 : 0) | (want_visible_result ? 2 : 0);
  return op_continue(call, *op);
}

namespace {

// BOOL NtUserSetWindowPos(HWND, HWND after, INT x, INT y, INT cx, INT cy, UINT flags)
uint64_t nt_set_window_pos(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (!w->parent) return call.fail(kErrorAccessDenied);
  if (w->destroy_stage == 2) return call.fail(kErrorInvalidParameter);
  const uint32_t flags = call.arg32(6);
  if (flags & ~0x7FFFu & ~0u) {
    if (flags & 0xFFFF8000u) return call.fail(kErrorInvalidParameter);
  }
  uint32_t after = call.arg32(1);
  if (!(flags & wn::SWP_NOZORDER)) {
    if (after != kHwndTop && after != kHwndBottom && after != kHwndTopmost && after != kHwndNotopmost) {
      Window* a = find_window(after);
      if (!a) return call.fail(kErrorInvalidWindowHandle);
      if (a->parent != w->parent && a != w) return call.fail(kErrorInvalidParameter);
    }
  }
  SwpArgs args;
  args.w = w;
  args.after = after;
  const uint32_t k = caller_scale_for(call, w);      // (DPI virtualization: an unaware caller's 96-dpi units)
  args.x = dpi_up(call.argi(2), k); args.y = dpi_up(call.argi(3), k); args.cx = dpi_up(call.argi(4), k); args.cy = dpi_up(call.argi(5), k);
  args.flags = flags;
  return swp_op_begin(call, args, kNoShow, false, false);
}

// BOOL NtUserMoveWindow(HWND, INT x, INT y, INT cx, INT cy, BOOL repaint)
uint64_t nt_move_window(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (!w->parent) return call.fail(kErrorAccessDenied);
  SwpArgs args;
  args.w = w;
  const uint32_t k = caller_scale_for(call, w);
  args.x = dpi_up(call.argi(1), k); args.y = dpi_up(call.argi(2), k); args.cx = dpi_up(call.argi(3), k); args.cy = dpi_up(call.argi(4), k);
  args.flags = wn::SWP_NOZORDER | wn::SWP_NOACTIVATE | (call.arg_bool(5) ? 0 : wn::SWP_NOREDRAW);
  return swp_op_begin(call, args, kNoShow, false, false);
}

// BOOL NtUserShowWindow(HWND, INT cmd): answers whether the window was visible
uint64_t nt_show_window(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t cmd = call.arg32(1);
  if (cmd > wn::SW_FORCEMINIMIZE) return call.fail(kErrorInvalidShowwinCommand);
  if (!w->parent) return call.fail(kErrorAccessDenied);
  SwpArgs args;
  args.w = w;
  return swp_op_begin(call, args, cmd, true, false);
}

// BOOL NtUserShowWindowAsync(HWND, INT cmd): queued to the window's thread; here the call is made on the caller's thread when the window is its own
uint64_t nt_show_window_async(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (w->thread == thread_of(call)) return nt_show_window(call);
  // (another thread: the request is posted to be done when that thread looks at its queue)
  return call.fail(kErrorNotSupported);
}

// HDWP NtUserBeginDeferWindowPos(INT count)
uint64_t nt_begin_defer(Call& call) {
  if (call.argi(0) < 0) return call.fail(kErrorInvalidParameter);
  State& s = S();
  DeferPos* d = s.defers.create(s.alloc);
  if (!d) return call.fail(kErrorNotEnoughMemory);
  d->thread = thread_of(call);
  return d->handle;
}

// HDWP NtUserDeferWindowPosAndBand(HDWP, HWND, HWND after, INT x, INT y, INT cx, INT cy, UINT flags, UINT unk1, UINT unk2)
uint64_t nt_defer_window_pos(Call& call) {
  State& s = S();
  DeferPos* d = s.defers.find(call.arg32(0));
  if (!d) return call.fail(kErrorInvalidDwpHandle);
  Window* w = window_arg(call, 1);
  if (!w) { s.defers.destroy(d); return 0; }
  const uint32_t flags = call.arg32(7);
  if (flags & 0xFFFF8000u) { s.defers.destroy(d); return call.fail(kErrorInvalidParameter); }
  // a second entry for the same window replaces the first
  DeferPos::Item item;
  item.hwnd = w->handle;
  item.after = call.arg32(2);
  const uint32_t k = caller_scale_for(call, w);
  item.x = dpi_up(call.argi(3), k); item.y = dpi_up(call.argi(4), k); item.cx = dpi_up(call.argi(5), k); item.cy = dpi_up(call.argi(6), k);
  item.flags = flags;
  for (DeferPos::Item& existing : d->items) if (existing.hwnd == item.hwnd) { existing = item; return d->handle; }
  if (!d->items.push_back(item)) return call.fail(kErrorNotEnoughMemory);
  return d->handle;
}

// BOOL NtUserEndDeferWindowPosEx(HDWP, BOOL async)
uint64_t nt_end_defer(Call& call) {
  State& s = S();
  DeferPos* d = s.defers.find(call.arg32(0));
  if (!d) return call.fail(kErrorInvalidDwpHandle);
  if (d->items.count() == 0) { s.defers.destroy(d); return 1; }
  Op* op = op_begin(call, swp_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[8] = 1;
  op->a[9] = 0;
  op->a[10] = d->handle;
  load_item(*op, 0);
  return op_continue(call, *op);
}

// ---- placement

// BOOL NtUserGetWindowPlacement(HWND, WINDOWPLACEMENT*): length 4, flags 4, showCmd 4, ptMin 8, ptMax 8, rcNormal 16 = 44 bytes
uint64_t nt_get_window_placement(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint32_t length = 0;
  if (!call.read_u32(call.arg(1), &length) || length != 44) return call.fail(kErrorInvalidParameter);
  const uint32_t k = caller_scale_for(call, w);
  uint8_t raw[44] = {};
  put32(raw, 0, 44);
  put32(raw, 4, w->placement.restore_maximized && (w->style & wn::WS_MINIMIZE) ? 2 : 0);      // WPF_RESTORETOMAXIMIZED
  put32(raw, 8, (w->style & wn::WS_MINIMIZE) ? wn::SW_SHOWMINIMIZED : (w->style & wn::WS_MAXIMIZE) ? wn::SW_SHOWMAXIMIZED : wn::SW_SHOWNORMAL);
  put32(raw, 12, uint32_t(dpi_down(w->placement.min_x, k)));
  put32(raw, 16, uint32_t(dpi_down(w->placement.min_y, k)));
  put32(raw, 20, uint32_t(dpi_down(w->placement.max_x, k)));
  put32(raw, 24, uint32_t(dpi_down(w->placement.max_y, k)));
  Rect normal = dpi_down((w->style & (wn::WS_MINIMIZE | wn::WS_MAXIMIZE)) && w->placement.has_normal ? w->placement.normal : w->bounds, k);
  put32(raw, 28, uint32_t(normal.left)); put32(raw, 32, uint32_t(normal.top)); put32(raw, 36, uint32_t(normal.right)); put32(raw, 40, uint32_t(normal.bottom));
  return call.write(call.arg(1), ConstByteView(raw, 44)) ? 1 : call.fail(kErrorInvalidParameter);
}

uint64_t nt_set_window_placement(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint8_t raw[44];
  if (!call.read(call.arg(1), ByteView(raw, 44)) || get32(raw, 0) != 44) return call.fail(kErrorInvalidParameter);
  const uint32_t flags = get32(raw, 4), cmd = get32(raw, 8);
  if (cmd > wn::SW_FORCEMINIMIZE) return call.fail(kErrorInvalidShowwinCommand);
  if (!w->parent) return call.fail(kErrorAccessDenied);
  const uint32_t k = caller_scale_for(call, w);
  if (flags & 1) { w->placement.min_x = dpi_up(int32_t(get32(raw, 12)), k); w->placement.min_y = dpi_up(int32_t(get32(raw, 16)), k); }
  w->placement.max_x = dpi_up(int32_t(get32(raw, 20)), k);
  w->placement.max_y = dpi_up(int32_t(get32(raw, 24)), k);
  const Rect normal = dpi_up(Rect{int32_t(get32(raw, 28)), int32_t(get32(raw, 32)), int32_t(get32(raw, 36)), int32_t(get32(raw, 40))}, k);
  w->placement.normal = normal;
  w->placement.has_normal = true;
  w->placement.restore_maximized = (flags & 2) != 0;
  SwpArgs args;
  args.w = w;
  if (cmd == wn::SW_SHOWNORMAL || cmd == wn::SW_SHOW || cmd == wn::SW_SHOWNA || cmd == wn::SW_SHOWNOACTIVATE || cmd == wn::SW_RESTORE || cmd == wn::SW_SHOWDEFAULT) {
    if (!(w->style & (wn::WS_MINIMIZE | wn::WS_MAXIMIZE))) {
      args.x = normal.left; args.y = normal.top; args.cx = normal.right - normal.left; args.cy = normal.bottom - normal.top;
      args.flags = wn::SWP_NOZORDER | wn::SWP_NOACTIVATE;
      return swp_op_begin(call, args, cmd, false, false) ? 1 : 0;
    }
  }
  return swp_op_begin(call, args, cmd, false, false) ? 1 : 0;
}

// UINT NtUserGetInternalWindowPos(HWND, RECT*, POINT*)
uint64_t nt_get_internal_window_pos(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t k = caller_scale_for(call, w);
  const Rect r = dpi_down((w->placement.has_normal) ? w->placement.normal : w->bounds, k);
  if (call.arg(1) != 0 && !write_rect(call, call.arg(1), r)) return call.fail(kErrorInvalidParameter);
  if (call.arg(2) != 0 && !write_point(call, call.arg(2), Point{dpi_down(w->placement.min_x, k), dpi_down(w->placement.min_y, k)})) return call.fail(kErrorInvalidParameter);
  return (w->style & wn::WS_MINIMIZE) ? wn::SW_SHOWMINIMIZED : (w->style & wn::WS_MAXIMIZE) ? wn::SW_SHOWMAXIMIZED : wn::SW_SHOWNORMAL;
}

// ---- hit testing services

Window* child_by_client_point(Window* parent, int32_t x, int32_t y, uint32_t flags) {
  const Rect cl = parent->client;
  if (!rect_contains(Rect{0, 0, cl.right - cl.left, cl.bottom - cl.top}, x, y)) return nullptr;
  for (Window* c = parent->first_child; c; c = c->next) {
    if ((flags & 1) && !(c->style & wn::WS_VISIBLE)) continue;
    if ((flags & 2) && (c->style & wn::WS_DISABLED)) continue;
    if ((flags & 4) && (c->ex_style & wn::WS_EX_TRANSPARENT)) continue;
    // the child's rectangle is in the parent's client coordinates plus the parent's client origin offset inside its own window rect
    const Rect r = rect_offset(c->bounds, -parent->client.left + parent->client.left, 0);
    const Rect in_client = rect_offset(r, -parent->client.left + 0, 0);
    (void)in_client;
    const Rect cr = c->bounds;
    // c->bounds is relative to the parent's client origin: the client point compares directly
    if (rect_contains(cr, x, y)) return c;
  }
  return parent;
}

// HWND NtUserChildWindowFromPointEx(HWND parent, LONG x, LONG y, UINT flags)
uint64_t nt_child_window_from_point_ex(Call& call) {
  Window* parent = window_arg(call, 0);
  if (!parent) return 0;
  const uint32_t k = caller_scale(call);
  Window* r = child_by_client_point(parent, dpi_up(call.argi(1), k), dpi_up(call.argi(2), k), call.arg32(3));
  return r ? r->handle : 0;
}
uint64_t nt_real_child_window_from_point(Call& call) {
  Window* parent = window_arg(call, 0);
  if (!parent) return 0;
  const uint32_t k = caller_scale(call);
  Window* r = child_by_client_point(parent, dpi_up(call.argi(1), k), dpi_up(call.argi(2), k), 0);
  return r ? r->handle : 0;
}
// HWND NtUserWindowFromPoint(LONG x, LONG y)
uint64_t nt_window_from_point(Call& call) {
  const uint32_t k = caller_scale(call);
  Window* w = window_at_point(dpi_up(call.argi(0), k), dpi_up(call.argi(1), k), true, thread_of(call));
  return w ? w->handle : 0;
}

// ---- window region

uint64_t nt_set_window_rgn(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t rgn = call.arg32(1);
  if (rgn != 0 && !gdi::is_region(rgn)) return call.fail(kErrorInvalidHandle);
  w->region = rgn;
  if (call.arg_bool(2) && (w->style & wn::WS_VISIBLE)) {
    invalidate_screen_rect(window_rect_screen(w), nullptr);
    invalidate_window(w, nullptr, true);
  }
  return 1;
}
uint64_t nt_get_window_rgn_ex(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (w->region == 0 || !gdi::is_region(w->region)) return 0;      // ERROR (the window has no region)
  List<Rect> rects(S().alloc);
  if (!gdi::region_rects(w->region, &rects)) return 0;
  if (!gdi::replace_region(call.arg32(1), rects.view())) return 0;
  return rects.count() <= 1 ? 2 : 3;
}

// ---- show owned popups, arrange: not here

}  // namespace

void register_geometry_services() {
  register_handler(WindowsSyscallNumber::NtUserSetWindowPos, nt_set_window_pos);
  register_handler(WindowsSyscallNumber::NtUserMoveWindow, nt_move_window);
  register_handler(WindowsSyscallNumber::NtUserShowWindow, nt_show_window);
  register_handler(WindowsSyscallNumber::NtUserShowWindowAsync, nt_show_window_async);
  register_handler(WindowsSyscallNumber::NtUserBeginDeferWindowPos, nt_begin_defer);
  register_handler(WindowsSyscallNumber::NtUserDeferWindowPosAndBand, nt_defer_window_pos);
  register_handler(WindowsSyscallNumber::NtUserEndDeferWindowPosEx, nt_end_defer);
  register_handler(WindowsSyscallNumber::NtUserGetWindowPlacement, nt_get_window_placement);
  register_handler(WindowsSyscallNumber::NtUserSetWindowPlacement, nt_set_window_placement);
  register_handler(WindowsSyscallNumber::NtUserGetInternalWindowPos, nt_get_internal_window_pos);
  register_handler(WindowsSyscallNumber::NtUserChildWindowFromPointEx, nt_child_window_from_point_ex);
  register_handler(WindowsSyscallNumber::NtUserRealChildWindowFromPoint, nt_real_child_window_from_point);
  register_handler(WindowsSyscallNumber::NtUserWindowFromPoint, nt_window_from_point);
  register_handler(WindowsSyscallNumber::NtUserSetWindowRgn, nt_set_window_rgn);
  register_handler(WindowsSyscallNumber::NtUserGetWindowRgnEx, nt_get_window_rgn_ex);
}

Rect window_maximized_rect(Window* w) { return maximized_rect(w); }

}  // namespace process::win32k::user
