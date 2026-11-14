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
// Window creation and destruction, the tree, styles and longs, text, properties, and the NtUserCallHwnd / NtUserCallHwndParam multiplexers.
#include "Process/Win32k/Win32kUserInternal.hpp"
#include "Process/Win32k/Win32kUserMisc.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }
constexpr uint32_t kCwUseDefault = 0x80000000u;
constexpr uint64_t kErrorTlwWithWsChild = 1406;

void put32(uint8_t* raw, uint32_t o, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) raw[o + i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* raw, uint32_t o, uint64_t v) { put32(raw, o, uint32_t(v)); put32(raw, o + 4, uint32_t(v >> 32)); }
uint32_t get32(const uint8_t* raw, uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; }

// The extended style the frame flags imply (WS_EX_WINDOWEDGE).
uint32_t fix_ex_style(uint32_t style, uint32_t ex) {
  if ((ex & wn::WS_EX_DLGMODALFRAME) || (!(ex & wn::WS_EX_STATICEDGE) && (style & (wn::WS_DLGFRAME | wn::WS_THICKFRAME)))) return ex | wn::WS_EX_WINDOWEDGE;
  return ex & ~wn::WS_EX_WINDOWEDGE;
}

void fill_create_struct(Window* w, const Op& op, List<uint8_t>& out) {
  uint8_t raw[80] = {};
  put64(raw, 0, op.a[1]);
  put64(raw, 8, w->instance);
  put64(raw, 16, (w->style & wn::WS_CHILD) ? w->id : w->menu);
  put64(raw, 24, w->parent && w->parent->parent ? uint64_t(w->parent->handle) : 0);
  const Rect b = dpi_down(w->bounds, window_scale(w));      // (in the window's coordinates)
  put32(raw, 32, uint32_t(b.bottom - b.top));
  put32(raw, 36, uint32_t(b.right - b.left));
  put32(raw, 40, uint32_t(b.top));
  put32(raw, 44, uint32_t(b.left));
  put32(raw, 48, w->cs_style);
  put32(raw, 72, w->cs_ex_style);
  out.clear();
  for (uint8_t b : raw) out.push_back(b);
}

void fill_windowpos(const Window* w, uint32_t after, uint32_t flags, List<uint8_t>& out) {
  uint8_t raw[48] = {};
  put64(raw, 0, w->handle);
  put64(raw, 8, after);
  const Rect b = dpi_down(w->bounds, window_scale(w));
  put32(raw, 16, uint32_t(b.left));
  put32(raw, 20, uint32_t(b.top));
  put32(raw, 24, uint32_t(b.right - b.left));
  put32(raw, 28, uint32_t(b.bottom - b.top));
  put32(raw, 32, flags);
  out.clear();
  for (uint8_t b : raw) out.push_back(b);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// CreateWindowEx
//
// a[0] = hwnd, a[1] = lpCreateParams, a[2] = parent handle for WM_PARENTNOTIFY
// phases: 0 WM_NCCREATE, 1 WM_NCCALCSIZE, 2 WM_CREATE, 3 notify the parent, 4 show, 5 done; 10 failed after NCCREATE (WM_NCDESTROY), 11 failed in WM_CREATE (WM_DESTROY, WM_NCDESTROY)

uint64_t create_step(Call& call, Op& op) {
  Window* w = find_window(uint32_t(op.a[0]));
  if (!w) return 0;      // destroyed by one of its own messages
  const bool has_proc = w->wndproc != 0;
  switch (op.phase) {
    case 20: {      // an overlapped or sizing window is asked for its limits before WM_NCCREATE
      op.phase = 21;
      if (!has_proc) return create_step(call, op);
      const uint32_t k = window_scale(w);      // (in the window's coordinates; an aware window's metrics at the system's dpi)
      const uint32_t dpi = window_working_dpi(w);
      const Rect max = dpi_down(window_maximized_rect(w), k);
      gdi::Rect none{0, 0, 0, 0};
      adjust_window_rect(&none, w->style, w->ex_style, false, 0);      // (the frame, as negative left and top)
      const bool plain = (w->style & (wn::WS_DLGFRAME | wn::WS_BORDER)) != 0;
      uint8_t raw[40] = {};
      put32(raw, 8, uint32_t(max.right - max.left)); put32(raw, 12, uint32_t(max.bottom - max.top));      // ptMaxSize
      put32(raw, 16, uint32_t(max.left)); put32(raw, 20, uint32_t(max.top));      // ptMaxPosition
      put32(raw, 24, plain ? uint32_t(user_misc::system_metrics(34, dpi)) : uint32_t(-2 * none.left));      // ptMinTrackSize
      put32(raw, 28, plain ? uint32_t(user_misc::system_metrics(35, dpi)) : uint32_t(-2 * none.top));
      put32(raw, 32, uint32_t(user_misc::system_metrics(59, dpi))); put32(raw, 36, uint32_t(user_misc::system_metrics(60, dpi)));      // ptMaxTrackSize
      List<uint8_t> payload(S().alloc);
      for (uint8_t v : raw) payload.push_back(v);
      if (!queue_payload_upcall(call, &op, w, wn::WM_GETMINMAXINFO, 0, payload, 0xFFFF, 0, 0)) { op.payload.clear(); return create_step(call, op); }
      return kUpcallDue;
    }
    case 21: {      // the window is made no smaller and no larger than the limits it gave
      op.phase = 0;
      if (op.payload.count() >= 40) {
        const uint8_t* p = op.payload.view().data();
        int32_t cx = w->bounds.right - w->bounds.left, cy = w->bounds.bottom - w->bounds.top;
        const uint32_t k = window_scale(w);
        const int32_t min_x = dpi_up(int32_t(get32(p, 24)), k), min_y = dpi_up(int32_t(get32(p, 28)), k);
        const int32_t max_x = dpi_up(int32_t(get32(p, 32)), k), max_y = dpi_up(int32_t(get32(p, 36)), k);
        if (max_x < cx) cx = max_x;
        if (max_y < cy) cy = max_y;
        if (cx < min_x) cx = min_x;
        if (cy < min_y) cy = min_y;
        w->bounds.right = w->bounds.left + cx;
        w->bounds.bottom = w->bounds.top + cy;
        Rect client;
        compute_nc(w, &client);
        w->client = client;
      }
      return create_step(call, op);
    }
    case 0: {
      op.phase = 1;
      if (!has_proc) return create_step(call, op);
      List<uint8_t> cs(S().alloc);
      fill_create_struct(w, op, cs);
      w->creating_stage = 1;
      if (!queue_payload_upcall(call, &op, w, wn::WM_NCCREATE, 0, cs, 0xFFFF, 0, UPF_CREATESTRUCT)) { op.first_result = 1; return create_step(call, op); }
      return kUpcallDue;
    }
    case 1: {
      if (has_proc && op.first_result == 0) {
        op.phase = 10;
        if (Upcall* u = upcall_to_self(call, &op, w->handle, wn::WM_NCDESTROY, 0, 0)) { (void)u; return kUpcallDue; }
        return create_step(call, op);
      }
      op.phase = 2;
      // the default client rectangle; WM_NCCALCSIZE may change it
      Rect client;
      compute_nc(w, &client);
      w->client = client;
      if (!has_proc) return create_step(call, op);
      uint8_t raw[104] = {};
      const uint32_t k = window_scale(w);      // (in the window's coordinates)
      auto wr = [&](uint32_t o, const Rect& r0) {
        const Rect r = dpi_down(r0, k);
        put32(raw, o, uint32_t(r.left)); put32(raw, o + 4, uint32_t(r.top)); put32(raw, o + 8, uint32_t(r.right)); put32(raw, o + 12, uint32_t(r.bottom));
      };
      wr(0, w->bounds); wr(16, w->bounds); wr(32, client);      // rgrc[0] proposed window rect; the procedure answers the client rect there
      put64(raw, 48, 0);
      List<uint8_t> payload(S().alloc);
      List<uint8_t> pos(S().alloc);
      fill_windowpos(w, 0, 0, pos);
      for (uint32_t i = 0; i < 56; ++i) payload.push_back(raw[i]);
      for (uint8_t b : pos) payload.push_back(b);
      // (the first rectangle carries the window rectangle in, the client rectangle out)
      for (uint32_t i = 0; i < 16; ++i) payload[i] = uint8_t(0);
      wr(0, w->bounds);
      for (uint32_t i = 0; i < 16; ++i) payload[i] = raw[i];
      if (!queue_payload_upcall(call, &op, w, wn::WM_NCCALCSIZE, 1, payload, 48, 56, 0)) return create_step(call, op);
      return kUpcallDue;
    }
    case 2: {
      op.phase = 3;
      if (has_proc && op.payload.count() >= 16) {
        const uint8_t* p = op.payload.view().data();
        Rect r{int32_t(get32(p, 0)), int32_t(get32(p, 4)), int32_t(get32(p, 8)), int32_t(get32(p, 12))};
        const uint32_t k = window_scale(w);
        if (!rect_equal(r, dpi_down(w->client, k))) w->client = dpi_up(r, k);      // (left as it was: the exact device rectangle stays)
      }
      if (!has_proc) return create_step(call, op);
      List<uint8_t> cs(S().alloc);
      fill_create_struct(w, op, cs);
      w->creating_stage = 2;
      if (!queue_payload_upcall(call, &op, w, wn::WM_CREATE, 0, cs, 0xFFFF, 0, UPF_CREATESTRUCT)) { op.first_result = 0; return create_step(call, op); }
      return kUpcallDue;
    }
    case 3: {
      w->creating_stage = 0;
      if (has_proc && int32_t(uint32_t(op.first_result)) == -1) {
        op.phase = 11;
        w->destroy_stage = 1;
        if (upcall_to_self(call, &op, w->handle, wn::WM_DESTROY, 0, 0)) return kUpcallDue;
        return create_step(call, op);
      }
      op.phase = 4;
      if ((w->style & wn::WS_CHILD) && !(w->ex_style & wn::WS_EX_NOPARENTNOTIFY) && w->parent && w->parent->parent) {
        Upcall* u = send_proc_upcall(call, &op, w->parent, wn::WM_PARENTNOTIFY, uint64_t{wn::WM_CREATE} | (w->id << 16), w->handle);
        if (u || op.waiting_on) return kUpcallDue;
      }
      return create_step(call, op);
    }
    case 4: {
      op.phase = 5;
      if (w->style & wn::WS_VISIBLE) {
        // the window shows: WM_SHOWWINDOW, then the position change messages of the show, then the first paint is due
        upcall_to_self(call, &op, w->handle, wn::WM_SHOWWINDOW, 1, 0);
        List<uint8_t> pos(S().alloc);
        fill_windowpos(w, 0, wn::SWP_SHOWWINDOW | wn::SWP_NOMOVE | wn::SWP_NOSIZE | wn::SWP_NOZORDER | wn::SWP_NOACTIVATE, pos);
        queue_payload_upcall(call, &op, w, wn::WM_WINDOWPOSCHANGING, 0, pos, 0xFFFF, 0, 0);
        fill_windowpos(w, 0, wn::SWP_SHOWWINDOW | wn::SWP_NOMOVE | wn::SWP_NOSIZE | wn::SWP_NOZORDER | wn::SWP_NOACTIVATE, pos);
        queue_payload_upcall(call, &op, w, wn::WM_WINDOWPOSCHANGED, 0, pos, 0xFFFF, 0, 0);
        invalidate_window(w, nullptr, true);
        w->nc_paint_pending = true;
        if (redirected(w)) sync_surface(w);
        notify_top_level(w);
        if (is_top_level(w) && !(w->ex_style & wn::WS_EX_NOACTIVATE)) activate_window_internal(call, w, true);
        return kUpcallDue;
      }
      return create_step(call, op);
    }
    case 5:
      return w->handle;
    case 10: {
      destroy_window_tree(w);
      return 0;
    }
    case 11: {
      op.phase = 10;
      w->destroy_stage = 2;
      if (upcall_to_self(call, &op, w->handle, wn::WM_NCDESTROY, 0, 0)) return kUpcallDue;
      return create_step(call, op);
    }
    default:
      return 0;
  }
}

// HWND NtUserCreateWindowEx(DWORD ex_style, UNICODE_STRING* class_name, UNICODE_STRING* version, UNICODE_STRING* window_name, DWORD style, INT x, INT y, INT cx, INT cy, HWND parent,
//                          HMENU menu, HINSTANCE instance, void* params, DWORD flags, HINSTANCE client_instance, const WCHAR* class, BOOL ansi)
uint64_t nt_create_window_ex(Call& call) {
  State& s = S();
  Queue* q = queue_of(call, true);
  Desktop* desktop = thread_desktop(call);
  if (!q || !desktop || !desktop->root) return call.fail(kErrorNotEnoughMemory);
  WideString name(s.alloc), title(s.alloc);
  uint32_t atom = 0;
  if (!read_class_name(call, call.arg(1), &name, &atom)) return call.fail(kErrorCannotFindWndClass);
  if (call.arg(3) != 0) {
    uint16_t length = 0;
    uint64_t buffer = 0;
    if (!call.read_u16(call.arg(3), &length) || !call.read_u64(call.arg(3) + 8, &buffer)) return call.fail(kErrorInvalidParameter);
    if (length != 0 && !call.read_wide(buffer, length / 2, &title)) return call.fail(kErrorInvalidParameter);
  }
  Class* cls = find_class(call, atom, name.view(), call.arg(11), true);
  if (!cls) cls = find_class(call, atom, name.view(), call.arg(14), true);
  if (!cls) return call.fail(kErrorCannotFindWndClass);
  uint32_t style = call.arg32(4), ex_style = call.arg32(0);
  Window* parent = nullptr;
  Window* owner = nullptr;
  const uint64_t parent_arg = call.arg(9);
  if (parent_arg == uint64_t(int64_t(-3)) || uint32_t(parent_arg) == kHwndMessage) parent = desktop->message_root;
  else if (parent_arg != 0) {
    parent = find_window(uint32_t(parent_arg));
    if (!parent) return call.fail(kErrorInvalidWindowHandle);
    if (parent->destroy_stage == 2) return call.fail(kErrorInvalidParameter);      // (a window past WM_NCDESTROY takes no new children)
  }
  const bool as_child = (style & wn::WS_CHILD) && !(style & wn::WS_POPUP);      // (WS_POPUP wins over WS_CHILD: it is owned, not a child)
  if (as_child && !parent) return call.fail(uint32_t(kErrorTlwWithWsChild));
  if (!as_child && parent && parent != desktop->message_root) {
    owner = parent;
    auto pure_child = [](const Window* x) { return (x->style & (wn::WS_CHILD | wn::WS_POPUP)) == wn::WS_CHILD; };      // (WS_POPUP makes a window an owner even with WS_CHILD)
    while (owner->parent && owner->parent->parent && pure_child(owner)) owner = owner->parent;      // the owner is a top-level window
    if (pure_child(owner)) owner = nullptr;
    if (owner == desktop->root) owner = nullptr;      // (owned by the desktop: not owned)
    parent = desktop->root;
  }
  if (!parent) parent = desktop->root;
  Window* w = s.windows.create(s.alloc);
  if (!w) return call.fail(kErrorNotEnoughMemory);
  w->cls = cls;
  ++cls->windows;
  w->desktop = desktop;
  w->thread = q->thread;
  w->pid = q->pid;
  w->cs_style = style;
  w->cs_ex_style = fix_ex_style(style, ex_style);
  // the styles the window gets: an overlapped window has a caption and the raised edge; all but a pure child clip their siblings
  const bool overlapped = !(style & (wn::WS_CHILD | wn::WS_POPUP));
  if (overlapped) style |= wn::WS_CAPTION;
  if ((style & (wn::WS_CHILD | wn::WS_POPUP)) != wn::WS_CHILD) style |= wn::WS_CLIPSIBLINGS;
  w->style = style;
  w->ex_style = fix_ex_style(style, ex_style);
  if (overlapped) w->ex_style |= wn::WS_EX_WINDOWEDGE;
  w->wndproc = cls->wndproc;
  w->unicode = !call.arg_bool(16);
  w->instance = call.arg(11);
  w->owner = owner;
  w->dpi_context = user_misc::dpi_context_of(call.tid(), call.pid());
  w->dpi_virtual = thread_unaware(call) ? 1 : 0;      // (DPI virtualization: the creating thread's awareness, as on Windows)
  w->dpi_hosting = uint8_t(user_misc::dpi_hosting(call.tid()));
  if (as_child && parent && parent->parent) adopt_tree_awareness(w, parent, w->dpi_hosting == 1);      // (a child: the tree's awareness, unless mixed)
  if (style & wn::WS_CHILD) w->id = call.arg(10); else w->menu = call.arg(10);
  if (!(style & wn::WS_CHILD) && w->menu != 0 && !user_misc::attach_window_menu(w->handle, uint32_t(w->menu))) w->menu = 0;      // (not a menu: no menu)
  window_text_set(w, title.view());
  for (int32_t i = 0; i < cls->cb_wnd_extra; ++i) w->extra.push_back(0);
  // geometry; CW_USEDEFAULT picks a default for overlapped windows
  int32_t x = call.argi(5), y = call.argi(6), cx = call.argi(7), cy = call.argi(8);
  const int32_t sw = int32_t(gdi::screen_width()), sh = int32_t(gdi::screen_height());
  if (uint32_t(x) == kCwUseDefault) { x = (style & wn::WS_CHILD) ? 0 : 0; if (uint32_t(y) == kCwUseDefault) y = 0; }
  if (uint32_t(y) == kCwUseDefault) y = 0;
  if (uint32_t(cx) == kCwUseDefault) { cx = (style & wn::WS_CHILD) ? 0 : (sw * 3) / 4 - x; if (uint32_t(cy) == kCwUseDefault) cy = (style & wn::WS_CHILD) ? 0 : (sh * 3) / 4 - y; }
  if (uint32_t(cy) == kCwUseDefault) cy = (style & wn::WS_CHILD) ? 0 : (sh * 3) / 4 - y;
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  {      // an unaware caller's 96-dpi units into device pixels (the defaults above are device pixels already)
    const uint32_t k = caller_scale(call);
    if (uint32_t(call.argi(5)) != kCwUseDefault) x = dpi_up(x, k);
    if (uint32_t(call.argi(6)) != kCwUseDefault && uint32_t(call.argi(5)) != kCwUseDefault) y = dpi_up(y, k);
    if (uint32_t(call.argi(7)) != kCwUseDefault) cx = dpi_up(cx, k);
    if (uint32_t(call.argi(8)) != kCwUseDefault && uint32_t(call.argi(7)) != kCwUseDefault) cy = dpi_up(cy, k);
  }
  // monitors at different scales: a top-level takes the scale of the monitor it lands on; made in its program's coordinates at another scale, it is
  // sized at that monitor's so that it keeps the size the program asked for (as when it moves there later)
  if (parent->parent == nullptr) {
    const uint32_t kc = caller_scale(call);
    const uint32_t km = user_misc::monitor_scale_at(int32_t(int64_t{x} + cx / 2), int32_t(int64_t{y} + cy / 2));
    if (km != 0) {
      w->monitor_scale = static_cast<uint16_t>(km == device_scale() ? 0 : km);
      const uint32_t kw = window_scale(w);      // (what its coordinates are on that monitor)
      if (kw != kc && kc != 0) {
        cx = int32_t(int64_t{cx} * kw / kc);
        cy = int32_t(int64_t{cy} * kw / kc);
      }
    }
  }
  w->bounds = Rect{x, y, int32_t(uint32_t(x) + uint32_t(cx)), int32_t(uint32_t(y) + uint32_t(cy))};
  // z-order: the top of its level (below the topmost windows unless it is one)
  if (parent->parent == nullptr || (w->ex_style & wn::WS_EX_TOPMOST) || true) {
    Window* after = nullptr;
    if (!(w->ex_style & wn::WS_EX_TOPMOST) && !as_child) {
      for (Window* c = parent->first_child; c && (c->ex_style & wn::WS_EX_TOPMOST); c = c->next) after = c;
    } else if (as_child && !(style & wn::WS_MAXIMIZE)) {      // a new child goes below its siblings (the dialog order is the creation order); a maximized one goes on top
      for (Window* c = parent->first_child; c; c = c->next) after = c;
    }
    link_window(w, parent, after, false);
  }
  Rect client;
  compute_nc(w, &client);
  w->client = client;
  Op* op = op_begin(call, create_step);
  if (!op) { destroy_window_tree(w); return call.fail(kErrorNotEnoughMemory); }
  op->a[0] = w->handle;
  op->a[1] = call.arg(12);
  if ((style & wn::WS_THICKFRAME) || !(style & (wn::WS_POPUP | wn::WS_CHILD))) op->phase = 20;
  return op_continue(call, *op);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// DestroyWindow

// pre-order: the first window of the tree (children, then owned windows) whose WM_DESTROY has not been sent
Window* next_without_destroy(Window* w) {
  if (w->destroy_stage == 0) return w;
  for (Window* c = w->first_child; c; c = c->next) if (Window* r = next_without_destroy(c)) return r;
  for (Window& o : S().windows.items()) if (o.owner == w && o.destroy_stage != 0 ? false : o.owner == w) if (Window* r = next_without_destroy(&o)) return r;
  return nullptr;
}
// post-order: the first window of the tree whose WM_NCDESTROY has not been sent (children first)
Window* next_without_ncdestroy(Window* w) {
  for (Window* c = w->first_child; c; c = c->next) if (Window* r = next_without_ncdestroy(c)) return r;
  for (Window& o : S().windows.items()) if (o.owner == w) if (Window* r = next_without_ncdestroy(&o)) return r;
  return w->destroy_stage < 2 ? w : nullptr;
}
Window* find_ncdestroyed(Window* w) {
  for (Window* c = w->first_child; c; c = c->next) if (Window* r = find_ncdestroyed(c)) return r;
  for (Window& o : S().windows.items()) if (o.owner == w) if (Window* r = find_ncdestroyed(&o)) return r;
  return w->destroy_stage == 2 ? w : nullptr;
}

// a[0] = root hwnd; phases: 0 start, 1 WM_DESTROY loop, 2 WM_NCDESTROY loop
uint64_t destroy_step(Call& call, Op& op) {
  Window* root = find_window(uint32_t(op.a[0]));
  if (!root) return 1;
  if (op.phase == 0) {
    op.phase = 1;
    const bool was_visible = (root->style & wn::WS_VISIBLE) != 0;
    // hide, then move focus and activation away from the window
    if (was_visible) {
      root->style &= ~wn::WS_VISIBLE;
      if (root->parent) {
        Rect r = window_rect_screen(root);
        (void)r;
        paint_region_changed(root->parent);
      }
    }
    queue_destroy_focus_changes(call, &op, root);
    if (op.outstanding) return kUpcallDue;
  }
  if (op.phase == 1) {
    // the root's own parent hears about the end of a child first
    if (Window* next = next_without_destroy(root)) {
      next->destroy_stage = 1;
      if (next == root && (root->style & wn::WS_CHILD) && !(root->ex_style & wn::WS_EX_NOPARENTNOTIFY) && root->parent && root->parent->parent) {
        // (sent after WM_DESTROY below)
      }
      Upcall* u = send_proc_upcall(call, &op, next, wn::WM_DESTROY, 0, 0);
      if (u || op.waiting_on || op.outstanding) return kUpcallDue;
      return destroy_step(call, op);
    }
    op.phase = 2;
  }
  // phase 2
  if (Window* gone = find_ncdestroyed(root)) {
    const bool is_root = gone == root;
    destroy_window_now(gone);
    if (is_root) return 1;
    return destroy_step(call, op);
  }
  Window* next = next_without_ncdestroy(root);
  if (!next) return 1;
  next->destroy_stage = 2;
  Upcall* u = send_proc_upcall(call, &op, next, wn::WM_NCDESTROY, 0, 0);
  if (u || op.waiting_on || op.outstanding) return kUpcallDue;
  return destroy_step(call, op);
}

// BOOL NtUserDestroyWindow(HWND)
uint64_t nt_destroy_window(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  Queue* q = queue_of(call, true);
  if (!w->parent || w->desktop->root == w) return call.fail(kErrorAccessDenied);
  if (w->thread != q->thread) return call.fail(kErrorAccessDenied);
  if (w->destroying || w->destroy_stage) return call.fail(kErrorInvalidWindowHandle);
  Op* op = op_begin(call, destroy_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  return op_continue(call, *op);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// SetParent

uint64_t set_parent_step(Call& call, Op& op) {
  Window* w = find_window(uint32_t(op.a[0]));
  if (!w) return 0;
  if (op.phase == 0) {
    op.phase = 1;
    Window* new_parent = find_window(uint32_t(op.a[1]));
    if (!new_parent) return 0;
    Window* old_parent = w->parent;
    op.a[2] = old_parent ? old_parent->handle : 0;
    const bool visible = (w->style & wn::WS_VISIBLE) != 0;
    if (visible) paint_region_changed(old_parent);
    unlink_window(w);
    link_window(w, new_parent, nullptr, false);
    if (w->desktop != new_parent->desktop) w->desktop = new_parent->desktop;
    if ((w->style & wn::WS_CHILD) && !(w->ex_style & wn::WS_EX_NOPARENTNOTIFY)) {
      if (old_parent && old_parent->parent) send_proc_upcall(call, &op, old_parent, wn::WM_PARENTNOTIFY, uint64_t{wn::WM_DESTROY} | (w->id << 16), w->handle);
    }
    if (visible) invalidate_window(w, nullptr, true);
    if (op.outstanding || op.waiting_on) return kUpcallDue;
  }
  if (op.phase == 1) {
    op.phase = 2;
    Window* w2 = find_window(uint32_t(op.a[0]));
    if (w2 && (w2->style & wn::WS_CHILD) && !(w2->ex_style & wn::WS_EX_NOPARENTNOTIFY) && w2->parent && w2->parent->parent) {
      send_proc_upcall(call, &op, w2->parent, wn::WM_PARENTNOTIFY, uint64_t{wn::WM_CREATE} | (w2->id << 16), w2->handle);
      if (op.outstanding || op.waiting_on) return kUpcallDue;
    }
  }
  return op.a[2];
}

// HWND NtUserSetParent(HWND, HWND)
uint64_t nt_set_parent(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  Desktop* desktop = w->desktop;
  if (w == desktop->root || w == desktop->message_root || !w->parent) return call.fail(kErrorAccessDenied);
  if (w->destroy_stage == 2) return call.fail(kErrorInvalidParameter);      // (past WM_NCDESTROY the window is not put back into a tree)
  Window* parent = nullptr;
  const uint64_t p = call.arg(1);
  if (p == 0) parent = desktop->root;
  else if (uint32_t(p) == kHwndMessage) parent = desktop->message_root;
  else {
    parent = find_window(uint32_t(p));
    if (!parent) return call.fail(kErrorInvalidWindowHandle);
  }
  // (no cycles: the new parent must not be the window, one of its descendants, or a window owned by one of them)
  for (const Window* x = parent; x; x = x->owner ? x->owner : x->parent) {
    if (x == w) return call.fail(kErrorInvalidParameter);
    if (!x->owner && !x->parent) break;
  }
  if (parent == w || is_child_of(w, parent)) return call.fail(kErrorInvalidParameter);
  // (DPI hosting: a child of another awareness than the tree it goes into only with mixed hosting; a top-level again keeps its own)
  if ((w->style & wn::WS_CHILD) && parent->parent) {
    if (!adopt_tree_awareness(w, parent, w->dpi_hosting == 1 || user_misc::dpi_hosting(call.tid()) == 1)) return call.fail(4317 /* ERROR_INVALID_OPERATION */);
  } else if (w->dpi_mixed) {
    drop_mixed_surface(w);
    w->dpi_mixed = false;
  }
  Queue* q = queue_of(call, true);
  (void)q;
  Op* op = op_begin(call, set_parent_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  op->a[1] = parent->handle;
  return op_continue(call, *op);
}

// HWND NtUserGetAncestor(HWND, UINT type)
uint64_t nt_get_ancestor(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t type = call.arg32(1);
  if (w->desktop->root == w) return 0;
  switch (type) {
    case wn::GA_PARENT: return w->parent ? w->parent->handle : 0;      // (a top-level window's parent is the desktop)
    case wn::GA_ROOT: {
      Window* r = top_level_of(w);
      return r && r->parent ? r->handle : 0;
    }
    case wn::GA_ROOTOWNER: {      // up through GetParent (the owner of a popup, the parent of a child) as long as there is one
      Window* r = w;
      for (int guard = 0; guard < 64; ++guard) {
        Window* up = nullptr;
        if (r->style & wn::WS_POPUP) up = r->owner;
        else if (r->style & wn::WS_CHILD) up = r->parent;
        if (!up) break;
        r = up;
      }
      return r->handle;
    }
    default: return call.fail(kErrorInvalidParameter);
  }
}

// HWND NtUserFindWindowEx(HWND parent, HWND child, UNICODE_STRING* class, UNICODE_STRING* title, ULONG unk)
uint64_t nt_find_window_ex(Call& call) {
  State& s = S();
  Desktop* desktop = thread_desktop(call);
  Window* parent = desktop->root;
  if (call.arg(0) != 0) {
    if (uint32_t(call.arg(0)) == kHwndMessage) parent = desktop->message_root;
    else { parent = window_arg(call, 0); if (!parent) return 0; }
  }
  Window* child = nullptr;
  if (call.arg(1) != 0) {
    child = window_arg(call, 1);
    if (!child) return 0;
    if (child->parent != parent) return call.fail(kErrorInvalidWindowHandle);
  }
  uint32_t atom = 0;
  WideString name(s.alloc);
  const bool have_class = call.arg(2) != 0;
  if (have_class) {
    if (!read_class_name(call, call.arg(2), &name, &atom)) return call.fail(kErrorCannotFindWndClass);
    if (atom == 0) atom = atom_find(name.view());
    if (atom == 0) return call.fail(kErrorCannotFindWndClass);
  }
  WideString title(s.alloc);
  bool have_title = false;
  if (call.arg(3) != 0) {
    have_title = true;
    uint16_t length = 0;
    uint64_t buffer = 0;
    if (!call.read_u16(call.arg(3), &length) || !call.read_u64(call.arg(3) + 8, &buffer)) return call.fail(kErrorInvalidParameter);
    if (length != 0 && !call.read_wide(buffer, length / 2, &title)) return call.fail(kErrorInvalidParameter);
  }
  for (Window* c = child ? child->next : parent->first_child; c; c = c->next) {
    if (have_class && (!c->cls || c->cls->atom != atom)) continue;
    if (have_title && !c->text.equals(title.view(), true)) continue;
    return c->handle;
  }
  return 0;
}

// NTSTATUS NtUserBuildHwndList(HDESK, HWND parent, BOOL children, BOOL non_immersive, ULONG thread_id, ULONG count, HWND* buffer, ULONG* size)
void collect_descendants(Window* w, bool recursive, uint32_t thread, List<uint32_t>& out) {
  for (Window* c = w->first_child; c; c = c->next) {
    if (c == c->desktop->message_root) continue;
    if (thread == 0 || c->thread == thread) out.push_back(c->handle);
    if (recursive) collect_descendants(c, true, thread, out);
  }
}

uint64_t nt_build_hwnd_list(Call& call) {
  State& s = S();
  Desktop* desktop = thread_desktop(call);
  if (call.arg(0) != 0) {
    ObjHandle* h = s.object_handles.find(call.arg32(0));
    if (!h || !h->desktop) return kStatusInvalidHandle;
    desktop = h->desktop;
  }
  Window* parent = desktop->root;
  const bool children = call.arg_bool(2);
  if (call.arg(1) != 0) {
    parent = find_window(call.arg32(1));
    if (!parent) return kStatusInvalidWindowHandle;
  }
  List<uint32_t> list(s.alloc);
  const uint32_t thread = uint32_t(call.arg(4));
  // thread_id is a thread id: compare on the thread this window belongs to
  List<uint32_t> raw(s.alloc);
  collect_descendants(parent, call.arg(1) != 0 && children, 0, raw);
  for (uint32_t h : raw) {
    Window* c = find_window(h);
    if (c && (thread == 0 || uint32_t(c->thread) == thread)) list.push_back(h);
  }
  const uint32_t count = call.arg32(5);
  if (call.arg(7) != 0 && !call.write_u32(call.arg(7), uint32_t(list.count()))) return kStatusAccessViolation;
  if (list.count() > count) return kStatusBufferTooSmall;
  for (uint64_t i = 0; i < list.count(); ++i) if (!call.write_u64(call.arg(6) + 8 * i, list[i])) return kStatusAccessViolation;
  return kStatusSuccess;
}

// BOOL NtUserEnableWindow(HWND, BOOL enable): answers whether the window was disabled
uint64_t enable_step(Call& call, Op& op) {
  if (op.phase == 0) {
    op.phase = 1;
    Window* w = find_window(uint32_t(op.a[0]));
    if (w) {
      if (!op.a[1]) {
        // a disabled window loses focus and capture
        if (Queue* q = find_queue(w->thread)) {
          InputState& in = input_of(*q);
          if (in.capture == w) in.capture = nullptr;
          if (in.focus == w) set_focus_internal(*q, nullptr);
        }
      }
      send_proc_upcall(call, &op, w, wn::WM_ENABLE, op.a[1] ? 1 : 0, 0);
      if (op.outstanding || op.waiting_on) return kUpcallDue;
    }
  }
  return op.a[2];
}
uint64_t nt_enable_window(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const bool enable = call.arg_bool(1);
  const bool was_disabled = (w->style & wn::WS_DISABLED) != 0;
  if (enable == !was_disabled) return was_disabled;
  if (enable) w->style &= ~wn::WS_DISABLED; else w->style |= wn::WS_DISABLED;
  Op* op = op_begin(call, enable_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  op->a[1] = enable;
  op->a[2] = was_disabled;
  return op_continue(call, *op);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Longs

uint64_t window_get_long(Call& call, Window* w, int32_t offset, uint32_t size) {
  if (offset >= 0) {
    if (uint64_t{uint32_t(offset)} + size > w->extra.count()) return call.fail(kErrorInvalidIndex);
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; ++i) v |= uint64_t{w->extra[uint64_t(offset) + i]} << (8 * i);
    return v;
  }
  uint64_t v;
  switch (offset) {
    case wn::GWL_STYLE: v = w->style; break;
    case wn::GWL_EXSTYLE: v = w->ex_style; break;
    case wn::GWL_ID: v = (w->style & wn::WS_CHILD) ? w->id : w->menu; break;   // (a top-level window's "ID" is its menu)
    case wn::GWL_HINSTANCE: v = w->instance; break;
    case wn::GWL_WNDPROC: v = w->wndproc; break;
    case wn::GWL_USERDATA: v = w->userdata; break;
    case wn::GWL_HWNDPARENT:      // the owner of a window on the desktop, else its parent
      v = (!w->parent || w->parent == w->desktop->root) ? (w->owner ? w->owner->handle : 0) : w->parent->handle;
      break;
    default: return call.fail(kErrorInvalidIndex);
  }
  if (size == 4) v &= 0xFFFFFFFFu;
  return v;
}

// a[0] = hwnd, a[1] = offset, a[2] = new value, a[3] = size, a[4] = old value; phases: 0 STYLECHANGING, 1 apply + STYLECHANGED, 2 answer
uint64_t set_long_step(Call& call, Op& op) {
  Window* w = find_window(uint32_t(op.a[0]));
  if (!w) return 0;
  const int32_t offset = int32_t(op.a[1]);
  if (op.phase == 0) {
    op.phase = 1;
    if (offset == wn::GWL_STYLE || offset == wn::GWL_EXSTYLE || offset == wn::GWL_HWNDPARENT) notify_top_level(w);      // (the desktop's window follows the frame, buttons and owner)
    if (offset == wn::GWL_STYLE || offset == wn::GWL_EXSTYLE) {
      List<uint8_t> payload(S().alloc);
      uint8_t raw[8];
      put32(raw, 0, uint32_t(op.a[4]));
      put32(raw, 4, uint32_t(op.a[2]));
      for (uint8_t b : raw) payload.push_back(b);
      Upcall* u = nullptr;
      if (w->thread == thread_of(call)) u = queue_payload_upcall(call, &op, w, wn::WM_STYLECHANGING, sign_extend32(uint32_t(offset)), payload, 0xFFFF, 0, 0);
      else {
        u = send_proc_upcall(call, &op, w, wn::WM_STYLECHANGING, sign_extend32(uint32_t(offset)), 0);
      }
      if (u || op.waiting_on) return kUpcallDue;
    }
  }
  if (op.phase == 1) {
    op.phase = 2;
    uint64_t value = op.a[2];
    if ((offset == wn::GWL_STYLE || offset == wn::GWL_EXSTYLE) && op.payload.count() >= 8) value = get32(op.payload.view().data(), 4);
    switch (offset) {
      case wn::GWL_STYLE:
        value = uint32_t(value);
        if ((uint32_t(value) & (wn::WS_CHILD | wn::WS_POPUP)) != wn::WS_CHILD) value |= wn::WS_CLIPSIBLINGS;      // (cannot be cleared on all but a pure child)
        w->style = uint32_t(value);
        w->ex_style = fix_ex_style(w->style, w->ex_style);
        break;
      case wn::GWL_EXSTYLE:
        value = (uint32_t(value) & ~wn::WS_EX_TOPMOST) | (w->ex_style & wn::WS_EX_TOPMOST);      // only SetWindowPos changes topmost
        w->ex_style = fix_ex_style(w->style, uint32_t(value));
        break;
      case wn::GWL_ID: if (w->style & wn::WS_CHILD) w->id = value; else w->menu = value; break;
      case wn::GWL_HINSTANCE: w->instance = value; break;
      case wn::GWL_WNDPROC: w->wndproc = value; break;
      case wn::GWL_USERDATA: w->userdata = value; break;
      case wn::GWL_HWNDPARENT: {
        Window* np = value ? find_window(uint32_t(value)) : nullptr;
        if (w->parent && w->parent != w->desktop->root) {      // a window inside another: the parent changes
          if (np && np != w && !is_child_of(w, np)) { { unlink_window(w); link_window(w, np, nullptr, false); } }
        } else if (!value || np) {      // a window on the desktop: the owner changes (to the window as given, even a child)
          if (!np || (np != w && !is_child_of(w, np) && np != w->desktop->root)) w->owner = np;
        }
        break;
      }
      default:
        if (offset >= 0) {
          const uint32_t size = uint32_t(op.a[3]);
          for (uint32_t i = 0; i < size; ++i) w->extra[uint64_t(offset) + i] = uint8_t(op.a[2] >> (8 * i));
        }
        break;
    }
    if (offset == wn::GWL_STYLE || offset == wn::GWL_EXSTYLE) {
      List<uint8_t> payload(S().alloc);
      uint8_t raw[8];
      put32(raw, 0, uint32_t(op.a[4]));
      put32(raw, 4, offset == wn::GWL_STYLE ? w->style : w->ex_style);
      for (uint8_t b : raw) payload.push_back(b);
      Upcall* u = nullptr;
      if (w->thread == thread_of(call)) u = queue_payload_upcall(call, &op, w, wn::WM_STYLECHANGED, sign_extend32(uint32_t(offset)), payload, 0xFFFF, 0, 0);
      else u = send_proc_upcall(call, &op, w, wn::WM_STYLECHANGED, sign_extend32(uint32_t(offset)), 0);
      if (u || op.waiting_on) return kUpcallDue;
    }
  }
  return op.a[4];
}

uint64_t set_window_long_impl(Call& call, uint32_t size) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const int32_t offset = call.argi(1);
  uint64_t value = call.arg(2);
  if (size == 4) value &= 0xFFFFFFFFu;
  if (size == 2) value &= 0xFFFFu;
  if (offset >= 0 && uint64_t{uint32_t(offset)} + size > w->extra.count()) return call.fail(kErrorInvalidIndex);
  if (offset < 0 && offset != wn::GWL_STYLE && offset != wn::GWL_EXSTYLE && offset != wn::GWL_ID && offset != wn::GWL_HINSTANCE && offset != wn::GWL_WNDPROC &&
      offset != wn::GWL_USERDATA && offset != wn::GWL_HWNDPARENT) return call.fail(kErrorInvalidIndex);
  if (w->pid != call.pid() && (offset == wn::GWL_WNDPROC || offset == wn::GWL_HINSTANCE || offset == wn::GWL_ID)) return call.fail(kErrorAccessDenied);
  if (w == w->desktop->root) return call.fail(kErrorAccessDenied);
  Op* op = op_begin(call, set_long_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  op->a[1] = uint64_t(uint32_t(offset));
  op->a[2] = value;
  op->a[3] = size;
  op->a[4] = window_get_long(call, w, offset, offset < 0 ? (offset == wn::GWL_STYLE || offset == wn::GWL_EXSTYLE ? 4 : 8) : size);
  if (offset == wn::GWL_STYLE || offset == wn::GWL_EXSTYLE) {
    // the old value is 32 bits
    op->a[4] &= 0xFFFFFFFFu;
  }
  op->a[1] = uint64_t(int64_t(offset));
  uint64_t r = op_continue(call, *op);
  return r;
}
uint64_t nt_set_window_long(Call& call) { uint64_t r = set_window_long_impl(call, 4); return r == kUpcallDue ? r : (r & 0xFFFFFFFFu); }
uint64_t nt_set_window_long_ptr(Call& call) { return set_window_long_impl(call, 8); }
uint64_t nt_set_window_word(Call& call) { uint64_t r = set_window_long_impl(call, 2); return r == kUpcallDue ? r : (r & 0xFFFFu); }

uint64_t nt_set_window_fnid(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (w->pid != call.pid()) return call.fail(kErrorAccessDenied);
  w->fnid = call.arg32(1) & 0xFFFF;
  return 1;
}

// ULONG NtUserAlterWindowStyle(HWND, UINT mask, UINT style): changes bits without messages
uint64_t nt_alter_window_style(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t old = w->style;
  w->style = (w->style & ~call.arg32(1)) | (call.arg32(2) & call.arg32(1));
  return old;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Text

uint64_t nt_internal_get_window_text(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  const uint32_t count = call.argi(2) > 0 ? call.arg32(2) : 0;
  if (count == 0) return 0;
  const int32_t n = write_wide_z(call, call.arg(1), w->text.view(), count);
  return n < 0 ? call.fail(kErrorInvalidParameter) : uint64_t(n);
}

// BOOL NtUserDefSetText(HWND, UNICODE_STRING*)
uint64_t nt_def_set_text(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (call.arg(1) == 0) { w->text.clear(); return 1; }
  WideString text(S().alloc);
  if (!call.read_unicode_string(call.arg(1), &text)) return call.fail(kErrorInvalidParameter);
  window_text_set(w, text.view());
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Properties

bool prop_key(Call& call, uint64_t str, uint32_t* atom, bool create) {
  if (str < 0x10000) { *atom = uint32_t(str); return *atom != 0; }
  WideString name(S().alloc);
  if (!call.read_wide_z(str, 255, &name) || name.empty()) return false;
  *atom = create ? atom_add(name.view()) : atom_find(name.view());
  return *atom != 0;
}

uint64_t nt_set_prop(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint32_t atom = 0;
  if (!prop_key(call, call.arg(1), &atom, true)) return call.fail(kErrorInvalidParameter);
  for (Prop& p : w->props) if (p.atom == atom) { p.value = call.arg(2); return 1; }
  Prop p;
  p.atom = atom;
  p.value = call.arg(2);
  if (!w->props.push_back(p)) return call.fail(kErrorNotEnoughMemory);
  return 1;
}
uint64_t nt_get_prop(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint32_t atom = 0;
  if (!prop_key(call, call.arg(1), &atom, false)) return 0;
  for (Prop& p : w->props) if (p.atom == atom) return p.value;
  return 0;
}
uint64_t nt_remove_prop(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint32_t atom = 0;
  if (!prop_key(call, call.arg(1), &atom, false)) return 0;
  for (uint64_t i = 0; i < w->props.count(); ++i) {
    if (w->props[i].atom != atom) continue;
    const uint64_t v = w->props[i].value;
    w->props.remove_at(i);
    return v;
  }
  return 0;
}
// NTSTATUS NtUserBuildPropList(HWND, ULONG count, ntuser_property_list* buffer, ULONG* ret_count); entries: {UINT64 data; ATOM atom; BOOLEAN string} = 24 bytes
uint64_t nt_build_prop_list(Call& call) {
  Window* w = find_window(call.arg32(0));
  if (!w) return kStatusInvalidWindowHandle;
  const uint32_t count = call.arg32(1);
  if (call.arg(3) != 0 && !call.write_u32(call.arg(3), uint32_t(w->props.count()))) return kStatusAccessViolation;
  if (w->props.count() > count) return kStatusBufferTooSmall;
  for (uint64_t i = 0; i < w->props.count(); ++i) {
    uint8_t raw[24] = {};
    put64(raw, 0, w->props[i].value);
    raw[8] = uint8_t(w->props[i].atom);
    raw[9] = uint8_t(w->props[i].atom >> 8);
    raw[10] = atom_by_id(w->props[i].atom) ? 1 : 0;
    if (!call.write(call.arg(2) + 24 * i, ConstByteView(raw, 24))) return kStatusAccessViolation;
  }
  return kStatusSuccess;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// QueryWindow, simple window attributes

uint64_t nt_query_window(Call& call) {
  Window* w = call.arg(0) != 0 ? window_arg(call, 0) : nullptr;
  if (call.arg(0) != 0 && !w) return 0;
  const uint32_t cls = call.arg32(1);
  Queue* q = w ? find_queue(w->thread) : queue_of(call, false);
  switch (cls) {
    case 0: case 1: return w ? w->pid : call.pid();
    case 2: return w ? uint32_t(w->thread) : uint32_t(thread_of(call));
    case 3: return q && q->input_state->active ? q->input_state->active->handle : 0;
    case 4: return q && q->input_state->focus ? q->input_state->focus->handle : 0;
    case 5: return 0;
    case 6: return 0;
    case 7: return q && q->thread == S().foreground_thread;
    case 8: case 9: return 0;
    default: return call.fail(kErrorInvalidParameter);
  }
}

uint64_t nt_set_layered_window_attributes(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (!(w->ex_style & wn::WS_EX_LAYERED)) return call.fail(kErrorInvalidParameter);
  w->layered_key = call.arg32(1);
  w->layered_alpha = uint8_t(call.arg(2));
  w->layered_flags = call.arg32(3);
  return 1;
}
uint64_t nt_get_layered_window_attributes(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  if (!(w->ex_style & wn::WS_EX_LAYERED) || w->layered_flags == 0) return call.fail(kErrorInvalidParameter);
  if (call.arg(1) != 0) call.write_u32(call.arg(1), w->layered_key);
  if (call.arg(2) != 0) { const uint8_t a = w->layered_alpha; call.write(call.arg(2), ConstByteView(&a, 1)); }
  if (call.arg(3) != 0) call.write_u32(call.arg(3), w->layered_flags);
  return 1;
}
uint64_t nt_get_window_context_help_id(Call& call) { Window* w = window_arg(call, 0); return w ? w->help_id : 0; }
uint64_t nt_set_window_context_help_id(Call& call) { Window* w = window_arg(call, 0); if (!w) return 0; w->help_id = call.arg32(1); return 1; }
uint64_t nt_get_window_display_affinity(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  return call.write_u32(call.arg(1), w->display_affinity) ? 1 : call.fail(kErrorInvalidParameter);
}
uint64_t nt_set_window_display_affinity(Call& call) { Window* w = window_arg(call, 0); if (!w) return 0; w->display_affinity = call.arg32(1); return 1; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// NtUserCallHwnd

// GetDpiForWindow: an unaware window works at 96 dpi (DPI virtualization), a system-aware one at the system DPI it started at, a per-monitor aware one
// at the monitor's.
uint32_t window_dpi(Window* w) {
  if (w) return window_working_dpi(w);      // (per-monitor aware: the monitor it is on)
  return user_misc::dpi_for_monitor(user_misc::monitor_from_point(0, 0, 1));
}

enum CallHwnd : uint32_t {
  kActivateOtherWindow, kGetDialogInfo, kGetDpiForWindow, kGetLastActivePopup, kGetMDIClientInfo, kGetParent, kGetWindowDpiAwarenessContext, kGetWindowInputContext,
  kGetWindowSysSubMenu, kGetWindowTextLength, kIsWindow, kIsWindowEnabled, kIsWindowUnicode, kIsWindowVisible, kSetForegroundWindowInternal, kGetFullWindowHandle,
  kIsCurrentProcessWindow, kIsCurrentThreadWindow
};

uint64_t hwnd_op_step(Call& call, Op& op) {
  (void)call;
  return op.a[1];
}

uint64_t nt_call_hwnd(Call& call) {
  const uint32_t code = call.arg32(1);
  Window* w = find_window(call.arg32(0));
  if (code == kIsWindow) return w != nullptr;
  if (code == kGetFullWindowHandle) return w ? w->handle : 0;
  if (!w) {
    if (code == kIsCurrentProcessWindow || code == kIsCurrentThreadWindow || code == kIsWindowVisible || code == kIsWindowEnabled || code == kIsWindowUnicode) return call.fail(kErrorInvalidWindowHandle);
    return call.fail(kErrorInvalidWindowHandle);
  }
  switch (code) {
    case kActivateOtherWindow: {
      Op* op = op_begin(call, hwnd_op_step);
      if (!op) return 0;
      op->a[1] = 0;
      activate_other_window(call, op, w);
      return op_continue(call, *op);
    }
    case kGetDialogInfo: return w->dialog_info;
    case kGetDpiForWindow: return window_dpi(w);
    case kGetLastActivePopup: return w->last_active_popup ? w->last_active_popup->handle : w->handle;
    case kGetMDIClientInfo: return w->mdi_info;
    case kGetParent:
      if (w == w->desktop->root) return 0;
      if (w->style & wn::WS_POPUP) return w->owner ? w->owner->handle : 0;      // (a popup is owned even when it also has WS_CHILD, as in Windows)
      if (w->style & wn::WS_CHILD) return w->parent ? w->parent->handle : 0;      // (a child of the desktop has the desktop as its parent)
      return 0;
    case kGetWindowDpiAwarenessContext: return w->dpi_context ? w->dpi_context : 0x6010u;
    case kGetWindowInputContext: return user_misc::window_input_context_raw(w->handle);
    case kGetWindowSysSubMenu: return w->sys_menu;
    case kGetWindowTextLength: return w->text.size();
    case kIsWindowEnabled: return is_enabled(w);
    case kIsWindowUnicode: return w->unicode;
    case kIsWindowVisible: return is_visible(w);
    case kSetForegroundWindowInternal: return set_foreground_internal(w);
    case kIsCurrentProcessWindow: return w->pid == call.pid();
    case kIsCurrentThreadWindow: return w->thread == thread_of(call);
    default: return call.fail(kErrorInvalidParameter);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// NtUserCallHwndParam

enum CallHwndParam : uint32_t {
  kClientToScreen, kGetChildRect, kGetClassLongA, kGetClassLongW, kGetClassLongPtrA, kGetClassLongPtrW, kGetClassWord, kGetScrollInfo, kGetWindowInfo, kGetWindowLongA,
  kGetWindowLongW, kGetWindowLongPtrA, kGetWindowLongPtrW, kGetWindowRect, kGetClientRect, kGetPresentRect, kGetWindowRelative, kGetWindowThread, kGetWindowWord, kIsChild,
  kMapWindowPoints, kMirrorRgn, kMonitorFromWindow, kScreenToClient, kSetDialogInfo, kSetMDIClientInfo, kSendHardwareInput, kExposeWindowSurface, kGetWinMonitorDpi,
  kSetRawWindowPos, kGetPrivateData, kSetPrivateData
};

uint64_t get_scroll_info(Call& call, Window* w, uint64_t params) {
  uint32_t bar = 0;
  uint64_t info = 0;
  if (!call.read_u32(params, &bar) || !call.read_u64(params + 8, &info)) return call.fail(kErrorInvalidParameter);
  uint32_t size = 0, mask = 0;
  if (!call.read_u32(info, &size) || !call.read_u32(info + 4, &mask) || size != 28) return call.fail(kErrorInvalidParameter);
  if (bar > wn::SB_VERT) return call.fail(kErrorInvalidParameter);      // SB_CTL windows keep their own state
  const ScrollBar& sb = w->scroll[bar];
  uint8_t raw[20] = {};
  if (mask & wn::SIF_RANGE) { put32(raw, 0, uint32_t(sb.min)); put32(raw, 4, uint32_t(sb.max)); }
  if (mask & wn::SIF_PAGE) put32(raw, 8, uint32_t(sb.page));
  if (mask & wn::SIF_POS) put32(raw, 12, uint32_t(sb.pos));
  if (mask & wn::SIF_TRACKPOS) put32(raw, 16, uint32_t(sb.track_pos));
  uint8_t current[20];
  if (!call.read(info + 8, ByteView(current, 20))) return call.fail(kErrorInvalidParameter);
  if (mask & wn::SIF_RANGE) for (int i = 0; i < 8; ++i) current[i] = raw[i];
  if (mask & wn::SIF_PAGE) for (int i = 8; i < 12; ++i) current[i] = raw[i];
  if (mask & wn::SIF_POS) for (int i = 12; i < 16; ++i) current[i] = raw[i];
  if (mask & wn::SIF_TRACKPOS) for (int i = 16; i < 20; ++i) current[i] = raw[i];
  if (!call.write(info + 8, ConstByteView(current, 20))) return call.fail(kErrorInvalidParameter);
  // (a scroll bar that is not there reports FALSE, as Windows does for a missing WS_xSCROLL bar with no range)
  return 1;
}

uint64_t get_window_info(Call& call, Window* w, uint64_t address) {
  uint32_t size = 0;
  if (!call.read_u32(address, &size)) return call.fail(kErrorInvalidParameter);      // (Windows ignores cbSize)
  uint8_t raw[60] = {};
  put32(raw, 0, 60);
  const uint32_t k = caller_scale_for(call, w);      // (in the caller's coordinates)
  auto wr = [&](uint32_t o, const Rect& r0) {
    const Rect r = dpi_down(r0, k);
    put32(raw, o, uint32_t(r.left)); put32(raw, o + 4, uint32_t(r.top)); put32(raw, o + 8, uint32_t(r.right)); put32(raw, o + 12, uint32_t(r.bottom));
  };
  wr(4, window_rect_screen(w));
  wr(20, client_rect_screen(w));
  put32(raw, 36, w->style);
  put32(raw, 40, w->ex_style);
  Queue* q = find_queue(w->thread);
  put32(raw, 44, q && q->input_state->active == w ? 1 : 0);
  const Rect win = dpi_down(window_rect_screen(w), k), cl = dpi_down(client_rect_screen(w), k);
  put32(raw, 48, uint32_t(cl.left - win.left));
  put32(raw, 52, uint32_t(cl.top - win.top));
  raw[56] = uint8_t(w->cls ? w->cls->atom : 0);
  raw[57] = uint8_t((w->cls ? w->cls->atom : 0) >> 8);
  raw[58] = 0;      // wCreatorVersion 0x0500
  raw[59] = 5;
  return call.write(address, ConstByteView(raw, 60)) ? 1 : call.fail(kErrorInvalidParameter);
}

// MapWindowPoints: params {HWND hwnd_to; POINT* points; UINT count; UINT dpi}
uint64_t map_window_points(Call& call, Window* from, uint64_t params) {
  uint64_t to_h = 0, points = 0;
  uint32_t count = 0;
  if (!call.read_u64(params, &to_h) || !call.read_u64(params + 8, &points) || !call.read_u32(params + 16, &count)) return call.fail(kErrorInvalidParameter);
  Window* to = to_h ? find_window(uint32_t(to_h)) : nullptr;
  if (to_h != 0 && !to) return call.fail(kErrorInvalidWindowHandle, 0);
  int32_t fx = 0, fy = 0, tx = 0, ty = 0;
  if (from && from != from->desktop->root) client_origin(from, &fx, &fy);
  if (to && to != to->desktop->root) client_origin(to, &tx, &ty);
  const int32_t dx = fx - tx, dy = fy - ty;
  const uint32_t k = caller_scale(call);      // (the caller's points: an unaware one's 96-dpi units, through device pixels and back)
  for (uint32_t i = 0; i < count; ++i) {
    Point p;
    if (!read_point(call, points + 8ull * i, &p)) return call.fail(kErrorInvalidParameter);
    p.x = dpi_down(int32_t(uint32_t(dpi_up(p.x, k)) + uint32_t(dx)), k);
    p.y = dpi_down(int32_t(uint32_t(dpi_up(p.y, k)) + uint32_t(dy)), k);
    if (!write_point(call, points + 8ull * i, p)) return call.fail(kErrorInvalidParameter);
  }
  return (uint64_t(uint16_t(dpi_down(dy, k))) << 16) | uint16_t(dpi_down(dx, k));
}

uint64_t nt_call_hwnd_param(Call& call) {
  const uint32_t code = call.arg32(2);
  const uint64_t param = call.arg(1);
  Window* w = find_window(call.arg32(0));
  // codes that take a window that may be absent
  if (code == kMapWindowPoints) {
    if (call.arg(0) != 0 && !w) return call.fail(kErrorInvalidWindowHandle);
    return map_window_points(call, w, param);
  }
  if (code == kIsChild) {
    Window* child = find_window(uint32_t(param));
    if (!w || !child) return 0;
    for (Window* p = child; p && (p->style & wn::WS_CHILD) && p->parent; p = p->parent) if (p->parent == w) return 1;      // (only through child windows: the desktop is nobody's parent)
    return 0;
  }
  if (!w) return call.fail(kErrorInvalidWindowHandle);
  switch (code) {
    case kClientToScreen: case kScreenToClient: {
      Point p;
      if (!read_point(call, param, &p)) return call.fail(kErrorInvalidParameter);
      int32_t ox, oy;
      if (w == w->desktop->root) ox = oy = 0; else client_origin(w, &ox, &oy);
      const uint32_t k = caller_scale_for(call, w);
      p.x = dpi_up(p.x, k);
      p.y = dpi_up(p.y, k);
      if (code == kClientToScreen) { p.x = int32_t(uint32_t(p.x) + uint32_t(ox)); p.y = int32_t(uint32_t(p.y) + uint32_t(oy)); }
      else { p.x = int32_t(uint32_t(p.x) - uint32_t(ox)); p.y = int32_t(uint32_t(p.y) - uint32_t(oy)); }
      p.x = dpi_down(p.x, k);
      p.y = dpi_down(p.y, k);
      return write_point(call, param, p) ? 1 : call.fail(kErrorInvalidParameter);
    }
    case kGetChildRect: return write_rect(call, param, dpi_down(w->bounds, caller_scale_for(call, w))) ? 1 : call.fail(kErrorInvalidParameter);
    case kGetClassLongA: case kGetClassLongW: case kGetClassLongPtrA: case kGetClassLongPtrW: case kGetClassWord: {
      const uint32_t size = code == kGetClassWord ? 2 : (code == kGetClassLongA || code == kGetClassLongW) ? 4 : 8;
      uint64_t v = 0;
      if (!class_get_value(call, w, int32_t(param), size, &v)) return 0;
      return v;
    }
    case kGetScrollInfo: return get_scroll_info(call, w, param);
    case kGetWindowInfo: return get_window_info(call, w, param);
    case kGetWindowLongA: case kGetWindowLongW: return window_get_long(call, w, int32_t(param), 4);
    case kGetWindowLongPtrA: case kGetWindowLongPtrW: return window_get_long(call, w, int32_t(param), 8);
    case kGetWindowWord: return window_get_long(call, w, int32_t(param), 2);
    case kGetWindowRect: case kGetClientRect: case kGetPresentRect: {
      uint64_t rect = 0;
      if (!call.read_u64(param, &rect)) return call.fail(kErrorInvalidParameter);
      Rect r;
      const uint32_t k = caller_scale_for(call, w);      // (an unaware caller's client size is what its WM_SIZE said)
      if (code == kGetClientRect) {
        const Rect cl = dpi_down(w->client, k);
        r = Rect{0, 0, cl.right - cl.left, cl.bottom - cl.top};
      } else {
        r = dpi_down(window_rect_screen(w), k);
      }
      return write_rect(call, rect, r) ? 1 : call.fail(kErrorInvalidParameter);
    }
    case kGetWindowRelative: {
      Window* r = get_window_relative(w, uint32_t(param));
      return r ? r->handle : 0;
    }
    case kGetWindowThread:
      if (param != 0) call.write_u32(param, uint32_t(w->pid));
      return uint32_t(w->thread);
    case kMonitorFromWindow: return user_misc::monitor_from_rect(window_rect_screen(w), uint32_t(param));
    case kSetDialogInfo: w->dialog_info = param; return 0;
    case kSetMDIClientInfo: w->mdi_info = param; return 0;
    case kGetWinMonitorDpi: return window_dpi(w);
    case kSetRawWindowPos: {
      Rect r;
      uint32_t flags = 0;
      if (!read_rect(call, param, &r) || !call.read_u32(param + 16, &flags)) return call.fail(kErrorInvalidParameter);
      w->bounds = dpi_up(r, caller_scale_for(call, w));
      compute_nc(w, &w->client);
      return 1;
    }
    case kGetPrivateData: case kSetPrivateData: {
      uint32_t offset = 0, size = 0;
      uint64_t value = 0;
      if (!call.read_u32(param, &offset) || !call.read_u32(param + 4, &size) || (size != 1 && size != 2 && size != 4 && size != 8) || offset + size > sizeof(w->private_data)) return call.fail(kErrorInvalidParameter);
      uint8_t* bytes = reinterpret_cast<uint8_t*>(w->private_data);      // (the array of 64-bit words as bytes: always valid)
      uint64_t old = 0;
      for (uint32_t i = 0; i < size; ++i) old |= uint64_t{bytes[offset + i]} << (8 * i);
      if (code == kSetPrivateData) {
        if (!call.read_u64(param + 8, &value)) return call.fail(kErrorInvalidParameter);
        for (uint32_t i = 0; i < size; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
      }
      return old;
    }
    default:
      return call.fail(kErrorNotSupported);      // MirrorRgn, SendHardwareInput, ExposeWindowSurface: not available
  }
}

}  // namespace

uint64_t destroy_window_op(Call& call, Window* w) {
  Op* op = op_begin(call, destroy_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->a[0] = w->handle;
  return op_continue(call, *op);
}

void register_window_services() {
  register_handler(WindowsSyscallNumber::NtUserCreateWindowEx, nt_create_window_ex);
  register_handler(WindowsSyscallNumber::NtUserDestroyWindow, nt_destroy_window);
  register_handler(WindowsSyscallNumber::NtUserSetParent, nt_set_parent);
  register_handler(WindowsSyscallNumber::NtUserGetAncestor, nt_get_ancestor);
  register_handler(WindowsSyscallNumber::NtUserFindWindowEx, nt_find_window_ex);
  register_handler(WindowsSyscallNumber::NtUserBuildHwndList, nt_build_hwnd_list);
  register_handler(WindowsSyscallNumber::NtUserEnableWindow, nt_enable_window);
  register_handler(WindowsSyscallNumber::NtUserSetWindowLong, nt_set_window_long);
  register_handler(WindowsSyscallNumber::NtUserSetWindowLongPtr, nt_set_window_long_ptr);
  register_handler(WindowsSyscallNumber::NtUserSetWindowWord, nt_set_window_word);
  register_handler(WindowsSyscallNumber::NtUserSetWindowFNID, nt_set_window_fnid);
  register_handler(WindowsSyscallNumber::NtUserAlterWindowStyle, nt_alter_window_style);
  register_handler(WindowsSyscallNumber::NtUserInternalGetWindowText, nt_internal_get_window_text);
  register_handler(WindowsSyscallNumber::NtUserDefSetText, nt_def_set_text);
  register_handler(WindowsSyscallNumber::NtUserSetProp, nt_set_prop);
  register_handler(WindowsSyscallNumber::NtUserGetProp, nt_get_prop);
  register_handler(WindowsSyscallNumber::NtUserRemoveProp, nt_remove_prop);
  register_handler(WindowsSyscallNumber::NtUserBuildPropList, nt_build_prop_list);
  register_handler(WindowsSyscallNumber::NtUserQueryWindow, nt_query_window);
  register_handler(WindowsSyscallNumber::NtUserSetLayeredWindowAttributes, nt_set_layered_window_attributes);
  register_handler(WindowsSyscallNumber::NtUserGetLayeredWindowAttributes, nt_get_layered_window_attributes);
  register_handler(WindowsSyscallNumber::NtUserGetWindowContextHelpId, nt_get_window_context_help_id);
  register_handler(WindowsSyscallNumber::NtUserSetWindowContextHelpId, nt_set_window_context_help_id);
  register_handler(WindowsSyscallNumber::NtUserGetWindowDisplayAffinity, nt_get_window_display_affinity);
  register_handler(WindowsSyscallNumber::NtUserSetWindowDisplayAffinity, nt_set_window_display_affinity);
  register_handler(WindowsSyscallNumber::NtUserCallHwnd, nt_call_hwnd);
  register_handler(WindowsSyscallNumber::NtUserCallHwndParam, nt_call_hwnd_param);
}

}  // namespace process::win32k::user
