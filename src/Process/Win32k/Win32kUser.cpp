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
// The window manager's core: its state, the window tree, regions of rectangles, atoms, and what the other win32k modules call (Win32kUser.hpp).
#include "Process/Win32k/Win32kUserInternal.hpp"
#include "Utility/UniquePointer.hpp"

namespace process::win32k::user {

namespace {

UniquePointer<State> g_state;
#if __STDC_HOSTED__
thread_local      // (host tests drive several threads; the kernel has no thread-local storage and never sets the override)
#endif
uint64_t g_thread_override = 0;

int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Rectangles and regions

bool rect_empty(const Rect& r) { return r.right <= r.left || r.bottom <= r.top; }
bool rect_contains(const Rect& r, int32_t x, int32_t y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool rect_equal(const Rect& a, const Rect& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
Rect rect_intersect(const Rect& a, const Rect& b) {
  Rect r;
  r.left = max32(a.left, b.left);
  r.top = max32(a.top, b.top);
  r.right = min32(a.right, b.right);
  r.bottom = min32(a.bottom, b.bottom);
  if (rect_empty(r)) return Rect();
  return r;
}
Rect rect_offset(const Rect& r, int32_t dx, int32_t dy) {
  // wraps like the Windows API on overflow, without undefined behaviour
  Rect o;
  o.left = int32_t(uint32_t(r.left) + uint32_t(dx));
  o.right = int32_t(uint32_t(r.right) + uint32_t(dx));
  o.top = int32_t(uint32_t(r.top) + uint32_t(dy));
  o.bottom = int32_t(uint32_t(r.bottom) + uint32_t(dy));
  return o;
}

// `a` minus `b`, as up to four rectangles appended to `out`.
static bool rect_minus(const Rect& a, const Rect& b, List<Rect>& out) {
  const Rect i = rect_intersect(a, b);
  if (rect_empty(i)) return out.push_back(a);
  if (i.top > a.top && !out.push_back(Rect{a.left, a.top, a.right, i.top})) return false;
  if (i.bottom < a.bottom && !out.push_back(Rect{a.left, i.bottom, a.right, a.bottom})) return false;
  if (i.left > a.left && !out.push_back(Rect{a.left, i.top, i.left, i.bottom})) return false;
  if (i.right < a.right && !out.push_back(Rect{i.right, i.top, a.right, i.bottom})) return false;
  return true;
}

void region_subtract(List<Rect>& region, const Rect& r) {
  if (rect_empty(r)) return;
  List<Rect> result(state().alloc);
  for (const Rect& piece : region) if (!rect_minus(piece, r, result)) return;      // out of memory: keep the region as it was
  region.clear();
  for (const Rect& piece : result) region.push_back(piece);
}

bool region_add(List<Rect>& region, const Rect& r) {
  if (rect_empty(r)) return true;
  List<Rect> pieces(state().alloc);
  if (!pieces.push_back(r)) return false;
  // pieces = r minus everything already in the region, then appended
  for (const Rect& existing : region) {
    List<Rect> next(state().alloc);
    for (const Rect& p : pieces) if (!rect_minus(p, existing, next)) return false;
    pieces.clear();
    for (const Rect& p : next) if (!pieces.push_back(p)) return false;
    if (pieces.count() == 0) return true;
  }
  for (const Rect& p : pieces) if (!region.push_back(p)) return false;
  return true;
}

void region_intersect(List<Rect>& region, const Rect& r) {
  List<Rect> result(state().alloc);
  for (const Rect& piece : region) {
    const Rect i = rect_intersect(piece, r);
    if (!rect_empty(i)) result.push_back(i);
  }
  region.clear();
  for (const Rect& piece : result) region.push_back(piece);
}

void region_offset(List<Rect>& region, int32_t dx, int32_t dy) {
  for (Rect& r : region) r = rect_offset(r, dx, dy);
}

Rect region_bounds(const List<Rect>& region) {
  Rect b;
  bool first = true;
  for (const Rect& r : region) {
    if (first) { b = r; first = false; continue; }
    b.left = min32(b.left, r.left);
    b.top = min32(b.top, r.top);
    b.right = max32(b.right, r.right);
    b.bottom = max32(b.bottom, r.bottom);
  }
  return b;
}

bool region_contains(const List<Rect>& region, int32_t x, int32_t y) {
  for (const Rect& r : region) if (rect_contains(r, x, y)) return true;
  return false;
}

bool region_copy(List<Rect>& to, const List<Rect>& from) {
  to.clear();
  for (const Rect& r : from) if (!to.push_back(r)) return false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// State

State::State(allocators::IAllocator& a)
    : alloc(a), windows(a), hooks(a), win_events(a), defers(a), stations(a), desktops(a), object_handles(a), classes(a), atoms(a), queues(a), input_states(a), upcalls(a),
      ops(a), timers(a), hotkeys(a), processes(a), dcs(a), thread_desktops(a), drops(a) {}

State* state_or_null() { return g_state.get(); }

State& state() {
  if (!g_state) {
    allocators::IAllocator* a = system_allocator();
    g_state = UniquePointer<State>::make(*a, *a);
  }
  return *g_state;
}

void reset_state() { g_state.reset(); }
void set_test_thread(uint64_t tid) { g_thread_override = tid; }
uint64_t thread_of(Call& call) { return g_thread_override != 0 ? g_thread_override : call.tid(); }

uint32_t now_ms_from_ns(uint64_t ns) { return uint32_t(ns / 1000000u); }
uint32_t now_ms(Call& call) { return now_ms_from_ns(now_ns(call.process())); }

uint64_t sign_extend32(uint32_t v) { return uint64_t(int64_t(int32_t(v))); }

int32_t write_wide_z(Call& call, uint64_t address, ArrayView<const char16_t> text, uint32_t capacity) {
  if (capacity == 0) return 0;
  const uint64_t n = text.size() < capacity - 1 ? text.size() : capacity - 1;
  return call.write_wide(address, text.first(n), capacity, true);
}

bool read_rect(Call& call, uint64_t address, Rect* out) {
  int32_t v[4];
  if (!call.read_array(address, v, 4)) return false;
  *out = Rect{v[0], v[1], v[2], v[3]};
  return true;
}
bool write_rect(Call& call, uint64_t address, const Rect& r) {
  const int32_t v[4] = {r.left, r.top, r.right, r.bottom};
  return call.write_array(address, v, 4);
}
bool read_point(Call& call, uint64_t address, Point* out) {
  int32_t v[2];
  if (!call.read_array(address, v, 2)) return false;
  out->x = v[0];
  out->y = v[1];
  return true;
}
bool write_point(Call& call, uint64_t address, const Point& p) {
  const int32_t v[2] = {p.x, p.y};
  return call.write_array(address, v, 2);
}

ProcessInfo& process_info(uint64_t pid) {
  State& s = state();
  for (ProcessInfo& p : s.processes) if (p.pid == pid) return p;
  ProcessInfo* p = s.processes.emplace();
  p->pid = pid;
  return *p;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Atoms

static bool atom_equal(const WideString& a, ArrayView<const char16_t> b) { return a.equals(b, true); }

uint32_t atom_find(ArrayView<const char16_t> name) {
  for (Atom& a : state().atoms) if (atom_equal(a.name, name)) return a.id;
  return 0;
}
uint32_t atom_add(ArrayView<const char16_t> name) {
  if (const uint32_t found = atom_find(name)) return found;
  State& s = state();
  if (s.next_atom > 0xFFFF || name.size() == 0 || name.size() > 255) return 0;
  Atom* a = s.atoms.emplace(s.alloc);
  if (!a) return 0;
  if (!a->name.append(name)) { s.atoms.erase(a); return 0; }
  a->id = s.next_atom++;
  return a->id;
}
const Atom* atom_by_id(uint32_t id) {
  for (Atom& a : state().atoms) if (a.id == id) return &a;
  return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The window tree

Window* find_window(uint32_t handle) { return state().windows.find(handle); }

Window* window_arg(Call& call, uint32_t index) {
  Window* w = find_window(call.arg32(index));
  if (!w) call.set_last_error(kErrorInvalidWindowHandle);
  return w;
}

bool is_child_of(const Window* ancestor, const Window* w) {
  for (const Window* p = w ? w->parent : nullptr; p; p = p->parent) if (p == ancestor) return true;
  return false;
}
bool is_top_level(const Window* w) { return w && w->parent && !w->parent->parent; }
bool is_enabled(const Window* w) { return (w->style & wn::WS_DISABLED) == 0; }
bool is_visible(const Window* w) {
  for (; w; w = w->parent) if (w->parent && (w->style & wn::WS_VISIBLE) == 0) return false;
  return true;
}
Window* top_level_of(Window* w) {
  while (w && w->parent && w->parent->parent) w = w->parent;
  return w;
}

void parent_origin(const Window* w, int32_t* x, int32_t* y) {
  *x = *y = 0;
  if (w && w->parent) client_origin(w->parent, x, y);
}
void client_origin(const Window* w, int32_t* x, int32_t* y) {
  int32_t px = 0, py = 0;
  if (w->parent) parent_origin(w, &px, &py);
  *x = int32_t(uint32_t(w->client.left) + uint32_t(px));
  *y = int32_t(uint32_t(w->client.top) + uint32_t(py));
}
Rect window_rect_screen(const Window* w) {
  int32_t px, py;
  parent_origin(w, &px, &py);
  return rect_offset(w->bounds, px, py);
}
Rect client_rect_screen(const Window* w) {
  int32_t px, py;
  parent_origin(w, &px, &py);
  return rect_offset(w->client, px, py);
}

void link_window(Window* w, Window* parent, Window* insert_after, bool at_end) {
  w->parent = parent;
  w->prev = w->next = nullptr;
  if (!parent) return;
  if (at_end || (insert_after == nullptr && parent->first_child == nullptr)) {
    w->prev = parent->last_child;
    if (parent->last_child) parent->last_child->next = w; else parent->first_child = w;
    parent->last_child = w;
  } else if (insert_after == nullptr) {
    w->next = parent->first_child;
    parent->first_child->prev = w;
    parent->first_child = w;
  } else {
    w->prev = insert_after;
    w->next = insert_after->next;
    if (insert_after->next) insert_after->next->prev = w; else parent->last_child = w;
    insert_after->next = w;
  }
}

void unlink_window(Window* w) {
  Window* parent = w->parent;
  if (!parent) return;
  if (w->prev) w->prev->next = w->next; else parent->first_child = w->next;
  if (w->next) w->next->prev = w->prev; else parent->last_child = w->prev;
  w->prev = w->next = nullptr;
}

Window* get_window_relative(const Window* w, uint32_t rel) {
  switch (rel) {
    case wn::GW_HWNDFIRST: return w->parent ? w->parent->first_child : const_cast<Window*>(w);
    case wn::GW_HWNDLAST: return w->parent ? w->parent->last_child : const_cast<Window*>(w);
    case wn::GW_HWNDNEXT: return w->next;
    case wn::GW_HWNDPREV: return w->prev;
    case wn::GW_OWNER: return w->owner;
    case wn::GW_CHILD: return w->first_child;
    default: return nullptr;
  }
}

void window_text_set(Window* w, ArrayView<const char16_t> text) {
  w->text.clear();
  w->text.append(text);
  if (is_top_level(w)) notify_top_level(w);      // (the desktop's window shows the title)
}

// The default desktop: a window station "WinSta0" with the desktop "Default" and its window, made on first use.
Desktop* default_desktop() {
  State& s = state();
  if (s.input_desktop) return s.input_desktop;
  create_system_classes();
  WinSta* station = s.stations.create(s.alloc);
  if (!station) return nullptr;
  station->name.append_ascii("WinSta0");
  station->flags = 1;      // WSF_VISIBLE
  Desktop* desktop = s.desktops.create(s.alloc);
  if (!desktop) return nullptr;
  desktop->name.append_ascii("Default");
  desktop->station = station;
  auto make_root = [&](uint32_t atom, bool message) -> Window* {
    Window* w = s.windows.create(s.alloc);
    if (!w) return nullptr;
    Class* cls = message ? s.message_class : s.desktop_class;
    (void)atom;
    w->cls = cls;
    if (cls) ++cls->windows;
    w->desktop = desktop;
    w->style = wn::WS_VISIBLE | wn::WS_CLIPCHILDREN;
    if (message) w->style = wn::WS_CLIPCHILDREN;
    w->bounds = w->client = Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())};
    return w;
  };
  desktop->root = make_root(wn::ATOM_DESKTOP, false);
  s.initial_desktop = desktop;
  if (!desktop->root) return nullptr;
  desktop->message_root = make_root(0, true);      // the "Message" class
  if (desktop->message_root) {
    link_window(desktop->message_root, desktop->root, nullptr, true);
    desktop->message_root->style &= ~wn::WS_VISIBLE;
  }
  s.input_desktop = desktop;
  return desktop;
}

Desktop* thread_desktop(Call& call) {
  State& s = state();
  const uint64_t tid = thread_of(call);
  for (const ThreadDesktop& t : s.thread_desktops) {
    if (t.thread != tid) continue;
    if (ObjHandle* h = s.object_handles.find(t.handle)) if (h->desktop) return h->desktop;
  }
  default_desktop();
  return s.initial_desktop;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Hit testing

uint32_t hit_test(const Window* w, int32_t x, int32_t y) {
  const Rect win = window_rect_screen(w), cl = client_rect_screen(w);
  if (!rect_contains(win, x, y)) return wn::HTNOWHERE;
  if (rect_contains(cl, x, y)) return wn::HTCLIENT;
  const uint32_t style = w->style;
  const int32_t frame = (style & wn::WS_THICKFRAME) ? 8 : 0;
  if (frame) {
    const bool left = x < win.left + frame, right = x >= win.right - frame, top = y < win.top + frame, bottom = y >= win.bottom - frame;
    if (top && left) return wn::HTTOPLEFT;
    if (top && right) return wn::HTTOPRIGHT;
    if (bottom && left) return wn::HTBOTTOMLEFT;
    if (bottom && right) return wn::HTBOTTOMRIGHT;
    if (top) return wn::HTTOP;
    if (bottom) return wn::HTBOTTOM;
    if (left) return wn::HTLEFT;
    if (right) return wn::HTRIGHT;
  }
  if ((style & wn::WS_VSCROLL) && x >= cl.right && y >= cl.top && y < cl.bottom) return wn::HTVSCROLL;
  if ((style & wn::WS_HSCROLL) && y >= cl.bottom && x >= cl.left && x < cl.right) return wn::HTHSCROLL;
  if ((style & wn::WS_CAPTION) == wn::WS_CAPTION && y < cl.top) {
    const int32_t button = 32;
    if ((style & wn::WS_SYSMENU) && x >= win.right - frame - button && x < win.right - frame) return wn::HTCLOSE;
    if ((style & wn::WS_SYSMENU) && x < win.left + frame + button) return wn::HTSYSMENU;
    return wn::HTCAPTION;
  }
  return wn::HTBORDER;
}

// The child of `parent` at the screen point (front to back). `real`: also hidden/disabled/transparent windows count as ChildWindowFromPointEx(flags) would skip; here only
// invisible windows are skipped.
Window* child_at_point(Window* parent, int32_t x, int32_t y, bool real) {
  for (Window* c = parent->first_child; c; c = c->next) {
    if (!(c->style & wn::WS_VISIBLE)) continue;
    if (!rect_contains(window_rect_screen(c), x, y)) continue;
    if (!real && (c->ex_style & wn::WS_EX_TRANSPARENT)) continue;
    return c;
  }
  return nullptr;
}

static Window* descend_at_point(Window* parent, int32_t x, int32_t y, bool skip_transparent, uint64_t tid) {
  for (Window* c = parent->first_child; c; c = c->next) {
    if (!(c->style & wn::WS_VISIBLE)) continue;
    if (!rect_contains(window_rect_screen(c), x, y)) continue;
    if (skip_transparent && (c->ex_style & wn::WS_EX_TRANSPARENT) && c->thread != tid) continue;
    if (c->style & wn::WS_MINIMIZE) return c;
    if (rect_contains(client_rect_screen(c), x, y)) {
      if (Window* deeper = descend_at_point(c, x, y, skip_transparent, tid)) return deeper;
    }
    return c;
  }
  return nullptr;
}

Window* window_at_point(int32_t x, int32_t y, bool skip_transparent, uint64_t tid) {
  Desktop* d = default_desktop();
  if (!d || !d->root) return nullptr;
  Window* w = descend_at_point(d->root, x, y, skip_transparent, tid);
  return w ? w : (rect_contains(window_rect_screen(d->root), x, y) ? d->root : nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The window manager's contract with the rest of win32k

bool is_window(uint32_t hwnd) { return state().windows.find(hwnd) != nullptr; }
void set_window_menu(uint32_t hwnd, uint32_t menu) {
  Window* w = find_window(hwnd);
  if (w && !(w->style & wn::WS_CHILD)) w->menu = menu;
}
uint64_t window_thread(uint32_t hwnd) { Window* w = find_window(hwnd); return w ? w->thread : 0; }
uint64_t window_process(uint32_t hwnd) { Window* w = find_window(hwnd); return w ? w->pid : 0; }
bool window_screen_rect(uint32_t hwnd, gdi::Rect* rect) {
  Window* w = find_window(hwnd);
  if (!w) return false;
  *rect = window_rect_screen(w);
  return true;
}
bool window_client_screen_rect(uint32_t hwnd, gdi::Rect* rect) {
  Window* w = find_window(hwnd);
  if (!w) return false;
  *rect = client_rect_screen(w);
  return true;
}
bool window_visible(uint32_t hwnd) { Window* w = find_window(hwnd); return w && is_visible(w); }
uint32_t desktop_window() {
  Desktop* d = default_desktop();
  return d && d->root ? d->root->handle : 0;
}
uint32_t window_from_screen_point(int32_t x, int32_t y) {
  Window* w = window_at_point(x, y, true, 0);
  return w ? w->handle : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Destruction without messages

void destroy_window_now(Window* w) {
  State& s = state();
  const uint32_t handle = w->handle;
  // children have gone already (destroy_window_tree); what refers to the window now goes
  w->destroying = true;
  if (is_top_level(w)) { drop_surface(w); notify_top_level(w); }      // (the desktop's window goes)
  focus_window_gone(w);
  kill_timers_of_window(handle);
  drop_hotkeys_of_window(handle);
  remove_window_messages(w);
  release_window_dcs(handle);
  if (s.lock_update_window == handle) s.lock_update_window = 0;
  for (Window& o : s.windows.items()) {
    if (o.owner == w) o.owner = nullptr;
    if (o.last_active_popup == w) o.last_active_popup = nullptr;
  }
  if (w->parent) {
    unlink_window(w);
  }
  if (w->cls) class_window_gone(w->cls);
  user_misc::window_destroyed(handle);
  if (Desktop* d = w->desktop) {
    if (d->root == w) d->root = nullptr;
    if (d->message_root == w) d->message_root = nullptr;
  }
  s.windows.destroy(w);
}

void destroy_window_tree(Window* w) {
  while (w->first_child) destroy_window_tree(w->first_child);
  // windows owned by this one go with it
  for (;;) {
    Window* owned = nullptr;
    for (Window& o : state().windows.items()) if (o.owner == w && !o.destroying) { owned = &o; break; }
    if (!owned) break;
    owned->owner = nullptr;
    destroy_window_tree(owned);
  }
  destroy_window_now(w);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Process exit

void on_process_exit_hook(uint64_t pid) {
  State* sp = state_or_null();
  if (!sp) return;
  State& s = *sp;
  for (;;) {
    Window* victim = nullptr;
    for (Window& w : s.windows.items()) {
      if (w.pid == pid && w.parent) { victim = top_level_of(&w) && top_level_of(&w)->pid == pid ? top_level_of(&w) : &w; break; }
    }
    if (!victim) break;
    destroy_window_tree(victim);
  }
  // queues of the process
  for (;;) {
    Queue* q = nullptr;
    for (Queue& x : s.queues) if (x.pid == pid) { q = &x; break; }
    if (!q) break;
    thread_gone(q->thread);
  }
  for (;;) {
    Timer* t = nullptr;
    for (Timer& x : s.timers) if (x.pid == pid) { t = &x; break; }
    if (!t) break;
    s.timers.erase(t);
  }
  // upcalls other threads were waiting for from this process's threads, and sends of its threads still in flight
  for (Upcall& u : s.upcalls) {
    if (u.sender_pid == pid && u.state != Upcall::Done) u.abandoned = true;
  }
  List<uint64_t> doomed(s.alloc);
  for (Upcall& u : s.upcalls) if (u.abandoned && u.state == Upcall::Queued) doomed.push_back(u.id);
  for (Upcall& u : s.upcalls) if (u.state == Upcall::Done && u.abandoned) doomed.push_back(u.id);
  for (uint64_t id : doomed) for (Upcall& u : s.upcalls) if (u.id == id) { s.upcalls.erase(&u); break; }
  for (;;) {
    HotKey* h = nullptr;
    for (HotKey& x : s.hotkeys) if ((x.thread & 0xFFFFF) == pid) { h = &x; break; }
    if (!h) break;
    s.hotkeys.erase(h);
  }
  for (;;) {
    Hook* h = nullptr;
    for (Hook& x : s.hooks.items()) if (x.pid == pid) { h = &x; break; }
    if (!h) break;
    s.hooks.destroy(h);
  }
  for (;;) {
    WinEventHook* h = nullptr;
    for (WinEventHook& x : s.win_events.items()) if (x.pid == pid) { h = &x; break; }
    if (!h) break;
    s.win_events.destroy(h);
  }
  for (;;) {
    DeferPos* d = nullptr;
    for (DeferPos& x : s.defers.items()) if ((x.thread & 0xFFFFF) == pid) { d = &x; break; }
    if (!d) break;
    s.defers.destroy(d);
  }
  for (;;) {
    ObjHandle* h = nullptr;
    for (ObjHandle& x : s.object_handles.items()) if (x.pid == pid) { h = &x; break; }
    if (!h) break;
    if (h->desktop && h->desktop->refs) --h->desktop->refs;
    if (h->station && h->station->refs) --h->station->refs;
    s.object_handles.destroy(h);
  }
  for (uint64_t i = 0; i < s.thread_desktops.count();) {
    if ((s.thread_desktops[i].thread & 0xFFFFF) == pid) s.thread_desktops.remove_at(i); else ++i;
  }
  drop_process_classes(pid);
  for (ProcessInfo& p : s.processes) if (p.pid == pid) { s.processes.erase(&p); break; }
  if ((s.foreground_thread & 0xFFFFF) == pid) s.foreground_thread = 0;
}

void install_user_services() {
  register_class_services();
  register_window_services();
  register_geometry_services();
  register_paint_services();
  register_message_services();
  register_input_services();
  register_hook_services();
  register_station_services();
}

}  // namespace process::win32k::user

namespace process::win32k {

void install_user() {
  user::install_user_services();
  register_process_exit_hook(user::on_process_exit_hook);
}

}  // namespace process::win32k
