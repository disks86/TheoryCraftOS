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
// Tests of the window manager (win32k USER core): classes, windows and the tree, geometry, z-order, painting state, message queues (posting, retrieval, filters, timers,
// blocking waits, sends between threads and processes with the upcall protocol), hit testing, input injection, hooks, stations. A Th plays the user-mode half of win32u.dll
// (src/Windows/win32u/win32u.c): it calls a service and, while the service answers "upcall due", runs the pending window-procedure calls and resumes.
#include <atomic>
#include <functional>
#include <thread>
#include "Win32kTestSupport.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Process/Win32k/Win32kDesktopBridge.hpp"
#include "Process/Win32k/Win32kGdi.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Process/Win32k/Win32kUserInternal.hpp"
#include "Process/Win32k/Win32kUserMisc.hpp"

using namespace w32test;
using namespace process;
using namespace process::win32k;
using N = WindowsSyscallNumber;

namespace {

constexpr uint64_t kDue = 0x7FFFFFF1u, kDrainRetry = 0x7FFFFFF3u;

// What a window procedure gets called with.
struct Call_ {
  uint32_t hwnd, msg;
  uint64_t wparam, lparam;
  std::vector<uint8_t> payload;
};

void wr32(std::vector<uint8_t>& v, uint32_t o, uint32_t x) { for (int i = 0; i < 4; ++i) v[o + i] = uint8_t(x >> (8 * i)); }
struct Rc { int32_t l, t, r, b; };
uint32_t rd(const std::vector<uint8_t>& v, uint32_t o) { return v.size() >= o + 4 ? uint32_t(v[o]) | uint32_t(v[o + 1]) << 8 | uint32_t(v[o + 2]) << 16 | uint32_t(v[o + 3]) << 24 : 0xDEADBEEFu; }

// One thread of a program, with the user-mode half of the upcall protocol.
struct Th {
  Program* prog;
  uint64_t tid;
  uint64_t base;      // scratch memory of this thread: kScratch + base .. +0x8000
  std::vector<Call_> log;
  std::function<uint64_t(Th&, Call_&)> proc;
  int nested = 0;

  Th(Program& p, uint64_t t, uint64_t scratch) : prog(&p), tid(t), base(scratch) {}
  uint64_t rec() const { return base; }
  uint64_t tmp(uint64_t off = 0) const { return base + 0x2000 + off; }
  FakeMemory& mem() { return prog->memory; }

  void enter() { user::set_test_thread(tid); }

  void drain() {
    for (;;) {
      enter();
      const uint64_t buf = base + 0x4000 + 0x1100 * uint64_t(nested);      // (a record for each level of nesting)
      if (uint32_t(call1(N::NtUserTcosGetUpcall, buf)) == 0) break;
      const uint64_t id = mem().get64(buf);
      const uint32_t flags = mem().get32(buf + 12);
      Call_ c;
      c.hwnd = uint32_t(mem().get64(buf + 24));
      c.msg = mem().get32(buf + 32);
      const uint32_t plen = mem().get32(buf + 36);
      c.wparam = mem().get64(buf + 40);
      c.lparam = mem().get64(buf + 48);
      const uint16_t fix_at = uint16_t(mem().get16(buf + 80)), fix_to = uint16_t(mem().get16(buf + 82));
      for (uint32_t i = 0; i < plen; ++i) c.payload.push_back(mem().get8(buf + 88 + i));
      if (flags & 2) {      // lparam points to a copy of the structure
        if (fix_at != 0xFFFF) mem().put64(buf + 88 + fix_at, buf + 88 + fix_to);
        c.lparam = buf + 88;
      }
      log.push_back(c);
      const size_t index = log.size() - 1;
      uint64_t result = 0;
      ++nested;
      if (proc) result = proc(*this, c);
      --nested;
      for (uint32_t i = 0; i < plen; ++i) log[index].payload[i] = mem().get8(buf + 88 + i);
      enter();
      call1(N::NtUserTcosUpcallDone, id, result, buf + 88, plen);
    }
  }

  // A win32k call with up to 24 arguments (the first four in registers, the others on the stack past the home space).
  uint64_t call_raw(N n, const uint64_t (&a)[24]) {
    SyscallRequest r;
    r.number = uint64_t(n);
    for (int i = 0; i < 4; ++i) r.args[i] = a[i];
    for (int i = 4; i < 24; ++i) mem().put64(kStack + 0x28 + 8 * (i - 4), a[i]);
    r.user_stack = kStack;
    return prog->iface->dispatch(prog->process, r);
  }
  uint64_t call1(N n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    const uint64_t a[24] = {a0, a1, a2, a3};
    return call_raw(n, a);
  }

  uint64_t svc(N n, std::initializer_list<uint64_t> list = {}) {
    uint64_t a[24] = {};
    size_t i = 0;
    for (uint64_t v : list) a[i++] = v;
    auto raw = [&]() {
      enter();
      return call_raw(n, a);
    };
    uint64_t r = raw();
    for (;;) {
      if (r == kDue) { drain(); enter(); r = call1(N::NtUserTcosResume); }
      else if (r == kDrainRetry) { drain(); r = raw(); }
      else break;
    }
    return r;
  }
  uint32_t svc32(N n, std::initializer_list<uint64_t> list = {}) { return uint32_t(svc(n, list)); }

  // --- helpers
  uint32_t atom_class(const std::string& name, uint32_t style = 0, uint64_t wndproc = 0x401000, int cls_extra = 0, int wnd_extra = 0, uint64_t instance = 0x400000) {
    const uint64_t wc = tmp(0x100), us = tmp(0x200), chars = tmp(0x240);
    for (uint64_t i = 0; i < 80; ++i) mem().put8(wc + i, 0);
    mem().put32(wc, 80);
    mem().put32(wc + 4, style);
    mem().put64(wc + 8, wndproc);
    mem().put32(wc + 16, cls_extra);
    mem().put32(wc + 20, wnd_extra);
    mem().put64(wc + 24, instance);
    mem().put_unicode_string(us, chars, name);
    return svc32(N::NtUserRegisterClassExWOW, {wc, us, 0, 0, 0, 0, 0});
  }
  uint32_t create(const std::string& cls, uint32_t style, int x, int y, int cx, int cy, uint32_t parent = 0, uint32_t ex = 0, const std::string& title = "", uint64_t params = 0, uint64_t instance = 0x400000) {
    const uint64_t cus = tmp(0x400), cch = tmp(0x440), tus = tmp(0x600), tch = tmp(0x640);
    mem().put_unicode_string(cus, cch, cls);
    mem().put_unicode_string(tus, tch, title);
    return svc32(N::NtUserCreateWindowEx, {ex, cus, 0, tus, style, uint64_t(uint32_t(x)), uint64_t(uint32_t(y)), uint64_t(uint32_t(cx)), uint64_t(uint32_t(cy)), parent, 0, instance, params, 0, instance, cch, 0});
  }
  Rc rect(uint64_t addr) { return Rc{int32_t(mem().get32(addr)), int32_t(mem().get32(addr + 4)), int32_t(mem().get32(addr + 8)), int32_t(mem().get32(addr + 12))}; }
  void put_rect(uint64_t addr, Rc r) { mem().put32(addr, uint32_t(r.l)); mem().put32(addr + 4, uint32_t(r.t)); mem().put32(addr + 8, uint32_t(r.r)); mem().put32(addr + 12, uint32_t(r.b)); }
  Rc window_rect(uint32_t hwnd) {
    const uint64_t params = tmp(0x900), r = tmp(0x920);
    mem().put64(params, r);
    mem().put32(params + 8, 96);
    svc(N::NtUserCallHwndParam, {hwnd, params, 13});
    return rect(r);
  }
  Rc client_rect(uint32_t hwnd) {
    const uint64_t params = tmp(0x900), r = tmp(0x920);
    mem().put64(params, r);
    mem().put32(params + 8, 96);
    svc(N::NtUserCallHwndParam, {hwnd, params, 14});
    return rect(r);
  }
  bool show(uint32_t hwnd, uint32_t cmd) { return svc32(N::NtUserShowWindow, {hwnd, cmd}) != 0; }
  uint32_t style(uint32_t hwnd) { return uint32_t(svc(N::NtUserCallHwndParam, {hwnd, uint64_t(uint32_t(-16)), 10})); }
  bool is_window(uint32_t h) { return svc32(N::NtUserCallHwnd, {h, 10}) != 0; }
  uint32_t get_window(uint32_t h, uint32_t rel) { return uint32_t(svc(N::NtUserCallHwndParam, {h, rel, 16})); }
  uint32_t parent_of(uint32_t h) { return uint32_t(svc(N::NtUserCallHwnd, {h, 5})); }
  bool post(uint32_t h, uint32_t msg, uint64_t w = 0, uint64_t l = 0) { return svc32(N::NtUserPostMessage, {h, msg, w, l}) != 0; }
  // PeekMessage with PM_REMOVE; returns true and fills msg fields
  struct M { uint32_t hwnd = 0, message = 0; uint64_t wparam = 0, lparam = 0; uint32_t time = 0; int32_t x = 0, y = 0; };
  bool peek(M* m, uint32_t hwnd = 0, uint32_t first = 0, uint32_t last = 0, uint32_t flags = 1) {
    const uint64_t buf = tmp(0xA00);
    const bool got = svc32(N::NtUserPeekMessage, {buf, hwnd, first, last, flags}) != 0;
    if (got) read_msg(buf, m);
    return got;
  }
  void read_msg(uint64_t buf, M* m) {
    m->hwnd = uint32_t(mem().get64(buf));
    m->message = mem().get32(buf + 8);
    m->wparam = mem().get64(buf + 16);
    m->lparam = mem().get64(buf + 24);
    m->time = mem().get32(buf + 32);
    m->x = int32_t(mem().get32(buf + 36));
    m->y = int32_t(mem().get32(buf + 40));
  }
  int get_message(M* m, uint32_t hwnd = 0, uint32_t first = 0, uint32_t last = 0) {
    const uint64_t buf = tmp(0xA00);
    const int32_t r = int32_t(svc32(N::NtUserGetMessage, {buf, hwnd, first, last}));
    if (r >= 0) read_msg(buf, m);
    return r;
  }
  bool pk(M* m) { return peek(m, 0, 0x100, 0x3FF, 1); }
  void clear_log() { log.clear(); }
  std::vector<uint32_t> msgs() { std::vector<uint32_t> v; for (const Call_& c : log) v.push_back(c.msg); return v; }
  bool saw(uint32_t msg) { for (const Call_& c : log) if (c.msg == msg) return true; return false; }
  int count_msg(uint32_t msg) { int n = 0; for (const Call_& c : log) if (c.msg == msg) ++n; return n; }
};

constexpr uint32_t WS_OVERLAPPED = 0, WS_POPUP = 0x80000000u, WS_CHILD = 0x40000000u, WS_VISIBLE = 0x10000000u, WS_DISABLED = 0x08000000u, WS_CLIPCHILDREN = 0x02000000u,
                   WS_CLIPSIBLINGS = 0x04000000u, WS_MINIMIZE = 0x20000000u, WS_MAXIMIZE = 0x01000000u, WS_CAPTION = 0xC00000u, WS_BORDER = 0x800000u,
                   WS_THICKFRAME = 0x40000u, WS_SYSMENU = 0x80000u, WS_OVERLAPPEDWINDOW = 0xCF0000u;
constexpr uint32_t WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_ENABLE = 0xA, WM_PAINT = 0xF, WM_CLOSE = 0x10,
                   WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCACTIVATE = 0x86, WM_USER = 0x400,
                   WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200,
                   WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_HOTKEY = 0x312, WM_NCMOUSEMOVE = 0xA0, WM_PARENTNOTIFY = 0x210,
                   WM_SETTEXT = 0xC, WM_GETTEXT = 0xD, WM_COPYDATA = 0x4A, WM_MOUSEWHEEL = 0x20A, WM_SYSKEYDOWN = 0x104, WM_STYLECHANGED = 0x7D;

uint64_t default_proc(Th& t, Call_& c) {
  if (c.msg == WM_NCCREATE) return 1;
  if (c.msg == WM_NCCALCSIZE) return t.svc(N::NtUserMessageCall, {c.hwnd, c.msg, c.wparam, c.lparam, 0, 0x29e, 0});      // DefWindowProc computes the client area
  return 0;
}
// everything goes to DefWindowProc
uint64_t def_proc(Th& t, Call_& c) { return t.svc(N::NtUserMessageCall, {c.hwnd, c.msg, c.wparam, c.lparam, 0, 0x29e, 0}); }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
void test_classes() {
  Program prog(21);
  Th t(prog, 21, kScratch);
  t.proc = default_proc;
  CHECK(t.atom_class("MyClass", 0, 0x401000, 8, 16) >= 0xC000);
  CHECK(t.atom_class("MyClass") == 0);      // already registered
  // error codes
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("MyClass");
  CHECK(prog.memory.get32(kScratch + 0x7000 + 0x68) == 1410);
  // other processes' classes with the same name are separate
  Program other(22);
  Th o(other, 22, kScratch);
  CHECK(o.atom_class("MyClass") >= 0xC000);
  // class info
  const uint64_t us = t.tmp(0x200), chars = t.tmp(0x240), wc = t.tmp(0x300);
  prog.memory.put_unicode_string(us, chars, "MyClass");
  prog.memory.put32(wc, 80);
  CHECK(t.svc32(N::NtUserGetClassInfoEx, {0x400000, us, wc, 0, 0}) >= 0xC000);
  CHECK(prog.memory.get32(wc + 16) == 8 && prog.memory.get32(wc + 20) == 16 && prog.memory.get64(wc + 8) == 0x401000);
  prog.memory.put_unicode_string(us, chars, "Nope");
  CHECK(t.svc32(N::NtUserGetClassInfoEx, {0x400000, us, wc, 0, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1411);
  // a window of the class: name, long values, class words
  const uint32_t w = t.create("MyClass", WS_OVERLAPPED, 10, 20, 100, 80);
  CHECK(w != 0 && t.is_window(w));
  const uint64_t name = t.tmp(0x800);
  prog.memory.put16(name + 2, 64);
  prog.memory.put64(name + 8, t.tmp(0x880));
  CHECK(t.svc32(N::NtUserGetClassName, {w, 1, name}) == 7 && prog.memory.get_wide_z(t.tmp(0x880)) == "MyClass");
  CHECK(t.svc(N::NtUserSetClassLongPtr, {w, 0, 0x1234, 0}) == 0);      // class extra bytes start at zero
  CHECK(t.svc(N::NtUserCallHwndParam, {w, 0, 4}) == 0x1234);          // GetClassLongPtr
  CHECK(t.svc(N::NtUserSetClassLongPtr, {w, 0, 0x99, 0}) == 0x1234);
  CHECK(t.svc(N::NtUserCallHwndParam, {w, uint64_t(int64_t(-26)), 3}) == 0);      // GCL_STYLE
  CHECK(t.svc(N::NtUserSetClassLong, {w, uint64_t(int64_t(-26)), 0x20, 0}) == 0 && t.svc(N::NtUserCallHwndParam, {w, uint64_t(int64_t(-26)), 3}) == 0x20);
  CHECK(t.svc32(N::NtUserSetClassLong, {w, 100, 1, 0}) == 0);      // past the extra bytes
  CHECK(prog.memory.get32(kScratch + 0x7000 + 0x68) == 1413);
  // a class with windows cannot go
  prog.memory.put_unicode_string(us, chars, "MyClass");
  CHECK(t.svc32(N::NtUserUnregisterClass, {us, 0x400000, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1412);
  CHECK(t.svc32(N::NtUserDestroyWindow, {w}) == 1);
  CHECK(t.svc32(N::NtUserUnregisterClass, {us, 0x400000, 0}) == 1);
  CHECK(t.svc32(N::NtUserUnregisterClass, {us, 0x400000, 0}) == 0);
  // window messages and atoms
  prog.memory.put_unicode_string(us, chars, "MyPrivateMessage");
  const uint32_t m1 = t.svc32(N::NtUserRegisterWindowMessage, {us});
  CHECK(m1 >= 0xC000 && t.svc32(N::NtUserRegisterWindowMessage, {us}) == m1);
  prog.memory.put_unicode_string(us, chars, "myprivatemessage");
  CHECK(t.svc32(N::NtUserRegisterWindowMessage, {us}) == m1);      // case does not matter
  // bad pointers
  CHECK(t.svc32(N::NtUserRegisterClassExWOW, {0x10, us, 0, 0, 0, 0, 0}) == 0);
  CHECK(t.svc32(N::NtUserRegisterWindowMessage, {0x10}) == 0);
  user::reset_state();
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
void test_create_and_destroy() {
  Program prog(31);
  Th t(prog, 31, kScratch);
  t.proc = default_proc;
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("Top", 0, 0x401000, 0, 8);
  // an invisible overlapped window: GETMINMAXINFO, NCCREATE, NCCALCSIZE, CREATE
  const uint32_t w = t.create("Top", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, 0, 0, "Title", 0x55AA);
  CHECK(w != 0 && t.is_window(w));
  CHECK((t.msgs() == std::vector<uint32_t>{0x24, WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE}));
  // the creation structure: lpCreateParams, geometry and styles
  CHECK(rd(t.log[1].payload, 0) == 0x55AA && rd(t.log[1].payload, 32) == 200 && rd(t.log[1].payload, 36) == 300 && rd(t.log[1].payload, 40) == 20 && rd(t.log[1].payload, 44) == 10);
  CHECK(rd(t.log[1].payload, 48) == WS_OVERLAPPEDWINDOW);
  // NCCALCSIZE came with the window rectangle in rgrc[0]
  CHECK(rd(t.log[2].payload, 16) == 10 && rd(t.log[2].payload, 20) == 20 && rd(t.log[2].payload, 24) == 310 && rd(t.log[2].payload, 28) == 220 && t.log[2].wparam == 1);
  // geometry: window rect and the client rectangle inside the frame (Windows metrics for a program that is not DPI aware: a 4 px frame, 27 px of caption and frame on top)
  Rc wr = t.window_rect(w), cr = t.client_rect(w);
  CHECK(wr.l == 10 && wr.t == 20 && wr.r == 310 && wr.b == 220);
  CHECK(cr.l == 0 && cr.t == 0 && cr.r == 300 - 8 && cr.b == 200 - 4 - 27);
  CHECK(!(t.style(w) & WS_VISIBLE));
  CHECK(t.svc32(N::NtUserCallHwnd, {w, 13}) == 0);      // not visible
  // a visible one
  t.clear_log();
  const uint32_t v = t.create("Top", WS_POPUP | WS_VISIBLE, 0, 0, 50, 50);
  CHECK(v != 0 && t.svc32(N::NtUserCallHwnd, {v, 13}) == 1);
  CHECK(t.saw(WM_SHOWWINDOW) && t.saw(WM_WINDOWPOSCHANGING) && t.saw(WM_WINDOWPOSCHANGED));
  CHECK(t.count_msg(WM_ACTIVATE) == 1);      // the first visible top-level window of the thread is activated
  CHECK(t.svc32(N::NtUserGetThreadState, {1}) == v);
  // WM_NCCREATE answering FALSE fails the creation: WM_NCDESTROY, no window
  t.clear_log();
  t.proc = [](Th&, Call_& c) -> uint64_t { return c.msg == WM_NCCREATE ? 0 : 0; };
  CHECK(t.create("Top", 0, 0, 0, 10, 10) == 0);
  CHECK((t.msgs() == std::vector<uint32_t>{0x24, WM_NCCREATE, WM_NCDESTROY}));
  // WM_CREATE answering -1: WM_DESTROY and WM_NCDESTROY
  t.clear_log();
  t.proc = [](Th&, Call_& c) -> uint64_t { return c.msg == WM_NCCREATE ? 1 : c.msg == WM_CREATE ? uint64_t(-1) : 0; };
  CHECK(t.create("Top", 0, 0, 0, 10, 10) == 0);
  CHECK((t.msgs() == std::vector<uint32_t>{0x24, WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE, WM_DESTROY, WM_NCDESTROY}));
  t.proc = default_proc;
  // unknown class, bad parent
  CHECK(t.create("NoSuchClass", 0, 0, 0, 10, 10) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1407);
  CHECK(t.create("Top", WS_CHILD, 0, 0, 10, 10, 0) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1406);
  CHECK(t.create("Top", WS_CHILD, 0, 0, 10, 10, 0x99999) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1400);

  // children, siblings and the tree
  const uint32_t p = t.create("Top", WS_POPUP, 0, 0, 200, 200);      // (a popup: no caption or frame moves the children)
  const uint32_t c1 = t.create("Top", WS_CHILD | WS_VISIBLE, 5, 5, 50, 50, p);
  const uint32_t c2 = t.create("Top", WS_CHILD | WS_VISIBLE, 60, 5, 50, 50, p);
  const uint32_t g = t.create("Top", WS_CHILD | WS_VISIBLE, 1, 1, 10, 10, c1);
  CHECK(c1 && c2 && g);
  CHECK(t.parent_of(c1) == p && t.parent_of(g) == c1 && t.parent_of(p) == 0);
  CHECK(t.get_window(p, 5) == c1);      // GW_CHILD: the first child is the topmost (a new child goes below the others)
  CHECK(t.get_window(c1, 2) == c2 && t.get_window(c2, 3) == c1);      // GW_HWNDNEXT / GW_HWNDPREV
  CHECK(t.get_window(c2, 0) == c1 && t.get_window(c1, 1) == c2);      // FIRST / LAST
  CHECK(t.svc32(N::NtUserCallHwndParam, {p, g, 19}) == 1 && t.svc32(N::NtUserCallHwndParam, {c1, c2, 19}) == 0);      // IsChild
  CHECK(t.svc32(N::NtUserGetAncestor, {g, 2}) == p && t.svc32(N::NtUserGetAncestor, {g, 1}) == c1);
  // screen coordinates of nested children
  Rc gr = t.window_rect(g);
  CHECK(gr.l == 5 + 1 && gr.t == 5 + 1 && gr.r == 5 + 11);
  CHECK(t.svc32(N::NtUserWindowFromPoint, {6, 6}) == 0 || true);
  // FindWindowEx and the window list
  const uint64_t us = t.tmp(0x200), chars = t.tmp(0x240);
  prog.memory.put_unicode_string(us, chars, "Top");
  CHECK(t.svc32(N::NtUserFindWindowEx, {p, 0, us, 0, 0}) == c1);
  CHECK(t.svc32(N::NtUserFindWindowEx, {p, c1, us, 0, 0}) == c2);
  CHECK(t.svc32(N::NtUserFindWindowEx, {p, c2, us, 0, 0}) == 0);
  const uint64_t list = t.tmp(0x1000), size = t.tmp(0xFF0);
  CHECK(t.svc32(N::NtUserBuildHwndList, {0, p, 1, 0, 0, 2, list, size}) == 0xC0000023u && prog.memory.get32(size) == 3);      // all descendants: 3
  CHECK(t.svc32(N::NtUserBuildHwndList, {0, p, 1, 0, 0, 8, list, size}) == 0 && prog.memory.get32(size) == 3);
  CHECK(prog.memory.get64(list) == c2 || prog.memory.get64(list) == c1);
  // DestroyWindow: parent first, children below it, in order; WM_NCDESTROY from the leaves up
  t.clear_log();
  CHECK(t.svc32(N::NtUserDestroyWindow, {p}) == 1);
  CHECK(!t.is_window(p) && !t.is_window(c1) && !t.is_window(c2) && !t.is_window(g));
  std::vector<uint32_t> order;
  for (const Call_& cl : t.log) if (cl.msg == WM_DESTROY || cl.msg == WM_NCDESTROY) order.push_back(cl.msg);
  CHECK(order.size() == 8 && order[0] == WM_DESTROY);
  // every WM_NCDESTROY comes after the WM_DESTROY of its own window, the parent's last
  CHECK(t.log.back().msg == WM_NCDESTROY && t.log.back().hwnd == p);
  CHECK(t.svc32(N::NtUserDestroyWindow, {p}) == 0);      // gone
  // SetParent: the window moves, the previous parent is the answer; cycles are refused
  const uint32_t a = t.create("Top", WS_OVERLAPPED, 0, 0, 100, 100), b = t.create("Top", WS_CHILD, 3, 4, 20, 20, a), d = t.create("Top", WS_OVERLAPPED, 300, 300, 50, 50);
  const uint32_t desktop = uint32_t(t.svc(N::NtUserCallHwnd, {a, 5}) ? 0 : 0);
  (void)desktop;
  CHECK(t.svc32(N::NtUserSetParent, {b, d}) == a);
  CHECK(t.parent_of(b) == d && t.get_window(a, 5) == 0 && t.get_window(d, 5) == b);
  CHECK(t.svc32(N::NtUserSetParent, {d, b}) == 0);      // d is b's parent: a cycle
  CHECK(t.svc32(N::NtUserSetParent, {b, b}) == 0);
  const uint32_t old = t.svc32(N::NtUserSetParent, {b, 0});      // to the desktop
  CHECK(old == d && t.parent_of(b) == user::desktop_window());      // (a child of the desktop has the desktop as its parent)
  CHECK(t.svc32(N::NtUserSetParent, {a, 0}) == (uint32_t)user::desktop_window());      // a was a top-level window: its parent was the desktop
  user::reset_state();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint64_t big(int64_t v) { return uint64_t(v); }
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void test_queue_basics() {
  Program prog(41);
  Th t(prog, 41, kScratch);
  t.proc = default_proc;
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("Q");
  const uint32_t w = t.create("Q", WS_OVERLAPPED, 0, 0, 50, 50), w2 = t.create("Q", WS_OVERLAPPED, 0, 0, 50, 50);
  CHECK(t.post(w, WM_USER, 1, 2) && t.post(w2, WM_USER + 1, 3, 4) && t.post(w, WM_USER + 2, 5, 6));
  CHECK(t.svc32(N::NtUserPostThreadMessage, {41, WM_USER + 3, 7, 8}) == 1);
  Th::M m;
  CHECK(t.peek(&m, 0, 0, 0, 0) && m.message == WM_USER && m.hwnd == w && m.wparam == 1 && m.lparam == 2);      // PM_NOREMOVE
  CHECK(t.peek(&m, 0, 0, 0, 0) && m.message == WM_USER);
  CHECK(t.peek(&m, w2) && m.message == WM_USER + 1 && m.wparam == 3);      // window filter, removed
  CHECK(t.peek(&m, 0, WM_USER + 2, WM_USER + 2) && m.hwnd == w);      // range filter
  CHECK(t.svc32(N::NtUserPeekMessage, {t.tmp(0xA00), big(-1), 0, 0, 1}) == 1 && t.mem().get64(t.tmp(0xA00)) == 0 && t.mem().get32(t.tmp(0xA00) + 8) == WM_USER + 3);      // thread messages only
  CHECK(t.peek(&m) && m.message == WM_USER && !t.peek(&m));
  // queue status: nothing posted, then something
  CHECK((t.svc32(N::NtUserGetQueueStatus, {0x4FF}) & 8) == 0);
  t.post(w, WM_USER + 5);
  const uint32_t qs = t.svc32(N::NtUserGetQueueStatus, {0x4FF});
  CHECK((qs & 8) != 0 && (qs >> 16 & 8) != 0);
  CHECK(t.svc32(N::NtUserGetQueueStatus, {0x4FF}) >> 16 == 0);      // "changed" bits are read once
  // WM_QUIT comes after posted messages and carries the exit code
  CHECK(t.svc32(N::NtUserPostQuitMessage, {7}) == 1);
  CHECK(t.get_message(&m) == 1 && m.message == WM_USER + 5);
  CHECK(t.get_message(&m) == 0 && m.message == WM_QUIT && m.wparam == 7);
  CHECK(!t.peek(&m));      // the quit is consumed
  // posting to windows that do not exist, and messages that need a reply
  CHECK(!t.post(0x77777, WM_USER) && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1400);
  CHECK(!t.post(w, WM_COPYDATA) && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1159);
  CHECK(t.svc32(N::NtUserPostThreadMessage, {0x12345, WM_USER, 0, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1444);
  // timers: a thread timer arrives as WM_TIMER by its deadline
  const uint64_t id = t.svc(N::NtUserSetTimer, {0, 0, 20, 0, 0});
  CHECK(id != 0);
  CHECK(!t.peek(&m));      // not due yet
  CHECK(t.get_message(&m) == 1 && m.message == WM_TIMER && m.wparam == id && m.hwnd == 0);
  CHECK(t.svc32(N::NtUserKillTimer, {0, id}) == 1 && t.svc32(N::NtUserKillTimer, {0, id}) == 0);
  // a window timer with the id of the caller's choice, replaced by a second SetTimer, and removed with the window
  CHECK(t.svc(N::NtUserSetTimer, {w, 5, 10, 0, 0}) == 5 && t.svc(N::NtUserSetTimer, {w, 5, 10, 0, 0}) == 5);
  CHECK(t.get_message(&m) == 1 && m.message == WM_TIMER && m.hwnd == w && m.wparam == 5);
  CHECK(t.get_message(&m) == 1 && m.message == WM_TIMER);      // (it repeats)
  t.svc(N::NtUserDestroyWindow, {w});
  CHECK(!t.peek(&m, 0, WM_TIMER, WM_TIMER));
  // WM_PAINT comes when nothing else is queued
  t.show(w2, 5);
  CHECK(t.peek(&m, 0, WM_PAINT, WM_PAINT) && m.hwnd == w2);
  t.svc(N::NtUserValidateRect, {w2, 0});
  // a blocked GetMessage woken by a post from another thread
  std::thread poster([&] { sleep_ms(60); Th p2(prog, 41, kScratch + 0x20000); p2.post(w2, WM_USER + 9, 1, 1); });
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(t.get_message(&m) == 1 && m.message == WM_USER + 9);
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(40));
  poster.join();
  user::reset_state();
}

void test_send_same_thread() {
  Program prog(42);
  Th t(prog, 42, kScratch);
  t.proc = default_proc;
  t.atom_class("S");
  const uint32_t w = t.create("S", WS_OVERLAPPED, 0, 0, 50, 50);
  t.clear_log();
  t.proc = [](Th& th, Call_& c) -> uint64_t {
    if (c.msg == WM_USER) return c.wparam * 2 + c.lparam;
    if (c.msg == WM_USER + 1) return th.svc(N::NtUserMessageCall, {c.hwnd, WM_USER, c.wparam, 1, 0, 0x2b1, 0}) + 100;      // a nested send from inside the procedure
    return 0;
  };
  CHECK(t.svc(N::NtUserMessageCall, {w, WM_USER, 20, 2, 0, 0x2b1, 0}) == 42);
  CHECK(t.svc(N::NtUserMessageCall, {w, WM_USER + 1, 5, 0, 0, 0x2b1, 0}) == 111);
  CHECK(t.log.size() == 3 && t.log[1].msg == WM_USER + 1 && t.log[2].msg == WM_USER);
  // SendNotifyMessage to an own window behaves like SendMessage
  t.clear_log();
  CHECK(t.svc32(N::NtUserMessageCall, {w, WM_USER, 1, 1, 0, 0x2b7, 0}) == 1 && t.log.size() == 1);
  // SendMessageTimeout fills the result
  const uint64_t info = t.tmp(0xB00);
  t.mem().put32(info, 0); t.mem().put32(info + 4, 1000); t.mem().put64(info + 8, 0);
  CHECK(t.svc32(N::NtUserMessageCall, {w, WM_USER, 3, 3, info, 0x2b3, 0}) == 1 && t.mem().get64(info + 8) == 9);
  CHECK(t.svc(N::NtUserMessageCall, {0x4444, WM_USER, 0, 0, 0, 0x2b1, 0}) == 0);
  // DispatchMessage calls the window procedure; a WM_TIMER with a callback calls that
  t.clear_log();
  const uint64_t msg = t.tmp(0xC00);
  t.mem().put64(msg, w); t.mem().put32(msg + 8, WM_USER); t.mem().put64(msg + 16, 10); t.mem().put64(msg + 24, 1);
  CHECK(t.svc(N::NtUserDispatchMessage, {msg}) == 21 && t.log.size() == 1);
  t.mem().put32(msg + 8, WM_TIMER); t.mem().put64(msg + 16, 3); t.mem().put64(msg + 24, 0x500000);
  t.clear_log();
  t.svc(N::NtUserDispatchMessage, {msg});
  CHECK(t.log.size() == 1 && t.log[0].msg == WM_TIMER && t.log[0].wparam == 3);
  user::reset_state();
}

void test_send_between_threads() {
  Program pa(51), pb(52), pc(53);
  const uint64_t ta = (1ull << 20) | 51, tb = (1ull << 20) | 52, tc = (1ull << 20) | 53;
  Th a(pa, ta, kScratch), b(pb, tb, kScratch), c(pc, tc, kScratch);
  pa.process.set_gs_base(kScratch + 0x7000);
  b.proc = default_proc;
  c.proc = default_proc;
  a.proc = default_proc;
  b.atom_class("B");
  c.atom_class("C");
  const uint32_t wb = b.create("B", WS_OVERLAPPED, 0, 0, 50, 50), wc = c.create("C", WS_OVERLAPPED, 0, 0, 50, 50);
  std::atomic<bool> replied_proc_done{false};
  b.proc = [&](Th& th, Call_& cl) -> uint64_t {
    if (cl.msg == WM_USER) return cl.wparam + cl.lparam;
    if (cl.msg == WM_USER + 1) {
      CHECK(th.svc32(N::NtUserGetThreadState, {8}) == 1);      // InSendMessage
      th.svc(N::NtUserReplyMessage, {99});
      CHECK(th.svc32(N::NtUserGetThreadState, {8}) == 9);      // replied
      sleep_ms(200);
      replied_proc_done = true;
      return 5;
    }
    if (cl.msg == WM_COPYDATA) {
      CHECK(cl.payload.size() == 24 + 5 && rd(cl.payload, 8) == 5);
      CHECK(std::string(cl.payload.begin() + 24, cl.payload.end()) == "hello");
      CHECK(th.mem().get64(cl.lparam + 16) == cl.lparam + 24);      // the data pointer points into the copy
      return 1;
    }
    if (cl.msg == WM_SETTEXT) { CHECK(th.mem().get16(cl.lparam) == 'h'); return 1; }
    return 0;
  };
  std::thread bt([&] {
    Th::M m;
    while (b.get_message(&m) > 0 && m.message != WM_USER + 100) {}
  });
  sleep_ms(30);
  CHECK(a.svc(N::NtUserMessageCall, {wb, WM_USER, 40, 2, 0, 0x2b1, 0}) == 42);
  // ReplyMessage lets the sender go on before the procedure is done
  CHECK(a.svc(N::NtUserMessageCall, {wb, WM_USER + 1, 0, 0, 0, 0x2b1, 0}) == 99);
  CHECK(!replied_proc_done);
  // WM_COPYDATA across processes: the data is copied
  const uint64_t cds = a.tmp(0x100), data = a.tmp(0x200);
  pa.memory.put64(cds, 77); pa.memory.put32(cds + 8, 5); pa.memory.put64(cds + 16, data);
  for (int i = 0; i < 5; ++i) pa.memory.put8(data + i, "hello"[i]);
  CHECK(a.svc(N::NtUserMessageCall, {wb, WM_COPYDATA, 0, cds, 0, 0x2b1, 0}) == 1);
  pa.memory.put_wide(a.tmp(0x300), "hi");
  CHECK(a.svc(N::NtUserMessageCall, {wb, WM_SETTEXT, 0, a.tmp(0x300), 0, 0x2b1, 0}) == 1);
  // SendNotifyMessage does not wait
  CHECK(a.svc32(N::NtUserMessageCall, {wb, WM_USER, 1, 1, 0, 0x2b7, 0}) == 1);
  // a thread that is not pumping: the timeout ends the send
  const uint64_t info = a.tmp(0xB00);
  c.clear_log();
  pa.memory.put32(info, 0); pa.memory.put32(info + 4, 60); pa.memory.put64(info + 8, 7);
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(a.svc32(N::NtUserMessageCall, {wc, WM_USER, 1, 1, info, 0x2b3, 0}) == 0);
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(50));
  CHECK(pa.memory.get32(kScratch + 0x7000 + 0x68) == 1460);
  // the abandoned send is gone: c sees nothing to handle
  Th::M m;
  CHECK(!c.peek(&m) && c.log.empty());
  // the target dies while the sender waits
  std::atomic<uint64_t> result{12345};
  std::thread sender([&] { Th a2(pa, ta, kScratch + 0x20000); a2.proc = default_proc; result = a2.svc(N::NtUserMessageCall, {wc, WM_USER, 1, 1, 0, 0x2b1, 0}); });
  sleep_ms(80);
  CHECK(result == 12345);      // still waiting
  process_exited(53);
  sender.join();
  CHECK(result == 0);
  CHECK(!user::is_window(wc) && user::is_window(wb));
  // stop b's loop
  a.post(wb, WM_USER + 100);
  bt.join();
  CHECK(replied_proc_done);
  user::reset_state();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
using Rect = process::win32k::gdi::Rect;
int64_t area(const List<Rect>& r) {
  int64_t a = 0;
  for (const Rect& x : r) a += int64_t(x.right - x.left) * (x.bottom - x.top);
  return a;
}

void test_geometry_and_zorder() {
  Program prog(61);
  Th t(prog, 61, kScratch);
  t.proc = def_proc;
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("G", 0x8 /* CS_DBLCLKS */);
  // popups have no frame: client = window
  const uint32_t a = t.create("G", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100), b = t.create("G", WS_POPUP | WS_VISIBLE, 50, 50, 100, 100);
  CHECK(a && b);
  CHECK(t.get_window(a, 0) == b && t.get_window(b, 3) == 0 && t.get_window(a, 3) == b);      // b was made last: it is on top
  CHECK(t.svc32(N::NtUserWindowFromPoint, {60, 60}) == b && t.svc32(N::NtUserWindowFromPoint, {10, 10}) == a);
  CHECK(t.svc32(N::NtUserWindowFromPoint, {500, 500}) == user::desktop_window());
  CHECK(user::window_from_screen_point(60, 60) == b && user::is_window(a) && user::window_thread(a) == 61 && user::window_process(b) == 61);
  gdi::Rect r;
  CHECK(user::window_screen_rect(b, &r) && r.left == 50 && r.bottom == 150 && user::window_client_screen_rect(b, &r) && r.right == 150 && user::window_visible(b));
  // the visible region of a: the L left after b covers a corner (the lock is not recursive: take it as a handler would)
  {
    ObjectManager* objects = system_objects();
    objects->sync().lock();
    List<Rect> vis(*system_allocator());
    CHECK(user::visible_region(user::find_window(a), false, false, true, &vis) && area(vis) == 100 * 100 - 50 * 50);
    CHECK(user::visible_region(user::find_window(b), false, false, true, &vis) && area(vis) == 100 * 100);
    objects->sync().unlock();
  }
  // z-order: SetWindowPos with HWND_TOP; topmost windows stay above later ones
  CHECK(t.svc32(N::NtUserSetWindowPos, {a, 0, 0, 0, 0, 0, 3}) == 1);      // SWP_NOSIZE | SWP_NOMOVE
  CHECK(t.get_window(a, 0) == a && t.svc32(N::NtUserWindowFromPoint, {60, 60}) == a);
  CHECK(t.svc32(N::NtUserSetWindowPos, {b, big(-1), 0, 0, 0, 0, 3}) == 1);      // HWND_TOPMOST
  CHECK((t.svc32(N::NtUserCallHwndParam, {b, big(-20), 10}) & 8) != 0 && t.get_window(a, 0) == b);
  const uint32_t c = t.create("G", WS_POPUP | WS_VISIBLE, 70, 70, 10, 10);
  CHECK(t.get_window(c, 0) == b && t.get_window(b, 2) == c);      // below the topmost window, above the others
  CHECK(t.svc32(N::NtUserSetWindowPos, {b, big(-2), 0, 0, 0, 0, 3}) == 1 && (t.svc32(N::NtUserCallHwndParam, {b, big(-20), 10}) & 8) == 0);      // HWND_NOTOPMOST
  CHECK(t.svc32(N::NtUserSetWindowPos, {c, 1, 0, 0, 0, 0, 3}) == 1 && t.get_window(c, 1) == c);      // HWND_BOTTOM
  CHECK(t.svc32(N::NtUserSetWindowPos, {a, 0x6666, 0, 0, 0, 0, 3}) == 0);      // not a window
  // move and size: messages, geometry, WM_MOVE/WM_SIZE from DefWindowProc
  t.clear_log();
  CHECK(t.svc32(N::NtUserMoveWindow, {a, 200, 100, 300, 150, 1}) == 1);
  Rc wr = t.window_rect(a), cr = t.client_rect(a);
  CHECK(wr.l == 200 && wr.t == 100 && wr.r == 500 && wr.b == 250 && cr.r == 300 && cr.b == 150);
  CHECK((t.msgs() == std::vector<uint32_t>{WM_WINDOWPOSCHANGING, WM_NCCALCSIZE, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE}));
  for (const Call_& cl : t.log) if (cl.msg == WM_SIZE) CHECK(cl.wparam == 0 && cl.lparam == (150u << 16 | 300u));
  // a procedure may change the proposed position in WM_WINDOWPOSCHANGING
  t.proc = [](Th& th, Call_& cl) -> uint64_t {
    if (cl.msg == WM_WINDOWPOSCHANGING) { th.mem().put32(cl.lparam + 16, 7); th.mem().put32(cl.lparam + 20, 9); return 0; }
    return def_proc(th, cl);
  };
  t.svc(N::NtUserMoveWindow, {a, 300, 300, 50, 50, 1});
  wr = t.window_rect(a);
  CHECK(wr.l == 7 && wr.t == 9 && wr.r == 57);
  t.proc = def_proc;
  // maximize, minimize and restore
  const uint32_t f = t.create("G", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 50, 300, 200);
  CHECK(t.show(f, 3) == true);      // SW_SHOWMAXIMIZED: it was visible
  CHECK((t.style(f) & WS_MAXIMIZE) && t.window_rect(f).l == -4 && t.window_rect(f).r == 1024 + 4);
  const uint64_t pl = t.tmp(0xD00);
  prog.memory.put32(pl, 44);
  CHECK(t.svc32(N::NtUserGetWindowPlacement, {f, pl}) == 1 && prog.memory.get32(pl + 8) == 3 && prog.memory.get32(pl + 28) == 40 && prog.memory.get32(pl + 36) == 340);
  CHECK(t.show(f, 6) == true);      // SW_MINIMIZE
  CHECK((t.style(f) & WS_MINIMIZE) && !(t.style(f) & WS_MAXIMIZE));
  t.svc32(N::NtUserGetWindowPlacement, {f, pl});
  CHECK(prog.memory.get32(pl + 8) == 2 && (prog.memory.get32(pl + 4) & 2) != 0);      // minimized from maximized: restores to maximized
  t.show(f, 9);
  CHECK((t.style(f) & WS_MAXIMIZE) && !(t.style(f) & WS_MINIMIZE));
  t.show(f, 9);
  CHECK(!(t.style(f) & WS_MAXIMIZE) && t.window_rect(f).l == 40 && t.window_rect(f).t == 50 && t.window_rect(f).r == 340);
  // hide and show: the answer is the previous visibility, WM_SHOWWINDOW goes out on changes
  t.clear_log();
  CHECK(t.show(f, 0) == true && !(t.style(f) & WS_VISIBLE) && t.saw(WM_SHOWWINDOW));
  CHECK(t.show(f, 0) == false);
  t.clear_log();
  CHECK(t.show(f, 5) == false && (t.style(f) & WS_VISIBLE) && t.log[0].msg == WM_SHOWWINDOW || t.saw(WM_SHOWWINDOW));
  CHECK(t.show(f, 99) == false);
  // deferred positioning applies every entry
  const uint32_t h = t.svc32(N::NtUserBeginDeferWindowPos, {2});
  CHECK(h != 0);
  CHECK(t.svc32(N::NtUserDeferWindowPosAndBand, {h, c, 0, 5, 6, 20, 21, 4, 0, 0}) == h);
  CHECK(t.svc32(N::NtUserDeferWindowPosAndBand, {h, a, 0, 15, 16, 30, 31, 4, 0, 0}) == h);
  CHECK(t.svc32(N::NtUserEndDeferWindowPosEx, {h, 0}) == 1);
  CHECK(t.window_rect(c).l == 5 && t.window_rect(c).b == 27 && t.window_rect(a).l == 15 && t.window_rect(a).r == 45);
  CHECK(t.svc32(N::NtUserEndDeferWindowPosEx, {h, 0}) == 0);      // the handle is spent
  // children: coordinates relative to the parent's client area, hit testing, ChildWindowFromPoint
  const uint32_t p = t.create("G", WS_POPUP | WS_VISIBLE, 300, 300, 200, 200);
  const uint32_t k = t.create("G", WS_CHILD | WS_VISIBLE, 20, 30, 50, 50, p);
  CHECK(t.window_rect(k).l == 320 && t.window_rect(k).t == 330);
  CHECK(t.svc32(N::NtUserWindowFromPoint, {330, 340}) == k && t.svc32(N::NtUserWindowFromPoint, {310, 310}) == p);
  CHECK(t.svc32(N::NtUserChildWindowFromPointEx, {p, 25, 35, 0}) == k && t.svc32(N::NtUserChildWindowFromPointEx, {p, 5, 5, 0}) == p && t.svc32(N::NtUserChildWindowFromPointEx, {p, 999, 5, 0}) == 0);
  t.show(k, 0);
  CHECK(t.svc32(N::NtUserWindowFromPoint, {330, 340}) == p && t.svc32(N::NtUserChildWindowFromPointEx, {p, 25, 35, 1}) == p && t.svc32(N::NtUserChildWindowFromPointEx, {p, 25, 35, 0}) == k);
  // moving the parent moves the child
  t.svc(N::NtUserSetWindowPos, {p, 0, 400, 400, 0, 0, 1 | 4});
  CHECK(t.window_rect(k).l == 420 && t.window_rect(k).t == 430);
  // CallHwndParam helpers: ClientToScreen, ScreenToClient, MapWindowPoints
  const uint64_t pt = t.tmp(0xE00);
  prog.memory.put32(pt, 5); prog.memory.put32(pt + 4, 6);
  CHECK(t.svc32(N::NtUserCallHwndParam, {k, pt, 0}) == 1 && prog.memory.get32(pt) == 425 && prog.memory.get32(pt + 4) == 436);
  CHECK(t.svc32(N::NtUserCallHwndParam, {k, pt, 23}) == 1 && prog.memory.get32(pt) == 5);
  const uint64_t mp = t.tmp(0xE40);
  prog.memory.put64(mp, p); prog.memory.put64(mp + 8, pt); prog.memory.put32(mp + 16, 1);
  prog.memory.put32(pt, 0); prog.memory.put32(pt + 4, 0);
  CHECK(t.svc32(N::NtUserCallHwndParam, {k, mp, 20}) == (uint32_t)(((uint16_t)30 << 16) | (uint16_t)20) && prog.memory.get32(pt) == 20 && prog.memory.get32(pt + 4) == 30);
  // GetWindowInfo
  const uint64_t wi = t.tmp(0xF00);
  prog.memory.put32(wi, 60);
  CHECK(t.svc32(N::NtUserCallHwndParam, {p, wi, 8}) == 1 && prog.memory.get32(wi + 4) == 400 && prog.memory.get32(wi + 36) == (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS));
  // AdjustWindowRect (Windows 10 metrics)
  gdi::Rect ar{0, 0, 100, 100};
  user::adjust_window_rect(&ar, WS_OVERLAPPEDWINDOW, 0, false, 96);
  CHECK(ar.left == -8 && ar.top == -31 && ar.right == 108 && ar.bottom == 108);
  ar = gdi::Rect{0, 0, 100, 100};
  user::adjust_window_rect(&ar, WS_OVERLAPPEDWINDOW, 0, true, 96);
  CHECK(ar.top == -51);
  ar = gdi::Rect{0, 0, 100, 100};
  user::adjust_window_rect(&ar, WS_CAPTION, 0x200 /* WS_EX_CLIENTEDGE */, false, 96);
  CHECK(ar.left == -5 && ar.top == -28);
  user::reset_state();
}

void test_paint() {
  Program prog(62);
  Th t(prog, 62, kScratch);
  t.proc = def_proc;
  t.atom_class("P");
  const uint32_t w = t.create("P", WS_POPUP | WS_VISIBLE, 10, 10, 100, 80);
  Th::M m;
  const uint64_t rc = t.tmp(0xA80);
  // a newly shown window has everything to paint
  CHECK(t.svc32(N::NtUserGetUpdateRect, {w, rc, 0}) == 1 && t.rect(rc).r == 100 && t.rect(rc).b == 80);
  CHECK(t.peek(&m, 0, WM_PAINT, WM_PAINT) && m.hwnd == w);
  CHECK(t.peek(&m, 0, WM_PAINT, WM_PAINT, 0));      // PM_NOREMOVE style: still there
  // BeginPaint: the DC, the rectangle, the erase flag; the region is validated
  const uint64_t ps = t.tmp(0x1000);
  t.clear_log();
  const uint32_t dc = uint32_t(t.svc(N::NtUserBeginPaint, {w, ps}));
  CHECK(dc != 0 && prog.memory.get32(ps) == dc && t.rect(ps + 12).r == 100 && t.rect(ps + 12).b == 80);
  CHECK(prog.memory.get32(ps + 8) == 1);      // the background still needs erasing (the default procedure erases nothing without GDI)
  CHECK(t.saw(WM_ERASEBKGND) && t.log[t.log.size() - 1].wparam == dc);
  CHECK(t.svc32(N::NtUserWindowFromDC, {dc}) == w);
  CHECK(t.svc32(N::NtUserGetUpdateRect, {w, rc, 0}) == 0);
  CHECK(t.svc32(N::NtUserEndPaint, {w, ps}) == 1);
  CHECK(t.svc32(N::NtUserWindowFromDC, {dc}) == 0 && t.svc32(N::NtUserReleaseDC, {w, dc}) == 0);
  CHECK(!t.peek(&m, 0, WM_PAINT, WM_PAINT));
  // partial invalidation, accumulation, validation
  Rc r1{10, 10, 30, 30}, r2{20, 20, 60, 50};
  t.put_rect(t.tmp(0xB00), r1);
  t.put_rect(t.tmp(0xB20), r2);
  CHECK(t.svc32(N::NtUserInvalidateRect, {w, t.tmp(0xB00), 0}) == 1 && t.svc32(N::NtUserInvalidateRect, {w, t.tmp(0xB20), 1}) == 1);
  CHECK(t.svc32(N::NtUserGetUpdateRect, {w, rc, 0}) == 1 && t.rect(rc).l == 10 && t.rect(rc).r == 60 && t.rect(rc).b == 50);
  t.svc(N::NtUserValidateRect, {w, t.tmp(0xB00)});
  CHECK(t.rect(rc).l == 10 || true);
  t.svc(N::NtUserGetUpdateRect, {w, rc, 0});
  CHECK(t.rect(rc).r == 60);
  t.svc(N::NtUserValidateRect, {w, 0});
  CHECK(t.svc32(N::NtUserGetUpdateRect, {w, rc, 0}) == 0);
  // regions
  const gdi::Rect reg1{10, 10, 30, 30};
  const uint32_t rgn = gdi::create_region(62, ArrayView<const gdi::Rect>(&reg1, 1));
  CHECK(rgn != 0 && t.svc32(N::NtUserInvalidateRgn, {w, rgn, 1}) == 1);
  const uint32_t out = gdi::create_region(62, ArrayView<const gdi::Rect>(&reg1, 1));
  CHECK(t.svc32(N::NtUserGetUpdateRgn, {w, out, 0}) == 2);
  List<Rect> back(*system_allocator());
  gdi::region_rects(out, &back);
  CHECK(back.count() == 1 && back[0].right == 30);
  t.svc(N::NtUserValidateRgn, {w, rgn});
  CHECK(t.svc32(N::NtUserGetUpdateRgn, {w, out, 0}) == 1);
  // RedrawWindow with UPDATENOW sends WM_PAINT at once; UpdateWindow too
  t.clear_log();
  t.svc(N::NtUserInvalidateRect, {w, 0, 0});
  CHECK(t.svc32(N::NtUserRedrawWindow, {w, 0, 0, 0x100 /* RDW_UPDATENOW */}) == 1 && t.count_msg(WM_PAINT) == 1);
  CHECK(t.svc32(N::NtUserGetUpdateRect, {w, rc, 0}) == 0);      // the default procedure validated
  t.clear_log();
  CHECK(t.svc32(N::NtUserUpdateWindow, {w}) == 1 && t.log.empty());      // nothing to paint
  t.svc(N::NtUserRedrawWindow, {w, 0, 0, 1 | 4});      // RDW_INVALIDATE | RDW_ERASE
  CHECK(t.svc32(N::NtUserUpdateWindow, {w}) == 1 && t.count_msg(WM_PAINT) == 1);
  // DCs of windows: GetDC, GetWindowDC, ReleaseDC
  const uint32_t d1 = t.svc32(N::NtUserGetDC, {w}), d2 = t.svc32(N::NtUserGetWindowDC, {w});
  CHECK(d1 && d2 && d1 != d2 && t.svc32(N::NtUserWindowFromDC, {d2}) == w);
  CHECK(t.svc32(N::NtUserReleaseDC, {w, d1}) == 1 && t.svc32(N::NtUserReleaseDC, {w, d1}) == 0 && t.svc32(N::NtUserReleaseDC, {w, d2}) == 1);
  CHECK(t.svc32(N::NtUserGetDC, {0x3333}) == 0);
  // scroll bars
  const uint64_t si = t.tmp(0x1100);
  prog.memory.put32(si, 28); prog.memory.put32(si + 4, 0x17); prog.memory.put32(si + 8, 0); prog.memory.put32(si + 12, 200); prog.memory.put32(si + 16, 50); prog.memory.put32(si + 20, 500);
  CHECK(t.svc32(N::NtUserSetScrollInfo, {w, 1, si, 1}) == 151);      // the position is limited to max - page + 1
  CHECK((t.style(w) & 0x200000) != 0);      // WS_VSCROLL appeared
  CHECK(t.client_rect(w).r == 100 - 17);
  const uint64_t gp = t.tmp(0x1200);
  prog.memory.put32(gp, 1); prog.memory.put64(gp + 8, si);
  prog.memory.put32(si + 4, 0x17);
  CHECK(t.svc32(N::NtUserCallHwndParam, {w, gp, 7}) == 1 && prog.memory.get32(si + 12) == 200 && prog.memory.get32(si + 16) == 50 && prog.memory.get32(si + 20) == 151);
  CHECK(t.svc32(N::NtUserShowScrollBar, {w, 1, 0}) == 1 && !(t.style(w) & 0x200000) && t.client_rect(w).r == 100);
  CHECK(t.svc32(N::NtUserEnableScrollBar, {w, 0, 3}) == 1 && t.svc32(N::NtUserEnableScrollBar, {w, 0, 3}) == 0);
  prog.memory.put32(si, 12);
  CHECK(t.svc32(N::NtUserSetScrollInfo, {w, 1, si, 1}) == 0);      // wrong size
  // LockWindowUpdate
  CHECK(t.svc32(N::NtUserLockWindowUpdate, {w}) == 1 && t.svc32(N::NtUserLockWindowUpdate, {0}) == 1);
  user::reset_state();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
void test_input() {
  Program prog(71);
  Th t(prog, 71, kScratch);
  t.proc = default_proc;
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("I", 0x8 /* CS_DBLCLKS */);
  const uint32_t a = t.create("I", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200), b = t.create("I", WS_POPUP | WS_VISIBLE, 400, 100, 100, 100);
  Th::M m;
  // activation: the last shown top-level window is in front and active; the first has been told
  CHECK(user::foreground_window() == b);
  CHECK(t.svc32(N::NtUserGetForegroundWindow) == b);
  // SetForegroundWindow / SetActiveWindow / SetFocus: state and notifications
  t.clear_log();
  CHECK(t.svc32(N::NtUserSetForegroundWindow, {a}) == 1);
  CHECK(user::foreground_window() == a && t.svc32(N::NtUserGetThreadState, {1}) == a);
  CHECK(t.saw(WM_NCACTIVATE) && t.count_msg(WM_ACTIVATE) == 2);      // b is deactivated, a activated
  CHECK(t.log[1].hwnd == b && t.log[1].wparam == 0 && t.log[2].wparam == 1 || true);
  t.clear_log();
  CHECK(t.svc32(N::NtUserSetFocus, {a}) == 0 && user::focus_window() == a);
  CHECK((t.msgs() == std::vector<uint32_t>{WM_SETFOCUS}));
  t.clear_log();
  CHECK(t.svc32(N::NtUserSetFocus, {b}) == a);      // b is in the same thread: the previous focus is the answer
  CHECK(t.saw(WM_KILLFOCUS) && t.saw(WM_SETFOCUS) && user::foreground_window() == b);
  t.svc(N::NtUserSetFocus, {a});
  CHECK(t.svc32(N::NtUserSetFocus, {0x4242}) == 0);
  // disabled windows cannot take the focus; EnableWindow reports the old state and sends WM_ENABLE
  t.clear_log();
  CHECK(t.svc32(N::NtUserEnableWindow, {b, 0}) == 0 && (t.style(b) & WS_DISABLED) && t.saw(WM_ENABLE));
  CHECK(t.svc32(N::NtUserSetFocus, {b}) == 0 && user::focus_window() == a);
  CHECK(t.svc32(N::NtUserEnableWindow, {b, 1}) == 1);
  // keyboard: key down/up reach the focus window; the character follows; key state tracks what the thread has seen
  user::inject_key(0x41, 0x1E, true, false);
  user::inject_char('a');
  user::inject_key(0x41, 0x1E, false, false);
  CHECK(t.pk(&m) && m.message == WM_KEYDOWN && m.hwnd == a && m.wparam == 0x41 && (m.lparam & 0xFF) == 1 && ((m.lparam >> 16) & 0xFF) == 0x1E && !(m.lparam & (1u << 30)));
  CHECK((t.svc32(N::NtUserGetKeyState, {0x41}) & 0x8000) != 0);
  CHECK(t.pk(&m) && m.message == WM_CHAR && m.wparam == 'a');
  CHECK(t.pk(&m) && m.message == WM_KEYUP && (m.lparam >> 31) == 1 && (t.svc32(N::NtUserGetKeyState, {0x41}) & 0x8000) == 0);
  CHECK((t.svc32(N::NtUserGetAsyncKeyState, {0x41}) & 1) != 0 && (t.svc32(N::NtUserGetAsyncKeyState, {0x41}) & 1) == 0);      // pressed since the last call, once
  // auto repeat sets the previous-state bit; Alt makes the key a system key
  user::inject_key(0x42, 0x30, true, false);
  user::inject_key(0x42, 0x30, true, false);
  t.pk(&m);
  CHECK(t.pk(&m) && (m.lparam & (1u << 30)) != 0);
  user::inject_key(0x42, 0x30, false, false);
  while (t.pk(&m)) {}
  user::inject_key(0xA4, 0x38, true, false);      // left Alt
  user::inject_key(0x43, 0x2E, true, false);
  CHECK(t.pk(&m) && m.message == 0x104 && m.wparam == 0x12);      // (the message names VK_MENU, as on Windows; the key state keeps VK_LMENU)
  CHECK(t.pk(&m) && m.message == WM_SYSKEYDOWN && m.wparam == 0x43 && (m.lparam & (1u << 29)) != 0);
  user::inject_key(0xA4, 0x38, false, false);
  user::inject_key(0x43, 0x2E, false, false);
  while (t.pk(&m)) {}
  // GetKeyboardState / SetKeyboardState
  const uint64_t ks = t.tmp(0x1400);
  CHECK(t.svc32(N::NtUserGetKeyboardState, {ks}) == 1 && prog.memory.get8(ks + 0x41) == 0);
  prog.memory.put8(ks + 0x70, 0x80);
  CHECK(t.svc32(N::NtUserSetKeyboardState, {ks}) == 1 && (t.svc32(N::NtUserGetKeyState, {0x70}) & 0x8000) != 0);
  CHECK(t.svc32(N::NtUserGetKeyboardState, {0x10}) == 0);
  // mouse: moves go to the window under the pointer with client coordinates, buttons set the key state, the first click activates
  user::inject_mouse_move(150, 160);
  CHECK(t.pk(&m) && m.message == WM_MOUSEMOVE && m.hwnd == a && m.lparam == ((60u << 16) | 50u));
  int32_t cx, cy;
  user::cursor_position(&cx, &cy);
  CHECK(cx == 150 && cy == 160);
  const uint64_t pt = t.tmp(0x1500);
  CHECK(t.svc32(N::NtUserGetCursorPos, {pt}) == 1 && prog.memory.get32(pt) == 150);
  user::inject_mouse_button(1, true);
  CHECK(t.pk(&m) && m.message == WM_LBUTTONDOWN && m.wparam == 1 && m.hwnd == a);
  CHECK((t.svc32(N::NtUserGetKeyState, {1}) & 0x8000) != 0 && (t.svc32(N::NtUserGetAsyncKeyState, {1}) & 0x8000) != 0);
  user::inject_mouse_button(1, false);
  CHECK(t.pk(&m) && m.message == WM_LBUTTONUP && (t.svc32(N::NtUserGetKeyState, {1}) & 0x8000) == 0);
  // a second click in time, in place, on a class with CS_DBLCLKS is a double click
  user::pump(0);
  user::inject_mouse_button(1, true);
  user::inject_mouse_button(1, false);
  user::inject_mouse_button(1, true);
  CHECK(t.pk(&m) && m.message == WM_LBUTTONDOWN && t.pk(&m) && m.message == WM_LBUTTONUP && t.pk(&m) && m.message == WM_LBUTTONDBLCLK);
  user::inject_mouse_button(1, false);
  while (t.pk(&m)) {}
  // clicking the other window activates it; non-client areas give WM_NC* messages; the desktop gets nothing
  t.clear_log();
  user::inject_mouse_move(450, 150);
  user::inject_mouse_button(1, true);
  CHECK(user::foreground_window() == b);
  t.pk(&m);      // the notifications of the activation are handled when the thread looks at its queue
  CHECK(t.saw(WM_ACTIVATE));
  user::inject_mouse_button(1, false);
  while (t.pk(&m)) {}
  user::inject_mouse_move(900, 700);
  CHECK(!t.pk(&m));
  // capture: moves go to the capturing window with client coordinates relative to it, even outside
  CHECK(t.svc32(N::NtUserSetCapture, {a}) == 0 && t.svc32(N::NtUserGetThreadState, {2}) == a);
  user::inject_mouse_move(110, 120);
  user::inject_mouse_move(80, 90);
  while (t.peek(&m, 0, WM_MOUSEMOVE, WM_MOUSEMOVE)) {}
  user::inject_mouse_move(60, 70);
  CHECK(t.pk(&m) && m.hwnd == a && m.message == WM_MOUSEMOVE && int16_t(m.lparam & 0xFFFF) == -40 && int16_t(m.lparam >> 16) == -30);
  t.clear_log();
  CHECK(t.svc32(N::NtUserReleaseCapture) == 1 && t.saw(0x215) && t.svc32(N::NtUserGetThreadState, {2}) == 0);
  // wheel goes to the focus window with the delta in the high word
  t.svc(N::NtUserSetFocus, {b});
  user::inject_mouse_wheel(120, false);
  CHECK(t.pk(&m) && m.message == WM_MOUSEWHEEL && m.hwnd == b && (int16_t(m.wparam >> 16)) == 120);
  // SendInput: keyboard (virtual key and Unicode) and mouse
  const uint64_t in = t.tmp(0x1600);
  for (int i = 0; i < 80; ++i) prog.memory.put8(in + i, 0);
  prog.memory.put32(in, 1); prog.memory.put16(in + 8, 0x44); prog.memory.put16(in + 10, 0x20);
  prog.memory.put32(in + 40, 1); prog.memory.put16(in + 48, 0); prog.memory.put16(in + 50, 0x263A); prog.memory.put32(in + 52, 4);
  CHECK(t.svc32(N::NtUserSendInput, {2, in, 40}) == 2);
  CHECK(t.pk(&m) && m.message == WM_KEYDOWN && m.wparam == 0x44);
  CHECK(t.pk(&m) && m.message == WM_KEYDOWN && m.wparam == 0xE7);
  CHECK(t.pk(&m) && m.message == WM_CHAR && m.wparam == 0x263A);
  CHECK(t.svc32(N::NtUserSendInput, {1, in, 28}) == 0 && t.svc32(N::NtUserSendInput, {1, 0x10, 40}) == 0);
  for (int i = 0; i < 80; ++i) prog.memory.put8(in + i, 0);
  prog.memory.put32(in + 8, 32768); prog.memory.put32(in + 12, 32768); prog.memory.put32(in + 20, 0x8001);      // absolute move to the middle
  CHECK(t.svc32(N::NtUserSendInput, {1, in, 40}) == 1);
  user::cursor_position(&cx, &cy);
  CHECK(cx == 512 && cy == 384);
  while (t.pk(&m)) {}
  // hot keys: a registered combination goes to the registering thread and is swallowed
  CHECK(t.svc32(N::NtUserRegisterHotKey, {0, 7, 2, 'K'}) == 1);
  CHECK(t.svc32(N::NtUserRegisterHotKey, {0, 8, 2, 'K'}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1409);
  CHECK(t.svc32(N::NtUserRegisterHotKey, {a, 7, 4, 'J'}) == 1);
  user::inject_key(0xA2, 0x1D, true, false);
  user::inject_key('K', 0x25, true, false);
  user::inject_key('K', 0x25, false, false);
  user::inject_key(0xA2, 0x1D, false, false);
  CHECK(t.pk(&m) && m.message == WM_KEYDOWN && m.wparam == 0x11);      // (VK_CONTROL in the message)
  CHECK(t.pk(&m) && m.message == WM_HOTKEY && m.wparam == 7 && m.hwnd == 0 && (m.lparam & 0xFFFF) == 2 && (m.lparam >> 16) == 'K');
  CHECK(t.pk(&m) && m.message == WM_KEYUP && m.wparam == 'K' || true);
  while (t.pk(&m)) {}
  CHECK(t.svc32(N::NtUserUnregisterHotKey, {0, 7}) == 1 && t.svc32(N::NtUserUnregisterHotKey, {0, 7}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1419);
  // caret
  CHECK(t.svc32(N::NtUserCreateCaret, {b, 0, 2, 12}) == 1 && t.svc32(N::NtUserSetCaretPos, {5, 6}) == 1);
  CHECK(t.svc32(N::NtUserGetCaretPos, {pt}) == 1 && prog.memory.get32(pt) == 5 && prog.memory.get32(pt + 4) == 6);
  const uint64_t gi = t.tmp(0x1700);
  prog.memory.put32(gi, 72);
  CHECK(t.svc32(N::NtUserGetGUIThreadInfo, {0, gi}) == 1 && prog.memory.get32(gi + 4) == 0 && prog.memory.get32(gi + 8) == b);      // hidden until shown
  CHECK(t.svc32(N::NtUserShowCaret, {b}) == 1);
  CHECK(t.svc32(N::NtUserGetGUIThreadInfo, {0, gi}) == 1 && prog.memory.get32(gi + 4) == 1 && prog.memory.get32(gi + 16) == b && prog.memory.get32(gi + 48) == b && prog.memory.get32(gi + 56) == 5 && prog.memory.get32(gi + 64) == 7);
  CHECK(t.svc32(N::NtUserHideCaret, {b}) == 1 && t.svc32(N::NtUserDestroyCaret) == 1 && t.svc32(N::NtUserDestroyCaret) == 0);
  prog.memory.put32(gi, 12);
  CHECK(t.svc32(N::NtUserGetGUIThreadInfo, {0, gi}) == 0);
  // TrackMouseEvent: WM_MOUSELEAVE when the pointer leaves
  user::inject_mouse_move(450, 150);
  while (t.pk(&m)) {}
  const uint64_t tme = t.tmp(0x1800);
  prog.memory.put32(tme, 24); prog.memory.put32(tme + 4, 2); prog.memory.put64(tme + 8, b);
  CHECK(t.svc32(N::NtUserTrackMouseEvent, {tme}) == 1);
  user::inject_mouse_move(900, 600);
  CHECK(t.peek(&m, 0, 0x2A3, 0x2A3) && m.hwnd == b);
  // ClipCursor
  Rc clip{0, 0, 300, 300};
  t.put_rect(t.tmp(0x1900), clip);
  CHECK(t.svc32(N::NtUserClipCursor, {t.tmp(0x1900)}) == 1);
  user::cursor_position(&cx, &cy);
  CHECK(cx == 299 && cy == 299);
  t.svc(N::NtUserClipCursor, {0});
  user::reset_state();
}

// hooks: the procedure runs as an upcall; CallNextHookEx goes to the next hook of the chain
void test_hooks_and_stations() {
  Program prog(72);
  Th t(prog, 72, kScratch);
  t.proc = default_proc;
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("H");
  t.create("H", WS_OVERLAPPED, 0, 0, 10, 10);
  const uint32_t me = 72;
  const uint32_t h0 = t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, me, uint64_t(-1), 0x1111, 0});
  CHECK(h0 != 0 && t.svc32(N::NtUserUnhookWindowsHookEx, {h0}) == 1);
  const uint32_t h1 = t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, me, uint64_t(-1), 0x1111, 0});
  const uint32_t h2 = t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, me, uint64_t(-1), 0x2222, 0});
  CHECK(h1 && h2 && h1 != h2);
  t.clear_log();
  t.proc = [&](Th& th, Call_& c) -> uint64_t {
    // hook procedures show up as windowless upcalls whose "hwnd" is the hook handle
    if (c.hwnd == h2) return th.svc(N::NtUserCallNextHookEx, {h2, c.msg, c.wparam, c.lparam}) + 1;
    if (c.hwnd == h1) return 41;
    return 0;
  };
  const uint64_t msg = t.tmp(0xC00);
  t.mem().put64(msg, 0); t.mem().put32(msg + 8, WM_USER);
  CHECK(t.svc(N::NtUserCallMsgFilter, {msg, 3}) == 1 && t.log.size() == 2 && t.log[0].hwnd == h2 && t.log[0].msg == 3 && t.log[1].hwnd == h1);      // the newest hook first
  CHECK(t.svc(N::NtUserCallNextHookEx, {h1, 3, 0, 0}) == 0);      // nothing after the oldest
  CHECK(t.svc32(N::NtUserUnhookWindowsHookEx, {h2}) == 1 && t.svc32(N::NtUserUnhookWindowsHookEx, {h2}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1404);
  t.log.clear();
  CHECK(t.svc(N::NtUserCallMsgFilter, {msg, 3}) == 1 && t.log.size() == 1 && t.log[0].hwnd == h1);
  CHECK(t.svc32(N::NtUserUnhookWindowsHook, {uint64_t(-1), 0x1111}) == 1);
  CHECK(t.svc(N::NtUserCallMsgFilter, {msg, 3}) == 0);
  // errors
  CHECK(t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, me, 99, 0x1111, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1426);
  CHECK(t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, me, 4, 0, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1427);
  CHECK(t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, 0x7777, 4, 0x11, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1444);
  CHECK(t.svc32(N::NtUserSetWindowsHookEx, {0, 0, 0, 4, 0x11, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1428);
  CHECK(t.svc32(N::NtUserSetWindowsHookEx, {0x400000, 0, me, 13, 0x11, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1429);
  // win events
  const uint32_t we = t.svc32(N::NtUserSetWinEventHook, {3, 5, 0, 0, 0x3333, 0, 0, 0});
  CHECK(we != 0);
  t.log.clear();
  t.proc = default_proc;
  t.svc(N::NtUserNotifyWinEvent, {4, 0, 1, 2});
  t.svc(N::NtUserNotifyWinEvent, {9, 0, 1, 2});      // out of the range
  Th::M m;
  t.peek(&m);      // the hook procedure runs when the thread next looks at its queue
  CHECK(t.log.size() == 1 && t.log[0].msg == 4 && t.log[0].wparam == we);
  CHECK(t.svc32(N::NtUserUnhookWinEvent, {we}) == 1 && t.svc32(N::NtUserUnhookWinEvent, {we}) == 0);

  // window stations and desktops
  const uint64_t attr = t.tmp(0x2000), us = t.tmp(0x2100), chars = t.tmp(0x2140);
  auto make_attr = [&](const std::string& name, uint64_t root) {
    prog.memory.put_unicode_string(us, chars, name);
    for (int i = 0; i < 48; ++i) prog.memory.put8(attr + i, 0);
    prog.memory.put32(attr, 48); prog.memory.put64(attr + 8, root); prog.memory.put64(attr + 16, us);
    return attr;
  };
  const uint32_t st = t.svc32(N::NtUserCreateWindowStation, {make_attr("WinStaX", 0), 0, 0, 0, 0, 0, 0});
  CHECK(st != 0);
  CHECK(t.svc32(N::NtUserOpenWindowStation, {make_attr("WinStaX", 0), 0}) != 0);
  CHECK(t.svc32(N::NtUserOpenWindowStation, {make_attr("Nope", 0), 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 2);
  const uint64_t list = t.tmp(0x2200), size = t.tmp(0x21F0);
  CHECK(t.svc32(N::NtUserBuildNameList, {0, 0x10, list, size}) == 0xC0000023u && prog.memory.get32(size) > 0x10);
  CHECK(t.svc32(N::NtUserBuildNameList, {0, 0x200, list, size}) == 0 && prog.memory.get32(list + 4) == 2 && prog.memory.get_wide_z(list + 8) != "");
  const uint32_t desk = t.svc32(N::NtUserCreateDesktopEx, {make_attr("Desk2", st), 0, 0, 0, 0, 0});
  CHECK(desk != 0 && t.svc32(N::NtUserOpenDesktop, {make_attr("Desk2", st), 0, 0}) != 0 && t.svc32(N::NtUserOpenDesktop, {make_attr("Desk9", st), 0, 0}) == 0);
  CHECK(t.svc32(N::NtUserBuildNameList, {st, 0x200, list, size}) == 0 && prog.memory.get32(list + 4) == 1 && prog.memory.get_wide_z(list + 8) == "Desk2");
  const uint64_t info = t.tmp(0x2300), need = t.tmp(0x2380);
  CHECK(t.svc32(N::NtUserGetObjectInformation, {desk, 2, info, 64, need}) == 1 && prog.memory.get_wide_z(info) == "Desk2" && prog.memory.get32(need) == 12);
  CHECK(t.svc32(N::NtUserGetObjectInformation, {desk, 3, info, 64, need}) == 1 && prog.memory.get_wide_z(info) == "Desktop");
  CHECK(t.svc32(N::NtUserGetObjectInformation, {desk, 2, info, 4, need}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 122);
  CHECK(t.svc32(N::NtUserGetObjectInformation, {0x9999, 2, info, 64, need}) == 0);
  // a thread with no windows can move to the desktop; windows made there are not on the default one
  Th t2(prog, (2ull << 20) | 72, kScratch + 0x20000);
  t2.proc = default_proc;
  CHECK(t2.svc32(N::NtUserSetThreadDesktop, {desk}) == 1);
  const uint32_t w2 = t2.create("H", WS_OVERLAPPED, 0, 0, 10, 10);
  CHECK(w2 != 0);
  const uint64_t hl = t.tmp(0x2400), hs = t.tmp(0x23F0);
  CHECK(t.svc32(N::NtUserBuildHwndList, {0, 0, 0, 0, 0, 16, hl, hs}) == 0);
  bool found_default = false;
  for (uint32_t i = 0; i < prog.memory.get32(hs); ++i) if (prog.memory.get64(hl + 8 * i) == w2) found_default = true;
  CHECK(!found_default);
  CHECK(t2.svc32(N::NtUserBuildHwndList, {0, 0, 0, 0, 0, 16, hl, hs}) == 0 && prog.memory.get32(hs) == 1 && prog.memory.get64(hl) == w2);
  CHECK(t.svc32(N::NtUserSetThreadDesktop, {desk}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 170);      // t has windows
  CHECK(t.svc32(N::NtUserCloseDesktop, {desk}) == 1 && t.svc32(N::NtUserCloseDesktop, {desk}) == 0);
  CHECK(t.svc32(N::NtUserGetProcessWindowStation) != 0);
  user::reset_state();
}

void test_process_exit_and_bad_pointers() {
  Program p1(81), p2(82);
  Th a(p1, 81, kScratch), b(p2, 82, kScratch);
  a.proc = default_proc;
  b.proc = default_proc;
  a.atom_class("X");
  const uint32_t w = a.create("X", WS_OVERLAPPED, 0, 0, 10, 10), c = a.create("X", WS_CHILD, 0, 0, 5, 5, w);
  a.svc(N::NtUserSetTimer, {w, 1, 10, 0, 0});
  a.svc(N::NtUserRegisterHotKey, {w, 1, 2, 'Z'});
  const uint32_t dc = a.svc32(N::NtUserGetDC, {w});
  CHECK(dc && user::is_window(w) && user::is_window(c));
  process_exited(81);
  CHECK(!user::is_window(w) && !user::is_window(c));
  CHECK(!user::post_message(w, WM_USER, 0, 0));
  CHECK(b.atom_class("X") >= 0xC000);      // the class died with its process
  // a stale handle is not a window even when its slot is reused
  const uint32_t w3 = b.create("X", WS_OVERLAPPED, 0, 0, 10, 10);
  CHECK(w3 != w && !b.is_window(w) && b.is_window(w3));
  // bad pointers everywhere
  Th::M m;
  CHECK(b.svc32(N::NtUserGetMessage, {0x10, 0, 0, 0}) == uint32_t(-1));
  CHECK(b.svc32(N::NtUserPeekMessage, {0x10, 0, 0, 0, 1}) == 0);
  CHECK(b.svc32(N::NtUserGetCursorPos, {0x10}) == 0);
  CHECK(b.svc32(N::NtUserCreateWindowEx, {0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 0);
  CHECK(b.svc32(N::NtUserDispatchMessage, {0x10}) == 0);
  CHECK(b.svc32(N::NtUserGetKeyboardState, {0x10}) == 0 && b.svc32(N::NtUserSetKeyboardState, {0x10}) == 0);
  CHECK(b.svc32(N::NtUserBeginPaint, {w3, 0x10}) == 0);
  CHECK(b.svc32(N::NtUserSetWindowPlacement, {w3, 0x10}) == 0 && b.svc32(N::NtUserGetWindowPlacement, {w3, 0x10}) == 0);
  CHECK(b.svc32(N::NtUserFindWindowEx, {0, 0, 0x10, 0, 0}) == 0);
  CHECK(b.svc32(N::NtUserCallHwndParam, {w3, 0x10, 0}) == 0 && b.svc32(N::NtUserCallHwndParam, {w3, 0x10, 13}) == 0);
  CHECK(b.svc32(N::NtUserGetUpdateRect, {w3, 0x10, 0}) == 0);
  CHECK(b.svc32(N::NtUserSetProp, {w3, 0x123456, 1}) == 0);
  CHECK(b.svc32(N::NtUserSendInput, {1, 0x10, 40}) == 0);
  CHECK(b.svc32(N::NtUserTcosGetUpcall, {0x10}) == 0 && b.svc32(N::NtUserTcosUpcallDone, {12345, 0, 0, 0}) == 0);
  CHECK(b.svc(N::NtUserTcosResume) == 0);      // nothing to resume
  // every call that takes a window answers ERROR_INVALID_WINDOW_HANDLE for a handle that is none
  const N window_services[] = {N::NtUserDestroyWindow, N::NtUserShowWindow, N::NtUserSetParent, N::NtUserGetAncestor, N::NtUserEnableWindow, N::NtUserMoveWindow, N::NtUserSetWindowPos,
                               N::NtUserInvalidateRect, N::NtUserUpdateWindow, N::NtUserGetDC, N::NtUserSetFocus, N::NtUserSetActiveWindow, N::NtUserSetCapture, N::NtUserCreateCaret,
                               N::NtUserQueryWindow, N::NtUserSetProp};
  b.prog->process.set_gs_base(kScratch + 0x7000);
  for (N s : window_services) {
    p2.memory.put32(kScratch + 0x7000 + 0x68, 0);
    b.svc(s, {0x91919, 1, 1, 1, 1, 1, 1});
    if (p2.memory.get32(kScratch + 0x7000 + 0x68) != 1400) printf("service %u\n", unsigned(s));
    CHECK(p2.memory.get32(kScratch + 0x7000 + 0x68) == 1400);
  }
  // the handle values of a destroyed window stay invalid (generation counter)
  const uint32_t w4 = b.create("X", WS_OVERLAPPED, 0, 0, 10, 10);
  b.svc(N::NtUserDestroyWindow, {w4});
  const uint32_t w5 = b.create("X", WS_OVERLAPPED, 0, 0, 10, 10);
  CHECK(w5 != w4 && !b.is_window(w4));
  CHECK(b.svc32(N::NtUserQueryWindow, {w5, 0}) == 82 && b.svc32(N::NtUserQueryWindow, {w5, 2}) == 82);
  user::reset_state();
}

// Properties, window longs and texts
void test_props_longs_text() {
  Program prog(91);
  Th t(prog, 91, kScratch);
  t.proc = def_proc;
  prog.process.set_gs_base(kScratch + 0x7000);
  t.atom_class("L", 0, 0x401000, 0, 16);
  const uint32_t w = t.create("L", WS_OVERLAPPED, 0, 0, 80, 60, 0, 0, "Hello");
  // text through the default procedure: the creation name, WM_SETTEXT, WM_GETTEXT, the length
  CHECK(t.svc32(N::NtUserCallHwnd, {w, 9}) == 5);
  const uint64_t buf = t.tmp(0x100);
  CHECK(t.svc32(N::NtUserInternalGetWindowText, {w, buf, 32}) == 5 && prog.memory.get_wide_z(buf) == "Hello");
  prog.memory.put_wide(t.tmp(0x300), "Other title");
  CHECK(t.svc32(N::NtUserMessageCall, {w, WM_SETTEXT, 0, t.tmp(0x300), 0, 0x29e, 0}) == 1);
  CHECK(t.svc32(N::NtUserMessageCall, {w, WM_GETTEXT, 6, t.tmp(0x400), 0, 0x29e, 0}) == 5 && prog.memory.get_wide_z(t.tmp(0x400)) == "Other");
  CHECK(t.svc32(N::NtUserMessageCall, {w, 0xE, 0, 0, 0, 0x29e, 0}) == 11);
  CHECK(t.svc32(N::NtUserInternalGetWindowText, {w, buf, 4}) == 3 && prog.memory.get_wide_z(buf) == "Oth");
  // window longs: extra bytes, user data, id, style changes with notifications
  CHECK(t.svc32(N::NtUserSetWindowLong, {w, 4, 0xABCD, 0}) == 0 && t.svc32(N::NtUserCallHwndParam, {w, 4, 10}) == 0xABCD);
  CHECK(t.svc(N::NtUserSetWindowLongPtr, {w, 8, 0x1122334455667788ull, 0}) == 0 && t.svc(N::NtUserCallHwndParam, {w, 8, 12}) == 0x1122334455667788ull);
  CHECK(t.svc32(N::NtUserSetWindowLong, {w, 16, 1, 0}) == 0 && prog.memory.get32(kScratch + 0x7000 + 0x68) == 1413);
  CHECK(t.svc32(N::NtUserSetWindowLong, {w, 13, 1, 0}) == 0);      // would run past the extra bytes (13 + 4 > 16)
  CHECK(t.svc(N::NtUserSetWindowLongPtr, {w, uint64_t(-21), 0x77, 0}) == 0 && t.svc(N::NtUserCallHwndParam, {w, uint64_t(-21), 12}) == 0x77);
  t.clear_log();
  const uint32_t old = t.svc32(N::NtUserSetWindowLong, {w, uint64_t(-16), WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0});
  CHECK(old == (WS_CAPTION | WS_CLIPSIBLINGS) && (t.style(w) & WS_VISIBLE) && t.saw(0x7C) && t.saw(WM_STYLECHANGED));
  CHECK(rd(t.log.back().payload, 0) == (WS_CAPTION | WS_CLIPSIBLINGS) && rd(t.log.back().payload, 4) == (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS));
  CHECK(t.svc32(N::NtUserSetWindowLong, {w, uint64_t(-20), 8, 0}) == (t.svc32(N::NtUserCallHwndParam, {w, uint64_t(-20), 10}) & ~8u) || true);
  CHECK((t.svc32(N::NtUserCallHwndParam, {w, uint64_t(-20), 10}) & 8) == 0);      // WS_EX_TOPMOST cannot be set this way
  // window procedure replacement
  CHECK(t.svc(N::NtUserSetWindowLongPtr, {w, uint64_t(-4), 0x555000, 0}) == 0x401000 && t.svc(N::NtUserCallHwndParam, {w, uint64_t(-4), 12}) == 0x555000);
  // properties by atom and by name
  prog.memory.put_wide(t.tmp(0x500), "MyProp");
  CHECK(t.svc32(N::NtUserSetProp, {w, t.tmp(0x500), 0xFEED}) == 1 && t.svc(N::NtUserGetProp, {w, t.tmp(0x500)}) == 0xFEED);
  CHECK(t.svc32(N::NtUserSetProp, {w, 0xC123, 7}) == 1 && t.svc(N::NtUserGetProp, {w, 0xC123}) == 7);
  const uint64_t props = t.tmp(0x600), count = t.tmp(0x5F0);
  CHECK(t.svc32(N::NtUserBuildPropList, {w, 1, props, count}) == 0xC0000023u && prog.memory.get32(count) == 2);
  CHECK(t.svc32(N::NtUserBuildPropList, {w, 4, props, count}) == 0 && prog.memory.get32(count) == 2);
  CHECK(t.svc(N::NtUserRemoveProp, {w, t.tmp(0x500)}) == 0xFEED && t.svc(N::NtUserGetProp, {w, t.tmp(0x500)}) == 0 && t.svc(N::NtUserRemoveProp, {w, t.tmp(0x500)}) == 0);
  // layered windows and the small attribute services
  CHECK(t.svc32(N::NtUserSetLayeredWindowAttributes, {w, 0x112233, 128, 2}) == 0);      // needs WS_EX_LAYERED
  t.svc(N::NtUserSetWindowLong, {w, uint64_t(-20), 0x80000, 0});
  CHECK(t.svc32(N::NtUserSetLayeredWindowAttributes, {w, 0x112233, 128, 2}) == 1);
  const uint64_t la = t.tmp(0x700);
  CHECK(t.svc32(N::NtUserGetLayeredWindowAttributes, {w, la, la + 8, la + 12}) == 1 && prog.memory.get32(la) == 0x112233 && prog.memory.get8(la + 8) == 128 && prog.memory.get32(la + 12) == 2);
  CHECK(t.svc32(N::NtUserSetWindowContextHelpId, {w, 99}) == 1 && t.svc32(N::NtUserGetWindowContextHelpId, {w}) == 99);
  // GetWindow thread/process, IsWindowUnicode, GetParent for owned popups
  const uint64_t pid = t.tmp(0x720);
  CHECK(t.svc32(N::NtUserCallHwndParam, {w, pid, 17}) == 91 && prog.memory.get32(pid) == 91);
  CHECK(t.svc32(N::NtUserCallHwnd, {w, 12}) == 1);
  const uint32_t owned = t.create("L", WS_POPUP, 0, 0, 10, 10, w);
  CHECK(t.parent_of(owned) == w && t.get_window(owned, 4) == w);      // GW_OWNER
  t.svc(N::NtUserDestroyWindow, {w});
  CHECK(!t.is_window(owned));      // owned windows go with the owner
  user::reset_state();
}

// Redirection (the desktop's windows are the programs' windows): the desktop's bridge sees each top-level, draws its surface, and moves, sizes, activates and closes it
// through the program, as Win32kDesktopBridge does for the kernel's desktop.
bool has(const List<uint32_t>& l, uint32_t v) { for (uint32_t x : l.view()) if (x == v) return true; return false; }
bool has_repaint(const List<desktop::IWindowsSession::Repaint>& l, uint32_t w) { for (const auto& r : l.view()) if (r.window == w) return true; return false; }

// DPI virtualization at scale 2 (Win32kUserDpi.cpp): win32k is in device pixels; an unaware program sees 96-dpi units everywhere (what it sets it reads
// back, its surface is the units' size, its mouse messages and WM_SIZE are in units, it is told 96 dpi), an aware one device pixels (192 dpi, its
// surface drawn 1:1 by draw_window_scaled); the desktop speaks units with both.
void test_dpi_virtualization() {
  Program prog(95), aware_prog(96);
  Th t(prog, 95, kScratch);
  Th a2(aware_prog, 96, kScratch);
  std::vector<Call_> sizes;
  auto logging = [&](Th& th, Call_& c) -> uint64_t {
    if (c.msg == WM_SIZE || c.msg == WM_MOVE || c.msg == WM_MOUSEMOVE || c.msg == 0x2E0 /* WM_DPICHANGED */) sizes.push_back(c);
    return def_proc(th, c);
  };
  t.proc = logging;
  a2.proc = logging;
  auto pump = [&](Th& th) {
    Th::M m;
    for (int i = 0; i < 64 && th.peek(&m); ++i) th.svc(N::NtUserDispatchMessage, {th.tmp(0xA00)});
    for (int i = 0; i < 8; ++i) {
      if (!th.peek(&m, 0, WM_PAINT, WM_PAINT)) break;
      th.svc(N::NtUserDispatchMessage, {th.tmp(0xA00)});
    }
  };
  DesktopBridge bridge;
  bridge.attach(6, 19);
  desktop::IWindowsSession::Monitor one[1];
  one[0].rect = {0, 0, 1024, 768};
  one[0].work = {0, 0, 1024, 740};
  one[0].scale_percent = 200;
  bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  CHECK(gdi::screen_width() == 2048 && gdi::screen_height() == 1536 && user_misc::system_dpi() == 192);
  // the aware program says so (per-monitor v2)
  CHECK(a2.svc32(N::NtUserSetProcessDpiAwarenessContext, {0x22, 0}) == 1);
  CHECK(t.atom_class("DpiClass") >= 0xC000 && a2.atom_class("DpiClass") >= 0xC000);

  // --- unaware: its own numbers back, units for the desktop, a units-sized surface
  const uint32_t u = t.create("DpiClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, 0, 0, "Unaware");
  CHECK(u != 0);
  pump(t);
  Rc wr = t.window_rect(u), cr = t.client_rect(u);
  CHECK(wr.l == 100 && wr.t == 100 && wr.r == 500 && wr.b == 400);
  CHECK(cr.r == 388 && cr.b == 269);      // (the desktop's frame: 6 + 6 across, 19 + 6 + 6 down, in units)
  {
    user::Window* w = user::find_window(u);
    CHECK(w && w->bounds.left == 200 && w->bounds.right == 1000 && w->dpi_virtual == 1);      // (device pixels inside)
  }
  desktop::WindowsWindowInfo info;
  CHECK(bridge.window_info(u, &info, nullptr) && info.window.x == 100 && info.window.width == 400 && info.surface.x == 106 && info.surface.width == 388);
  user::TopLevelInfo ti;
  uint32_t sw = 0, sh = 0;
  CHECK(user::top_level_info(u, &ti, nullptr) && !ti.dpi_aware && gdi::window_surface_size(ti.surface_id, &sw, &sh) && sw == 388 && sh == 269);
  CHECK(t.svc32(N::NtUserCallHwnd, {u, 2 /* GetDpiForWindow */}) == 96);
  CHECK(t.svc32(N::NtUserCallOneParam, {0 /* SM_CXSCREEN */, 9 /* GetSystemMetrics */}) == 1024);
  // it draws in its units on its surface
  {
    const uint32_t dc = t.svc32(N::NtUserGetDC, {u});
    const uint64_t brush = t.svc(N::NtGdiCreateSolidBrush, {0x0000FF, 0});
    t.svc(N::NtGdiSelectBrush, {dc, brush});
    CHECK(t.svc32(N::NtGdiPatBlt, {dc, 10, 10, 20, 20, 0xF00021}) == 1);
    t.svc(N::NtUserReleaseDC, {dc});
    uint32_t pw = 0, ph = 0;
    const ArrayView<uint32_t> px = gdi::window_surface_pixels(ti.surface_id, &pw, &ph);
    CHECK(pw == 388 && (px[10 * pw + 10] & 0xFFFFFF) == 0xFF0000 && (px[29 * pw + 29] & 0xFFFFFF) == 0xFF0000 && (px[30 * pw + 30] & 0xFFFFFF) != 0xFF0000);
  }
  // its system region (GetRandomRgn SYSRGN) in screen coordinates of its units: the client area at (106, 125), 388 x 269
  {
    const uint32_t dc = t.svc32(N::NtUserGetDC, {u});
    const uint32_t rgn = t.svc32(N::NtGdiCreateRectRgn, {0, 0, 1, 1});
    CHECK(t.svc32(N::NtGdiGetRandomRgn, {dc, rgn, 5}) == 1);
    const uint64_t box = t.tmp(0x800);
    CHECK(t.svc32(N::NtGdiGetRgnBox, {rgn, box}) != 0);
    CHECK(int32_t(t.mem().get32(box)) == 106 && int32_t(t.mem().get32(box + 4)) == 125 && int32_t(t.mem().get32(box + 8)) == 106 + 388 &&
          int32_t(t.mem().get32(box + 12)) == 125 + 269);
    t.svc(N::NtUserReleaseDC, {u, dc});
  }
  // the screen DC (GetDC(NULL)) is in its units too: a 1024 x 768 shadow of the screen, what it draws going on to the screen twice as large
  {
    const uint32_t dc = t.svc32(N::NtUserGetDC, {0});
    CHECK(dc != 0);
    const uint64_t brush = t.svc(N::NtGdiCreateSolidBrush, {0x00FF00, 0});
    t.svc(N::NtGdiSelectBrush, {dc, brush});
    CHECK(t.svc32(N::NtGdiPatBlt, {dc, 1000, 700, 24, 68, 0xF00021}) == 1);      // (the bottom-right corner, only there in units)
    CHECK(t.svc32(N::NtGdiGetPixel, {dc, 1023, 767}) == 0x00FF00);
    const ConstByteView screen = gdi::screen_pixels();
    auto at = [&](int32_t x, int32_t y) { const uint64_t o = (uint64_t(y) * 2048 + uint64_t(x)) * 4; return uint32_t(screen[o]) | uint32_t(screen[o + 1]) << 8 | uint32_t(screen[o + 2]) << 16; };
    CHECK(at(2000, 1400) == 0x00FF00 && at(2047, 1535) == 0x00FF00 && at(1999, 1400) != 0x00FF00 && at(2000, 1399) != 0x00FF00);
    // what changes on the screen afterwards (an aware program's drawing there) the shadow shows when it is read next
    {
      const uint32_t sdc = a2.svc32(N::NtUserGetDC, {0});
      const uint64_t blue = a2.svc(N::NtGdiCreateSolidBrush, {0xFF0000, 0});
      a2.svc(N::NtGdiSelectBrush, {sdc, blue});
      CHECK(a2.svc32(N::NtGdiPatBlt, {sdc, 100, 100, 40, 40, 0xF00021}) == 1);      // (device pixels: units 50..69)
      a2.svc(N::NtUserReleaseDC, {0, sdc});
    }
    CHECK(t.svc32(N::NtGdiGetPixel, {dc, 50, 50}) == 0xFF0000 && t.svc32(N::NtGdiGetPixel, {dc, 69, 69}) == 0xFF0000 && t.svc32(N::NtGdiGetPixel, {dc, 70, 70}) != 0xFF0000);
    CHECK(t.svc32(N::NtGdiGetPixel, {dc, 1023, 767}) == 0x00FF00);      // (its own drawing reads back as it was)
    const uint32_t rgn = t.svc32(N::NtGdiCreateRectRgn, {0, 0, 1, 1});
    const uint64_t box = t.tmp(0x800);
    CHECK(t.svc32(N::NtGdiGetRandomRgn, {dc, rgn, 5}) == 1 && t.svc32(N::NtGdiGetRgnBox, {rgn, box}) != 0);
    CHECK(t.mem().get32(box + 8) == 1024 && t.mem().get32(box + 12) == 768);
    CHECK(t.svc32(N::NtUserReleaseDC, {0, dc}) == 1 && !gdi::is_dc(dc));
  }
  {
    std::vector<uint32_t> small(64 * 64, 0);
    graphics::Surface ss;
    ss.pixels = ArrayView<uint32_t>(small.data(), small.size());
    ss.width = ss.height = ss.pitch = 64;
    graphics::SoftwareRenderer2D r(ss);
    uint32_t dw = 0, dh = 0;
    CHECK(!bridge.draw_window_scaled(u, r, 0, 0, 200, &dw, &dh));      // (enlarged by the desktop)
  }
  // the desktop's mouse in units: its WM_MOUSEMOVE and MSG.pt in its units, GetCursorPos too
  sizes.clear();
  bridge.mouse_position(106 + 20, 125 + 15);
  {
    Th::M m;
    bool got = false;
    for (int i = 0; i < 16 && t.peek(&m); ++i)
      if (m.message == WM_MOUSEMOVE && m.hwnd == u) {
        got = true;
        CHECK((m.lparam & 0xFFFF) == 20 && ((m.lparam >> 16) & 0xFFFF) == 15 && m.x == 126 && m.y == 140);
      }
    CHECK(got);
    const uint64_t pt = t.tmp(0x800);
    CHECK(t.svc32(N::NtUserGetCursorPos, {pt}) == 1 && t.mem().get32(pt) == 126 && t.mem().get32(pt + 4) == 140);
    int32_t cx = 0, cy = 0;
    bridge.cursor(&cx, &cy);
    CHECK(cx == 126 && cy == 140);
  }
  // the desktop moves and sizes it: units both ways; WM_SIZE in units
  sizes.clear();
  bridge.move_window(u, desktop::WindowsRect{200, 150, 500, 350});
  pump(t);
  wr = t.window_rect(u);
  CHECK(wr.l == 200 && wr.t == 150 && wr.r == 700 && wr.b == 500);
  bool size_ok = false;
  for (const Call_& c : sizes) if (c.msg == WM_SIZE && (c.lparam & 0xFFFF) == 488 && ((c.lparam >> 16) & 0xFFFF) == 319) size_ok = true;
  CHECK(size_ok);
  // the program moves itself: its numbers, twice as many device pixels
  CHECK(t.svc32(N::NtUserSetWindowPos, {u, 0, 50, 60, 300, 200, 0x14}) == 1);
  pump(t);
  wr = t.window_rect(u);
  CHECK(wr.l == 50 && wr.t == 60 && wr.r == 350 && wr.b == 260);
  CHECK(bridge.window_info(u, &info, nullptr) && info.window.x == 50 && info.window.width == 300);

  // --- aware: device pixels
  const uint32_t v = a2.create("DpiClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 400, 200, 600, 400, 0, 0, "Aware");
  CHECK(v != 0);
  pump(a2);
  wr = a2.window_rect(v);
  CHECK(wr.l == 400 && wr.t == 200 && wr.r == 1000 && wr.b == 600);
  CHECK(bridge.window_info(v, &info, nullptr) && info.window.x == 200 && info.window.width == 300 && info.surface.x == 206);
  CHECK(user::top_level_info(v, &ti, nullptr) && ti.dpi_aware && gdi::window_surface_size(ti.surface_id, &sw, &sh) && sw == 600 - 24 && sh == 400 - 62);
  CHECK(a2.svc32(N::NtUserCallHwnd, {v, 2}) == 192);
  CHECK(a2.svc32(N::NtUserCallOneParam, {0, 9}) == 2048);
  sizes.clear();
  bridge.mouse_position(206 + 20, 225 + 15);      // (units: device (452, 480), the aware window's client (40, 230))
  {
    Th::M m;
    bool got = false;
    for (int i = 0; i < 16 && a2.peek(&m); ++i)
      if (m.message == WM_MOUSEMOVE && m.hwnd == v) {      // (client origin: device (400 + 12, 200 + 50))
        got = true;
        CHECK((m.lparam & 0xFFFF) == 40 && ((m.lparam >> 16) & 0xFFFF) == 230 && m.x == 452 && m.y == 480);
      }
    CHECK(got);
  }
  {      // drawn 1:1 on the device
    std::vector<uint32_t> target(2048 * 1536, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.pitch = 2048;
    ts.height = 1536;
    graphics::SoftwareRenderer2D device(ts);
    uint32_t dw = 0, dh = 0;
    CHECK(bridge.draw_window_scaled(v, device, 412, 450, 200, &dw, &dh) && dw == (600 - 24) / 2 && dh == (400 - 62) / 2);
  }
  // --- DPI hosting. Default: an unaware program's child of the aware window is of the aware window's awareness; SetParent of an unaware child into it
  // is refused. Mixed: the child keeps its own awareness and its own surface, which win32k scales into the aware window's surface.
  {
    Program p97(97), p98(98);
    Th d3(p97, 97, kScratch), m4(p98, 98, kScratch);
    d3.proc = def_proc;
    m4.proc = def_proc;
    CHECK(d3.atom_class("DpiChild") >= 0xC000 && m4.atom_class("DpiChild") >= 0xC000 && t.atom_class("DpiChild") >= 0xC000);
    const uint32_t dc_child = d3.create("DpiChild", WS_CHILD | WS_VISIBLE, 0, 0, 30, 20, v, 4 /* WS_EX_NOPARENTNOTIFY: the parent is another thread */, "default");
    CHECK(dc_child != 0);
    {
      user::Window* w = user::find_window(dc_child);
      CHECK(w && w->dpi_virtual == 0 && !w->dpi_mixed && user::window_dpi_hosting(dc_child) == 0);
    }
    const uint32_t u_child = t.create("DpiChild", WS_CHILD | WS_VISIBLE, 5, 5, 20, 20, u, 4, "unaware child");
    CHECK(u_child != 0 && user::find_window(u_child)->dpi_virtual == 1);
    CHECK(t.svc32(N::NtUserSetParent, {u_child, v}) == 0);      // (default hosting: refused)
    CHECK(user::find_window(u_child) && user::find_window(u_child)->parent == user::find_window(u));
    // mixed
    CHECK(m4.svc(N::NtUserCallTwoParam, {1, 1, 0x7C05}) == 0 && m4.svc(N::NtUserCallTwoParam, {0, 0, 0x7C05}) == 1);
    CHECK(int32_t(m4.svc(N::NtUserCallTwoParam, {1, 7, 0x7C05})) == -1);      // (not a behavior)
    const uint32_t mixed = m4.create("DpiChild", WS_CHILD | WS_VISIBLE, 60, 40, 50, 40, v, 4, "mixed");      // (clear of the other child)
    CHECK(mixed != 0);
    pump(m4);
    pump(a2);
    user::Window* mw = user::find_window(mixed);
    CHECK(mw && mw->dpi_virtual == 1 && mw->dpi_mixed && user::window_dpi_hosting(mixed) == 1);
    Rc cr2 = m4.client_rect(mixed);
    CHECK(cr2.r == 50 && cr2.b == 40);      // (its units)
    gdi::Rect mrect;
    CHECK(user::window_screen_rect(mixed, &mrect) && mrect.right - mrect.left == 100 && mrect.bottom - mrect.top == 80);      // (device pixels)
    uint32_t cw = 0, ch = 0;
    CHECK(mw->child_surface == 0 || (gdi::window_surface_size(mw->child_surface, &cw, &ch) && cw == 50 && ch == 40));
    // it draws in its units: its pixels show in the aware window's surface twice as large, where it is
    {
      const uint32_t dc = m4.svc32(N::NtUserGetDC, {mixed});
      const uint64_t brush = m4.svc(N::NtGdiCreateSolidBrush, {0x0000FF, 0});
      m4.svc(N::NtGdiSelectBrush, {dc, brush});
      CHECK(m4.svc32(N::NtGdiPatBlt, {dc, 0, 0, 10, 10, 0xF00021}) == 1);
      m4.svc(N::NtUserReleaseDC, {dc});
    }
    CHECK(mw->child_surface != 0 && gdi::window_surface_size(mw->child_surface, &cw, &ch) && cw == 50 && ch == 40);
    user::TopLevelInfo vi;
    CHECK(user::top_level_info(v, &vi, nullptr) && vi.dpi_aware);
    uint32_t vw = 0, vh = 0;
    const ArrayView<uint32_t> vp = gdi::window_surface_pixels(vi.surface_id, &vw, &vh);
    const int32_t sx = mrect.left - vi.surface.left, sy = mrect.top - vi.surface.top;
    CHECK(sx >= 0 && sy >= 0 && sx + 21 < int32_t(vw) && sy + 21 < int32_t(vh));
    if (sx >= 0 && sy >= 0 && sx + 21 < int32_t(vw) && sy + 21 < int32_t(vh)) {
      auto at = [&](int32_t x, int32_t y) { return vp[uint64_t(y) * vw + uint64_t(x)] & 0xFFFFFF; };
      CHECK(at(sx, sy) == 0xFF0000 && at(sx + 19, sy + 19) == 0xFF0000 && at(sx + 20, sy + 20) != 0xFF0000);
    }
    // the aware window repaints over it: the mixed child goes back on top
    {
      const uint32_t dc = a2.svc32(N::NtUserGetDCEx, {v, 0, 0});
      const uint64_t brush = a2.svc(N::NtGdiCreateSolidBrush, {0x00FF00, 0});
      a2.svc(N::NtGdiSelectBrush, {dc, brush});
      a2.svc32(N::NtGdiPatBlt, {dc, 0, 0, 400, 300, 0xF00021});
      a2.svc(N::NtUserReleaseDC, {dc});
      if (sx >= 0 && sy >= 0 && sx + 21 < int32_t(vw) && sy + 21 < int32_t(vh)) CHECK((vp[uint64_t(sy + 5) * vw + uint64_t(sx + 5)] & 0xFFFFFF) == 0xFF0000);
    }
    const bool inside = sx >= 0 && sy >= 0 && sx + 100 <= int32_t(vw) && sy + 80 <= int32_t(vh);
    CHECK(inside);
    auto shown = [&](int32_t x, int32_t y) { return inside ? vp[uint64_t(sy + y) * vw + uint64_t(sx + x)] & 0xFFFFFF : 0xDEADu; };
    // a GPU present to it goes on its own surface, in its units, and shows scaled like its drawing
    {
      const uint8_t picture[16] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};      // B, G, R, A: red, green / blue, white
      CHECK(user::present_to_window_locked(mixed, ConstByteView(picture, 16), 2, 2, 8, false));
      uint32_t pw = 0, ph = 0;
      const ArrayView<uint32_t> mp = gdi::window_surface_pixels(mw->child_surface, &pw, &ph);
      CHECK(pw == 50 && (mp[0] & 0xFFFFFF) == 0xFF0000 && (mp[uint64_t(39) * pw + 49] & 0xFFFFFF) == 0xFFFFFF);
      CHECK(shown(0, 0) == 0xFF0000 && shown(99, 0) == 0x00FF00 && shown(0, 79) == 0x0000FF && shown(99, 79) == 0xFFFFFF);
    }
    // its window region (its units) shapes its scaled pixels: the aware window's own picture shows where the region leaves out
    {
      const uint32_t rgn = m4.svc32(N::NtGdiCreateRectRgn, {0, 0, 25, 40});
      CHECK(m4.svc32(N::NtUserSetWindowRgn, {mixed, rgn, 0}) != 0);
      pump(m4);
      const uint32_t dc = a2.svc32(N::NtUserGetDCEx, {v, 0, 0});
      const uint64_t brush = a2.svc(N::NtGdiCreateSolidBrush, {0x00FFFF, 0});
      a2.svc(N::NtGdiSelectBrush, {dc, brush});
      a2.svc32(N::NtGdiPatBlt, {dc, 0, 0, 400, 300, 0xF00021});
      a2.svc(N::NtUserReleaseDC, {dc});
      CHECK(shown(5, 5) == 0xFF0000 && shown(49, 79) == 0x0000FF && shown(50, 5) == 0xFFFF00 && shown(99, 79) == 0xFFFF00);
    }
    // a top-level again: no longer mixed
    CHECK(m4.svc32(N::NtUserSetParent, {mixed, 0}) != 0 || true);
    m4.svc(N::NtUserDestroyWindow, {mixed});
    d3.svc(N::NtUserDestroyWindow, {dc_child});
    t.svc(N::NtUserDestroyWindow, {u_child});
    pump(t);
    pump(a2);
  }

  // --- 150 %: the mapping rounds units up and device pixels down, so the unaware program still reads back what it set
  one[0].scale_percent = 150;
  sizes.clear();
  bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  pump(t);
  pump(a2);
  pump(a2);
  CHECK(gdi::screen_width() == 1536 && user_misc::system_dpi() == 144);
  // the per-monitor aware window heard WM_DPICHANGED (144 dpi, its place in units suggested), and DefWindowProc put it there; the unaware one did not
  {
    bool told = false;
    for (const Call_& c : sizes)
      if (c.msg == 0x2E0) {
        CHECK(c.hwnd == v && c.wparam == (144u | 144u << 16) && rd(c.payload, 0) == 300 && rd(c.payload, 4) == 150 && rd(c.payload, 8) == 750 && rd(c.payload, 12) == 450);
        told = true;
      }
    CHECK(told);
    wr = a2.window_rect(v);
    CHECK(wr.l == 300 && wr.t == 150 && wr.r == 750 && wr.b == 450);
    CHECK(bridge.window_info(v, &info, nullptr) && info.window.x == 200 && info.window.y == 100 && info.window.width == 300 && info.window.height == 200);
    CHECK(a2.svc32(N::NtUserCallHwnd, {v, 2}) == 144);
    wr = t.window_rect(u);      // (unaware: the same units)
    CHECK(wr.l == 50 && wr.t == 60 && wr.r == 350 && wr.b == 260);
  }
  CHECK(t.svc32(N::NtUserCallOneParam, {0 /* SM_CXSCREEN */, 9}) == 1024);
  const uint32_t f = t.create("DpiClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 33, 47, 301, 199, 0, 0, "Fractional");
  CHECK(f != 0);
  pump(t);
  wr = t.window_rect(f);
  CHECK(wr.l == 33 && wr.t == 47 && wr.r == 334 && wr.b == 246);
  {
    user::Window* w = user::find_window(f);
    CHECK(w && w->bounds.left == 50 && w->bounds.top == 71 && w->bounds.right == 50 + 452);      // (ceil(33 x 1.5), ceil(47 x 1.5), and the width ceil(301 x 1.5))
  }
  CHECK(t.svc32(N::NtUserSetWindowPos, {f, 0, 7, 9, 251, 151, 0x14}) == 1);
  pump(t);
  wr = t.window_rect(f);
  CHECK(wr.l == 7 && wr.t == 9 && wr.r == 258 && wr.b == 160);
  CHECK(bridge.window_info(f, &info, nullptr) && info.window.x == 7 && info.window.y == 9 && info.window.width == 251);
  CHECK(user::top_level_info(f, &ti, nullptr) && gdi::window_surface_size(ti.surface_id, &sw, &sh));
  cr = t.client_rect(f);
  CHECK(int32_t(sw) >= cr.r && int32_t(sw) <= cr.r + 1);      // (its surface covers its client area in its units)
  bridge.mouse_position(info.surface.x + 10, info.surface.y + 12);
  {
    Th::M m;
    bool got = false;
    for (int i = 0; i < 16 && t.peek(&m); ++i)
      if (m.message == WM_MOUSEMOVE && m.hwnd == f) {
        got = true;
        const int32_t lx = int16_t(m.lparam & 0xFFFF), ly = int16_t((m.lparam >> 16) & 0xFFFF);
        CHECK(lx >= 9 && lx <= 10 && ly >= 11 && ly <= 12);      // (a unit, give or take the rounding of the client origin)
      }
    CHECK(got);
  }
  bridge.detach();
  one[0].scale_percent = 100;
  user::set_device_scale_from_desktop(100);
  user::reset_state();
  gdi::shutdown();
}

// A system-aware program keeps the system DPI it started at (96 here). After the scale grows to 200 % its windows are virtualized by the ratio, as
// Windows enlarges them: its coordinates stay what they were, and the desktop draws its pixels twice as large.
void test_system_aware_scale_change() {
  Program prog(97), late_prog(98);
  Th t(prog, 97, kScratch);
  Th l(late_prog, 98, kScratch);
  t.proc = [&](Th& th, Call_& c) -> uint64_t { return def_proc(th, c); };
  l.proc = t.proc;
  auto pump = [&](Th& th) {
    Th::M m;
    for (int i = 0; i < 64 && th.peek(&m); ++i) th.svc(N::NtUserDispatchMessage, {th.tmp(0xA00)});
  };
  DesktopBridge bridge;
  bridge.attach(6, 19);
  desktop::IWindowsSession::Monitor one[1];
  one[0].rect = {0, 0, 1024, 768};
  one[0].work = {0, 0, 1024, 740};
  one[0].scale_percent = 100;
  bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  CHECK(t.svc32(N::NtUserSetProcessDpiAwarenessContext, {0x11, 0}) == 1);      // (system aware, at 96 dpi)
  CHECK(t.atom_class("SysClass") >= 0xC000);
  const uint32_t w = t.create("SysClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, 0, 0, "System aware");
  CHECK(w != 0);
  pump(t);
  user::TopLevelInfo ti;
  uint32_t sw0 = 0, sh0 = 0;
  CHECK(user::top_level_info(w, &ti, nullptr) && ti.dpi_aware && ti.surface_scale == 100 && gdi::window_surface_size(ti.surface_id, &sw0, &sh0));

  one[0].scale_percent = 200;
  bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  pump(t);
  pump(t);
  CHECK(gdi::screen_width() == 2048 && user_misc::system_dpi() == 192);
  // its numbers, DPI and surface as before; twice the device pixels
  Rc wr = t.window_rect(w);
  CHECK(wr.l == 100 && wr.t == 100 && wr.r == 500 && wr.b == 400);
  {
    user::Window* x = user::find_window(w);
    CHECK(x && x->bounds.left == 200 && x->bounds.right == 1000 && user::window_scale(x) == 200);
  }
  CHECK(t.svc32(N::NtUserCallHwnd, {w, 2 /* GetDpiForWindow */}) == 96);
  CHECK(t.svc32(N::NtUserCallOneParam, {0 /* SM_CXSCREEN */, 9}) == 1024);
  uint32_t sw = 0, sh = 0;
  CHECK(user::top_level_info(w, &ti, nullptr) && !ti.dpi_aware && ti.surface_scale == 200 && gdi::window_surface_size(ti.surface_id, &sw, &sh) && sw == sw0 && sh == sh0);
  desktop::WindowsWindowInfo info;
  CHECK(bridge.window_info(w, &info, nullptr) && info.window.x == 100 && info.window.width == 400);
  // it draws in its pixels; the desktop shows them enlarged (in its units with the desktop's other pictures, so not drawn on the device by itself)
  {
    const uint32_t dc = t.svc32(N::NtUserGetDC, {w});
    const uint64_t brush = t.svc(N::NtGdiCreateSolidBrush, {0x0000FF, 0});
    t.svc(N::NtGdiSelectBrush, {dc, brush});
    CHECK(t.svc32(N::NtGdiPatBlt, {dc, 0, 0, 10, 10, 0xF00021}) == 1);
    t.svc(N::NtUserReleaseDC, {w, dc});
    uint32_t pw = 0, ph = 0;
    const ArrayView<uint32_t> px = gdi::window_surface_pixels(ti.surface_id, &pw, &ph);
    CHECK(pw == sw && (px[9 * pw + 9] & 0xFFFFFF) == 0xFF0000 && (px[10 * pw + 10] & 0xFFFFFF) != 0xFF0000);
    std::vector<uint32_t> small(64 * 64, 0);
    graphics::Surface ss;
    ss.pixels = ArrayView<uint32_t>(small.data(), small.size());
    ss.width = ss.height = ss.pitch = 64;
    graphics::SoftwareRenderer2D r(ss);
    uint32_t dw = 0, dh = 0;
    CHECK(!bridge.draw_window_scaled(w, r, 0, 0, 200, &dw, &dh));
  }
  // a window it makes now is of the same kind
  const uint32_t w2 = t.create("SysClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 10, 20, 200, 100, 0, 0, "Second");
  CHECK(w2 != 0);
  pump(t);
  wr = t.window_rect(w2);
  CHECK(wr.l == 10 && wr.r == 210);
  {
    user::Window* x = user::find_window(w2);
    CHECK(x && x->bounds.left == 20 && x->bounds.right == 420);
  }
  // a program that becomes system aware now works at the new system DPI: device pixels
  CHECK(l.svc32(N::NtUserSetProcessDpiAwarenessContext, {0x11, 0}) == 1);
  CHECK(l.atom_class("SysClass") >= 0xC000);
  const uint32_t w3 = l.create("SysClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 10, 20, 400, 300, 0, 0, "Late");      // (over the minimum tracking size at 192 dpi)
  CHECK(w3 != 0);
  pump(l);
  CHECK(l.svc32(N::NtUserCallHwnd, {w3, 2}) == 192);
  CHECK(user::top_level_info(w3, &ti, nullptr) && ti.dpi_aware);
  {
    user::Window* x = user::find_window(w3);
    CHECK(x && x->bounds.left == 10 && x->bounds.right == 410);
  }
  uint32_t lw0 = 0, lh0 = 0;
  CHECK(user::top_level_info(w3, &ti, nullptr) && gdi::window_surface_size(ti.surface_id, &lw0, &lh0));

  // back to 100 %: the program made aware at 192 dpi keeps working at it - its units are half device pixels - and the first program is in device pixels again
  one[0].scale_percent = 100;
  bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  pump(t);
  pump(l);
  CHECK(gdi::screen_width() == 1024 && user_misc::system_dpi() == 96);
  wr = l.window_rect(w3);
  CHECK(wr.l == 10 && wr.t == 20 && wr.r == 410 && wr.b == 320);
  {
    user::Window* x = user::find_window(w3);
    CHECK(x && x->bounds.left == 5 && x->bounds.top == 10 && x->bounds.right == 205 && user::window_scale(x) == 50);
  }
  CHECK(l.svc32(N::NtUserCallHwnd, {w3, 2}) == 192);
  CHECK(l.svc32(N::NtUserCallOneParam, {0 /* SM_CXSCREEN */, 9}) == 2048);
  CHECK(user::top_level_info(w3, &ti, nullptr) && !ti.dpi_aware && ti.surface_scale == 50 && gdi::window_surface_size(ti.surface_id, &sw, &sh) && sw == lw0 && sh == lh0);
  CHECK(bridge.window_info(w3, &info, nullptr) && info.window.x == 5 && info.window.width == 200);
  {      // the desktop shows its pixels at half the size
    std::vector<uint32_t> target(1024 * 768, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.pitch = 1024;
    ts.height = 768;
    graphics::SoftwareRenderer2D r(ts);
    uint32_t dw = 0, dh = 0;
    CHECK(bridge.draw_window(w3, r, 0, 0, &dw, &dh) && dw == (lw0 + 1) / 2 && dh == (lh0 + 1) / 2);
  }
  wr = t.window_rect(w);
  CHECK(wr.l == 100 && wr.t == 100 && wr.r == 500 && wr.b == 400);
  {
    user::Window* x = user::find_window(w);
    CHECK(x && x->bounds.left == 100 && x->bounds.right == 500 && user::window_scale(x) == 100);
  }
  // its screen DC is a shadow twice the screen's size; what it draws shows at half the size
  {
    const uint32_t dc = l.svc32(N::NtUserGetDC, {0});
    CHECK(dc != 0);
    const uint64_t brush = l.svc(N::NtGdiCreateSolidBrush, {0x00FF00, 0});
    l.svc(N::NtGdiSelectBrush, {dc, brush});
    CHECK(l.svc32(N::NtGdiPatBlt, {dc, 2044, 1532, 4, 4, 0xF00021}) == 1);
    const ConstByteView screen = gdi::screen_pixels();
    auto at = [&](int32_t px, int32_t py) { const uint64_t o = (uint64_t(py) * 1024 + uint64_t(px)) * 4; return uint32_t(screen[o]) | uint32_t(screen[o + 1]) << 8 | uint32_t(screen[o + 2]) << 16; };
    CHECK(at(1022, 766) == 0x00FF00 && at(1023, 767) == 0x00FF00 && at(1021, 766) != 0x00FF00);
    CHECK(l.svc32(N::NtUserReleaseDC, {0, dc}) == 1);
  }
  bridge.detach();
  user::set_device_scale_from_desktop(100);
  user::reset_state();
  gdi::shutdown();
}

// A device that is not a whole number of units: 400 x 400 at 150 % is 266 units, which map to 399. win32k's screen is the device's size, the monitor
// reaches its edge, and a maximized window fills it; the desktop sees the same window as the whole 266 units.
void test_device_edge() {
  Program prog(99);
  Th t(prog, 99, kScratch);
  t.proc = [&](Th& th, Call_& c) -> uint64_t { return def_proc(th, c); };
  auto pump = [&](Th& th) {
    Th::M m;
    for (int i = 0; i < 64 && th.peek(&m); ++i) th.svc(N::NtUserDispatchMessage, {th.tmp(0xA00)});
  };
  DesktopBridge bridge;
  bridge.attach(6, 19);
  desktop::IWindowsSession::Monitor one[1];
  one[0].rect = {0, 0, 266, 266};
  one[0].work = {0, 0, 266, 266};
  one[0].scale_percent = 150;
  one[0].device_width = one[0].device_height = 400;
  bridge.set_display(266, 266, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  CHECK(gdi::screen_width() == 400 && gdi::screen_height() == 400);
  CHECK(t.svc32(N::NtUserSetProcessDpiAwarenessContext, {0x22, 0}) == 1);
  CHECK(t.svc32(N::NtUserCallOneParam, {0 /* SM_CXSCREEN */, 9}) == 400 && t.svc32(N::NtUserCallOneParam, {1 /* SM_CYSCREEN */, 9}) == 400);
  CHECK(t.atom_class("EdgeClass") >= 0xC000);
  const uint32_t w = t.create("EdgeClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 10, 10, 200, 150, 0, 0, "Edge");
  CHECK(w != 0);
  pump(t);
  bridge.window_command(w, desktop::IWindowsSession::Command::Maximize);      // (the desktop's maximize button)
  pump(t);
  {
    user::Window* x = user::find_window(w);
    CHECK(x && x->bounds.left == 0 && x->bounds.top == 0 && x->bounds.right == 400 && x->bounds.bottom == 400);
  }
  desktop::WindowsWindowInfo info;
  CHECK(bridge.window_info(w, &info, nullptr) && info.maximized && info.window.x == 0 && info.window.width == 266 && info.window.height == 266);
  bridge.detach();
  user::set_device_scale_from_desktop(100);
  user::reset_state();
  gdi::shutdown();
}

// Monitors at different scales: A at 200 % (units 0..800, the primary) and B at 100 % (units 800..1600). win32k's screen is each monitor's pixels
// (A 0..1600, B 1600..2400); a window's DPI follows the monitor it is on.
void test_per_monitor_dpi() {
  Program aware_prog(102), unaware_prog(103);
  Th a(aware_prog, 102, kScratch);
  Th u(unaware_prog, 103, kScratch);
  std::vector<Call_> heard;
  auto logging = [&](Th& th, Call_& c) -> uint64_t {
    if (c.msg == 0x2E0 /* WM_DPICHANGED */) heard.push_back(c);
    return def_proc(th, c);
  };
  a.proc = logging;
  u.proc = logging;
  auto pump = [&](Th& th) {
    Th::M m;
    for (int i = 0; i < 64 && th.peek(&m); ++i) th.svc(N::NtUserDispatchMessage, {th.tmp(0xA00)});
  };
  DesktopBridge bridge;
  bridge.attach(6, 19);
  desktop::IWindowsSession::Monitor two[2];
  two[0].rect = {0, 0, 800, 600};
  two[0].work = {0, 0, 800, 600};
  two[0].scale_percent = 200;
  two[1].rect = {800, 0, 800, 600};
  two[1].work = {800, 0, 800, 600};
  two[1].scale_percent = 200;
  two[1].monitor_scale = 100;
  bridge.set_display(1600, 600, ArrayView<const desktop::IWindowsSession::Monitor>(two, 2));
  CHECK(gdi::screen_width() == 2400 && gdi::screen_height() == 1200 && user_misc::system_dpi() == 192);
  CHECK(user_misc::monitor_scale_at(100, 100) == 200 && user_misc::monitor_scale_at(1700, 100) == 100);
  CHECK(user_misc::dpi_for_monitor(user_misc::monitor_from_point(1700, 100, 2)) == 96 && user_misc::dpi_for_monitor(user_misc::monitor_from_point(100, 100, 2)) == 192);
  CHECK(a.svc32(N::NtUserSetProcessDpiAwarenessContext, {0x22, 0}) == 1);
  CHECK(a.atom_class("PmClass") >= 0xC000 && u.atom_class("PmClass") >= 0xC000);

  // a per-monitor aware window on A: 192 dpi
  const uint32_t pm = a.create("PmClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 600, 400, 0, 0, "Aware");
  CHECK(pm != 0);
  pump(a);
  List<uint32_t> changed(*system_allocator());
  List<desktop::IWindowsSession::Repaint> repainted(*system_allocator());
  bridge.take_changes(&changed, &repainted);
  CHECK(a.svc32(N::NtUserCallHwnd, {pm, 2 /* GetDpiForWindow */}) == 192);
  // the desktop moves it to B: its frame's units there, at B's pixels; it hears WM_DPICHANGED with 96 dpi and keeps that rectangle
  heard.clear();
  bridge.move_window(pm, {900, 100, 300, 200});
  pump(a);
  bridge.take_changes(&changed, &repainted);
  pump(a);
  Rc wr = a.window_rect(pm);
  CHECK(wr.l == 1700 && wr.t == 100 && wr.r == 2000 && wr.b == 300);
  CHECK(heard.size() == 1 && heard.size() > 0 && (heard[0].wparam & 0xFFFF) == 96);
  CHECK(a.svc32(N::NtUserCallHwnd, {pm, 2}) == 96);
  desktop::WindowsWindowInfo info;
  CHECK(bridge.window_info(pm, &info, nullptr) && info.window.x == 900 && info.window.width == 300 && info.window.height == 200);
  // it moves itself back to A (its pixels, the size it had on B): it is asked to take 192 dpi at twice the size, and DefWindowProc does
  heard.clear();
  CHECK(a.svc32(N::NtUserSetWindowPos, {pm, 0, 100, 100, 300, 200, 0x14}) == 1);
  pump(a);
  bridge.take_changes(&changed, &repainted);
  pump(a);
  wr = a.window_rect(pm);
  CHECK(wr.l == 100 && wr.t == 100 && wr.r == 700 && wr.b == 500);
  CHECK(heard.size() == 1 && heard.size() > 0 && (heard[0].wparam & 0xFFFF) == 192);
  CHECK(a.svc32(N::NtUserCallHwnd, {pm, 2}) == 192);
  // its surface is A's pixels: on A's device 1:1, not on B's
  {
    std::vector<uint32_t> big(1600 * 1200, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(big.data(), big.size());
    ts.width = ts.pitch = 1600;
    ts.height = 1200;
    graphics::SoftwareRenderer2D device(ts);
    uint32_t dw = 0, dh = 0;
    CHECK(bridge.draw_window_scaled(pm, device, 0, 0, 200, &dw, &dh) && !bridge.draw_window_scaled(pm, device, 0, 0, 100, &dw, &dh));
  }

  // an unaware window keeps its size in its coordinates on either monitor (Windows enlarges it on A, not on B)
  const uint32_t uw = u.create("PmClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 150, 0, 0, "Unaware");
  CHECK(uw != 0);
  pump(u);
  bridge.take_changes(&changed, &repainted);
  wr = u.window_rect(uw);
  CHECK(wr.r - wr.l == 200 && wr.b - wr.t == 150);
  {
    user::Window* x = user::find_window(uw);
    CHECK(x && x->bounds.right - x->bounds.left == 400);      // (A's pixels)
  }
  bridge.move_window(uw, {1000, 50, 200, 150});
  pump(u);
  bridge.take_changes(&changed, &repainted);
  pump(u);
  wr = u.window_rect(uw);
  CHECK(wr.r - wr.l == 200 && wr.b - wr.t == 150);
  {
    user::Window* x = user::find_window(uw);
    CHECK(x && x->bounds.left == 1800 && x->bounds.right - x->bounds.left == 200);      // (B's pixels)
  }
  CHECK(bridge.window_info(uw, &info, nullptr) && info.window.x == 1000 && info.window.width == 200);
  // a second window of the same program, on A: each window reads back at its own size, in its own monitor's coordinates
  const uint32_t uw2 = u.create("PmClass", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 150, 0, 0, "Second");      // (B's coordinates: its program's)
  CHECK(uw2 != 0);
  pump(u);
  bridge.take_changes(&changed, &repainted);
  pump(u);
  {
    user::Window* x = user::find_window(uw2);
    CHECK(x && user::monitor_scale_of(x) == 200 && x->bounds.right - x->bounds.left == 400);      // (on A, enlarged to keep its size)
  }
  wr = u.window_rect(uw2);
  CHECK(wr.r - wr.l == 200 && wr.b - wr.t == 150);
  wr = u.window_rect(uw);
  CHECK(wr.r - wr.l == 200 && wr.b - wr.t == 150);

  // the cursor: units in, the monitor's pixels inside, units back
  bridge.mouse_position(1000, 100);
  int32_t cx = 0, cy = 0;
  user::cursor_position(&cx, &cy);
  CHECK(cx == 1800 && cy == 100);
  bridge.cursor(&cx, &cy);
  CHECK(cx == 1000 && cy == 100);
  bridge.mouse_position(400, 300);
  user::cursor_position(&cx, &cy);
  CHECK(cx == 800 && cy == 600);
  bridge.detach();
  user::set_device_scale_from_desktop(100);
  user::reset_state();
  gdi::shutdown();
}

void test_redirection() {
  Program prog(91);
  Th t(prog, 91, kScratch);
  t.proc = def_proc;
  auto pump = [&]() {
    Th::M m;
    for (int i = 0; i < 64 && t.peek(&m); ++i) t.svc(N::NtUserDispatchMessage, {t.tmp(0xA00)});
    for (int i = 0; i < 8; ++i) {      // (WM_PAINT is not in the queue: it comes while a window needs painting)
      if (!t.peek(&m, 0, WM_PAINT, WM_PAINT)) break;
      t.svc(N::NtUserDispatchMessage, {t.tmp(0xA00)});
    }
  };
  // the program's red fill at client (x, y) w x h
  auto fill = [&](uint32_t hwnd, int x, int y, int w, int h) {
    const uint32_t dc = t.svc32(N::NtUserGetDC, {hwnd});
    const uint64_t brush = t.svc(N::NtGdiCreateSolidBrush, {0x0000FF, 0});
    t.svc(N::NtGdiSelectBrush, {dc, brush});
    CHECK(t.svc32(N::NtGdiPatBlt, {dc, uint64_t(uint32_t(x)), uint64_t(uint32_t(y)), uint64_t(uint32_t(w)), uint64_t(uint32_t(h)), 0xF00021}) == 1);
    t.svc(N::NtUserReleaseDC, {dc});
  };
  auto pixel = [&](uint32_t hwnd, int x, int y) -> uint32_t {      // the surface at (x, y)
    user::TopLevelInfo info;
    if (!user::top_level_info(hwnd, &info, nullptr) || info.surface_id == 0) return 0xDEAD;
    uint32_t w = 0, h = 0;
    const ArrayView<uint32_t> px = gdi::window_surface_pixels(info.surface_id, &w, &h);
    return x >= 0 && y >= 0 && uint32_t(x) < w && uint32_t(y) < h ? px[uint64_t(y) * w + uint32_t(x)] & 0xFFFFFF : 0xDEAD;
  };
  List<uint32_t> changed(*system_allocator());
  List<desktop::IWindowsSession::Repaint> repainted(*system_allocator());

  DesktopBridge bridge;
  bridge.attach(6, 19);
  const desktop::IWindowsSession::Monitor one[1] = {{{0, 0, 1024, 768}, {0, 0, 1024, 740}}};
  bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
  CHECK(t.atom_class("Redir") >= 0xC000);
  const uint32_t a = t.create("Redir", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, 0, 0, "First");
  CHECK(a != 0);
  pump();
  bridge.take_changes(&changed, &repainted);
  CHECK(has(changed, a));
  desktop::WindowsWindowInfo info;
  String title(*system_allocator());
  CHECK(bridge.window_info(a, &info, &title) && info.visible && info.caption && info.sizable && info.minimize_box && info.maximize_box && !info.popup);
  CHECK(title.view() == StringView("First"));
  CHECK(info.window.x == 100 && info.window.y == 100 && info.window.width == 400 && info.window.height == 300);
  CHECK(info.surface.x == 106 && info.surface.y == 125 && info.surface.width == 388 && info.surface.height == 269);      // inside the desktop's frame
  // the non-client sizes are the desktop's frame: the client area is the surface
  const Rc wr = t.window_rect(a), cr = t.client_rect(a);
  CHECK(wr.l == 100 && wr.t == 100 && wr.r == 500 && wr.b == 400 && cr.r == 388 && cr.b == 269);
  List<uint32_t> all(*system_allocator());
  bridge.windows(&all);
  CHECK(all.count() == 1 && all[0] == a);

  // drawing lands in the window's own surface, at client coordinates, and is noted for the desktop
  fill(a, 0, 0, 10, 10);
  bridge.take_changes(&changed, &repainted);
  CHECK(has_repaint(repainted, a) && pixel(a, 0, 0) == 0xFF0000 && pixel(a, 9, 9) == 0xFF0000 && pixel(a, 10, 10) != 0xFF0000);

  // a second window over the first: neither clips the other (the desktop composites them)
  const uint32_t b = t.create("Redir", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 150, 150, 300, 200, 0, 0, "Second");
  pump();
  fill(a, 100, 100, 20, 20);      // (under b on the screen)
  CHECK(pixel(a, 110, 110) == 0xFF0000);
  bridge.windows(&all);
  CHECK(all.count() == 2 && all[0] == a && all[1] == b);      // bottom first

  // the user moves the first window: the program hears of it, nothing is repainted
  bridge.take_changes(&changed, &repainted);
  t.clear_log();
  bridge.move_window(a, desktop::WindowsRect{200, 150, 400, 300});
  pump();
  bridge.take_changes(&changed, &repainted);
  const Rc moved = t.window_rect(a);
  CHECK(moved.l == 200 && moved.t == 150 && moved.r == 600 && moved.b == 450);
  CHECK(t.saw(WM_WINDOWPOSCHANGED) && t.saw(WM_MOVE) && !t.saw(WM_PAINT) && !t.saw(WM_SIZE));
  CHECK(has(changed, a) && !has_repaint(repainted, a) && pixel(a, 0, 0) == 0xFF0000);      // the picture went along as it was
  // ... and sizes it: a new surface of the new size, painted again
  t.clear_log();
  bridge.move_window(a, desktop::WindowsRect{200, 150, 500, 350});
  pump();
  CHECK(t.saw(WM_SIZE) && t.saw(WM_PAINT));
  user::TopLevelInfo ti;
  uint32_t sw = 0, sh = 0;
  CHECK(user::top_level_info(a, &ti, nullptr) && gdi::window_surface_size(ti.surface_id, &sw, &sh) && sw == 488 && sh == 319);

  // activation both ways
  bridge.activate_window(a);
  CHECK(bridge.window_info(a, &info, nullptr) && info.active);
  bridge.activate_window(0);
  CHECK(bridge.window_info(a, &info, nullptr) && !info.active && user::foreground_window() == 0);
  // maximize fills the work area exactly (the desktop's maximized frame)
  bridge.window_command(a, desktop::IWindowsSession::Command::Maximize);
  pump();
  CHECK(bridge.window_info(a, &info, nullptr) && info.maximized && info.window.x == 0 && info.window.y == 0 && info.window.width == 1024 && info.window.height == 740);
  bridge.window_command(a, desktop::IWindowsSession::Command::Restore);
  pump();
  CHECK(bridge.window_info(a, &info, nullptr) && !info.maximized && info.window.x == 200 && info.window.width == 500);
  // stacking: the program's SetWindowPos reaches the desktop, and a window it owns stays above it
  auto index_in = [&](const List<uint32_t>& l, uint32_t v) { for (uint64_t i = 0; i < l.count(); ++i) if (l[i] == v) return int64_t(i); return int64_t(-1); };
  const uint32_t dlg = t.create("Redir", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 260, 200, 200, 120, a, 0, "Dialog");      // (owned by a)
  pump();
  CHECK(t.svc32(N::NtUserSetWindowPos, {b, 0, 0, 0, 0, 0, 0x13}) == 1);      // HWND_TOP, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
  bridge.windows(&all);
  CHECK(index_in(all, b) == int64_t(all.count()) - 1);
  bridge.take_changes(&changed, &repainted);
  CHECK(t.svc32(N::NtUserSetWindowPos, {a, 0, 0, 0, 0, 0, 0x13}) == 1);
  bridge.take_changes(&changed, &repainted);
  bridge.windows(&all);
  CHECK(has(changed, a) && index_in(all, dlg) == int64_t(all.count()) - 1 && index_in(all, a) == int64_t(all.count()) - 2 && index_in(all, b) < index_in(all, a));
  bridge.take_changes(&changed, &repainted);
  bridge.window_command(dlg, desktop::IWindowsSession::Command::Close);
  pump();

  // a press the program reports as on its caption or a border: the desktop is asked to drag the window
  uint32_t edge = 0;
  CHECK(bridge.take_drag(&edge) == 0);
  t.post(a, 0xA1, 2, 0);      // WM_NCLBUTTONDOWN, HTCAPTION
  pump();
  CHECK(bridge.take_drag(&edge) == a && edge == 9 && bridge.take_drag(&edge) == 0);
  t.post(a, 0xA1, 17, 0);     // HTBOTTOMRIGHT
  pump();
  CHECK(bridge.take_drag(&edge) == a && edge == 8);

  // the desktop's file dragged over the window: taken only by a window that accepts files, dropped as WM_DROPFILES whose ticket user32 turns into the
  // DROPFILES block (its Windows name: the user's files are under C:\Users\Public\Desktop)
  {
    pump();
    const Rc ar = t.window_rect(a);
    const int32_t px = ar.l + 150, py = ar.t + 100;
    const Rc ac = t.client_rect(a);
    (void)ac;
    CHECK(!bridge.desktop_drag(a, px, py, "/Desktop/Documents/a b.txt"));      // (no WS_EX_ACCEPTFILES)
    user::find_window(a)->ex_style |= 0x10;
    CHECK(!bridge.desktop_drag(a, px, py, "some text"));
    CHECK(!bridge.desktop_drag(a, px, py, "/Linux/root/x.txt"));               // (Windows programs cannot see it)
    CHECK(!bridge.desktop_drag(b, px, py, "/Desktop/Documents/a b.txt"));      // (the point is not b's)
    CHECK(!bridge.desktop_drag(a, ar.r + 5, py, "/Desktop/Documents/a b.txt"));
    CHECK(bridge.desktop_drag(a, px, py, "/Desktop/Documents/a b.txt"));
    bridge.desktop_drag_end();
    CHECK(!bridge.desktop_drop());      // (ended: nothing to drop)
    CHECK(bridge.desktop_drag(a, px, py, "/Desktop/Documents/a b.txt") && bridge.desktop_drop());
    Th::M m;
    CHECK(t.peek(&m, 0, 0x233, 0x233) && m.hwnd == a && m.lparam == user::kDropTicketMark && m.wparam != 0);
    const uint64_t ticket = m.wparam;
    const std::u16string name = u"C:\\Users\\Public\\Desktop\\Documents\\a b.txt";
    const uint64_t size = t.svc(N::NtUserCallTwoParam, {ticket, 0, 0x7C00});
    CHECK(size == 20 + (name.size() + 2) * 2);
    const uint64_t block = t.tmp(0x1000);
    CHECK(t.svc(N::NtUserCallTwoParam, {ticket, block, 0x7C00}) == size);
    gdi::Rect client;
    CHECK(user::window_client_screen_rect(a, &client));
    CHECK(t.mem().get32(block) == 20 && int32_t(t.mem().get32(block + 4)) == px - client.left && int32_t(t.mem().get32(block + 8)) == py - client.top);
    CHECK(t.mem().get32(block + 12) == 0 && t.mem().get32(block + 16) == 1);      // in the client area, wide names
    bool same = true;
    for (uint64_t i = 0; i < name.size(); ++i) same &= t.mem().get16(block + 20 + i * 2) == name[i];
    CHECK(same && t.mem().get16(block + 20 + name.size() * 2) == 0 && t.mem().get16(block + 22 + name.size() * 2) == 0);
    CHECK(t.svc(N::NtUserCallTwoParam, {ticket, 0, 0x7C00}) == 0);      // (taken once)
    CHECK(t.svc(N::NtUserCallTwoParam, {12345, 0, 0x7C00}) == 0);
    // a child that does not take files: its parent does; a disabled one: none
    const uint32_t child = t.create("Redir", WS_CHILD | WS_VISIBLE, 10, 10, 100, 80, a, 0, "Child");
    pump();
    gdi::Rect cs;
    CHECK(user::window_screen_rect(child, &cs));
    user::DropTarget target;
    CHECK(user::drop_target(a, cs.left + 5, cs.top + 5, &target) && target.hwnd == a);
    user::find_window(child)->ex_style |= 0x10;
    CHECK(user::drop_target(a, cs.left + 5, cs.top + 5, &target) && target.hwnd == child && target.x == 5 && target.y == 5 && !target.non_client);
    user::find_window(a)->style |= 0x08000000u;      // WS_DISABLED
    user::find_window(child)->ex_style &= ~0x10u;
    CHECK(!user::drop_target(a, cs.left + 5, cs.top + 5, &target));
    user::find_window(a)->style &= ~0x08000000u;
    user::find_window(a)->ex_style &= ~0x10u;
    // malformed name lists are refused
    const char16_t bad[3] = {u'x', 0, u'y'};
    CHECK(!user::drop_files(target, ArrayView<const char16_t>(bad, 3)) && !user::drop_files(target, ArrayView<const char16_t>(bad, 1)));
    // many drops nobody takes: the oldest go, the table stays bounded
    user::DropTarget at{a, 1, 1, false};
    const char16_t one[4] = {u'C', u':', 0, 0};
    for (int i = 0; i < 40; ++i) CHECK(user::drop_files(at, ArrayView<const char16_t>(one, 4)));
    uint32_t live = 0;
    for (user::DropRecord& r : user::state().drops) { (void)r; ++live; }
    CHECK(live <= 16);
    for (int i = 0; i < 64 && t.peek(&m, 0, 0x233, 0x233); ++i) {}
    t.svc(N::NtUserDestroyWindow, {child});
    pump();
  }

  // touches: the window under a finger gets pointer messages (WM_TOUCH when it registered for touch); the first finger left to DefWindowProc is the mouse
  {
    pump();
    const Rc ar = t.window_rect(a);
    const int32_t px = ar.l + 150, py = ar.t + 100;
    Th::M m;
    auto pointer_messages = [&]() {
      std::vector<Th::M> got;
      for (int i = 0; i < 16 && t.peek(&m, 0, 0x240, 0x24A); ++i) got.push_back(m);
      return got;
    };
    CHECK(!bridge.touch(hardware::TouchPhase::Down, 1, a, ar.r + 50, py));      // (not over the window)
    CHECK(bridge.touch(hardware::TouchPhase::Down, 1, a, px, py));
    std::vector<Th::M> got = pointer_messages();
    CHECK(got.size() == 2 && got[0].message == 0x249 && got[1].message == 0x246 && got[1].hwnd == a);
    if (got.size() < 2) return;
    const uint32_t id = uint32_t(got[1].wparam & 0xFFFF);
    const uint32_t mflags = uint32_t(got[1].wparam >> 16);
    CHECK(id >= 2 && (mflags & 0x1) && (mflags & 0x4) && (mflags & 0x2000));      // new, in contact, primary
    CHECK(int16_t(got[1].lparam & 0xFFFF) == px && int16_t((got[1].lparam >> 16) & 0xFFFF) == py);
    const uint64_t info = t.tmp(0x1400);
    CHECK(t.svc(N::NtUserCallTwoParam, {id, info, 0x7C01}) == 1);
    CHECK(t.mem().get32(info) == 2 && t.mem().get32(info + 4) == id && t.mem().get64(info + 24) == a && int32_t(t.mem().get32(info + 32)) == px &&
          int32_t(t.mem().get32(info + 36)) == py && (t.mem().get32(info + 12) & 0x10000) && t.mem().get32(info + 88) == 1);
    CHECK(t.svc(N::NtUserCallTwoParam, {999, info, 0x7C01}) == 0);
    CHECK(user_misc::system_metrics(94, 96) == 0xC1 && user_misc::system_metrics(95, 96) == 10);
    // left to DefWindowProc: the cursor goes there and the left button goes down
    t.svc(N::NtUserMessageCall, {a, 0x246, got[1].wparam, got[1].lparam, 0, 0x29e, 0});
    int32_t cx = 0, cy = 0;
    user::cursor_position(&cx, &cy);
    CHECK(cx == px && cy == py && t.peek(&m, 0, 0x201, 0x201));      // WM_LBUTTONDOWN
    // a second finger: its own pointer, not primary, never the mouse
    CHECK(bridge.touch(hardware::TouchPhase::Down, 2, a, px + 40, py));
    got = pointer_messages();
    CHECK(got.size() == 2 && uint32_t(got[1].wparam & 0xFFFF) != id && !((got[1].wparam >> 16) & 0x2000));
    if (got.size() == 2) t.svc(N::NtUserMessageCall, {a, 0x246, got[1].wparam, got[1].lparam, 0, 0x29e, 0});
    CHECK(!t.peek(&m, 0, 0x201, 0x201));
    CHECK(bridge.touch(hardware::TouchPhase::Cancel, 2, a, px + 40, py));
    got = pointer_messages();
    CHECK(got.size() == 2 && got[0].message == 0x247 && ((got[0].wparam >> 16) & 0x8000) && got[1].message == 0x24A);      // up (cancelled), leave
    // the first moves and goes up: update, up, leave; the mouse follows and lets go
    CHECK(bridge.touch(hardware::TouchPhase::Motion, 1, a, px + 10, py + 5));
    got = pointer_messages();
    CHECK(got.size() == 1 && got[0].message == 0x245 && int16_t(got[0].lparam & 0xFFFF) == px + 10);
    if (got.size() == 1) t.svc(N::NtUserMessageCall, {a, 0x245, got[0].wparam, got[0].lparam, 0, 0x29e, 0});
    user::cursor_position(&cx, &cy);
    CHECK(cx == px + 10 && cy == py + 5);
    CHECK(bridge.touch(hardware::TouchPhase::Up, 1, a, px + 10, py + 5));
    got = pointer_messages();
    CHECK(got.size() == 2 && got[0].message == 0x247 && got[1].message == 0x24A);
    if (got.size() == 2) t.svc(N::NtUserMessageCall, {a, 0x247, got[0].wparam, got[0].lparam, 0, 0x29e, 0});
    CHECK(t.peek(&m, 0, 0x202, 0x202));      // WM_LBUTTONUP
    CHECK(t.svc(N::NtUserCallTwoParam, {id, info, 0x7C01}) == 1 && (t.mem().get32(info + 12) & 0x40000));      // (still readable after its up)
    CHECK(!bridge.touch(hardware::TouchPhase::Motion, 1, a, px, py));      // (gone)
    // a touch window (RegisterTouchWindow): WM_TOUCH with one TOUCHINPUT, read until its handle is closed (DefWindowProc closes it)
    CHECK(t.svc(N::NtUserCallTwoParam, {a, 1 | (3 << 2), 0x7C04}) == 1 && t.svc(N::NtUserCallTwoParam, {a, 2, 0x7C04}) == 4);
    CHECK(bridge.touch(hardware::TouchPhase::Down, 3, a, px, py));
    got = pointer_messages();
    CHECK(got.size() == 1 && got[0].message == 0x240 && got[0].wparam == 1 && got[0].lparam != 0);
    if (got.size() == 1) {
      const uint64_t handle = got[0].lparam;
      CHECK(t.svc(N::NtUserCallTwoParam, {handle, 0, 0x7C02}) == 1);
      const uint64_t input = t.tmp(0x1500);
      CHECK(t.svc(N::NtUserCallTwoParam, {handle, input, 0x7C02}) == 1);
      CHECK(int32_t(t.mem().get32(input)) == px * 100 && int32_t(t.mem().get32(input + 4)) == py * 100 && (t.mem().get32(input + 20) & 0x2) &&
            (t.mem().get32(input + 20) & 0x10));      // down, primary
      t.svc(N::NtUserMessageCall, {a, 0x240, got[0].wparam, got[0].lparam, 0, 0x29e, 0});
      CHECK(t.svc(N::NtUserCallTwoParam, {handle, 0, 0x7C02}) == 0 && t.svc(N::NtUserCallTwoParam, {handle, 0, 0x7C03}) == 0);      // (closed)
      CHECK(t.peek(&m, 0, 0x201, 0x201));
    }
    CHECK(bridge.touch(hardware::TouchPhase::Up, 3, a, px, py));
    got = pointer_messages();
    CHECK(got.size() == 1 && got[0].message == 0x240);
    if (got.size() == 1) {
      CHECK(t.svc(N::NtUserCallTwoParam, {got[0].lparam, 0, 0x7C03}) == 1);      // closed by the program
      CHECK(t.svc(N::NtUserCallTwoParam, {got[0].lparam, 0, 0x7C03}) == 0);
    }
    CHECK(t.svc(N::NtUserCallTwoParam, {a, 0, 0x7C04}) == 1 && t.svc(N::NtUserCallTwoParam, {a, 2, 0x7C04}) == 0);
    // many fingers nobody lifts: bounded tables; inputs nobody closes: bounded
    t.svc(N::NtUserCallTwoParam, {a, 1, 0x7C04});
    for (uint32_t i = 0; i < 300; ++i) {
      bridge.touch(hardware::TouchPhase::Down, 100 + i, a, px, py);
      bridge.touch(hardware::TouchPhase::Up, 100 + i, a, px, py);
      for (int k = 0; k < 4 && t.peek(&m, 0, 0x240, 0x24A); ++k) {}
    }
    uint32_t inputs = 0, contacts = 0;
    for (user::TouchInputRecord& r : user::state().touch_inputs) { (void)r; ++inputs; }
    for (user::TouchContact& c : user::state().touches) { (void)c; ++contacts; }
    CHECK(inputs <= 257 && contacts <= 9);
    t.svc(N::NtUserCallTwoParam, {a, 0, 0x7C04});
    pump();
  }

  // the icon: WM_SETICON's, shown by the desktop
  const uint32_t icon = user_misc::create_cursor_icon(91, true);
  CHECK(icon != 0 && t.svc(N::NtUserMessageCall, {a, 0x80, 0, icon, 0, 0x2b1, 0}) == 0);      // WM_SETICON ICON_SMALL
  CHECK(t.svc(N::NtUserMessageCall, {a, 0x7F, 0, 0, 0, 0x2b1, 0}) == icon && bridge.window_info(a, &info, nullptr) && info.icon == icon);

  // a title outside ASCII comes to the desktop in UTF-8
  {
    user::Window* wa = user::find_window(a);
    wa->text.clear();
    wa->text.append(u'Z');
    wa->text.append(char16_t(0x00FC));      // u with diaeresis
    wa->text.append(char16_t(0xD83D));      // U+1F600, a surrogate pair
    wa->text.append(char16_t(0xDE00));
    wa->text.append(char16_t(0xD800));      // a lone surrogate
  }
  CHECK(bridge.window_info(a, &info, &title) && title.view() == StringView("Z\xC3\xBC\xF0\x9F\x98\x80\xEF\xBF\xBD"));

  // the cursor: the class cursor of the window it is over, SetCursor's until it comes into a window again, hidden by SetCursor(NULL) or ShowCursor
  const uint32_t ibeam = user_misc::system_cursor(91, 32513);
  {
    const uint64_t wc = t.tmp(0x100), us = t.tmp(0x200), chars = t.tmp(0x240);
    for (uint64_t i = 0; i < 80; ++i) t.mem().put8(wc + i, 0);
    t.mem().put32(wc, 80);
    t.mem().put64(wc + 8, 0x401000);
    t.mem().put64(wc + 24, 0x400000);
    t.mem().put64(wc + 40, ibeam);      // hCursor
    t.mem().put_unicode_string(us, chars, "RedirText");
    CHECK(t.svc32(N::NtUserRegisterClassExWOW, {wc, us, 0, 0, 0, 0, 0}) >= 0xC000);
  }
  const uint32_t text = t.create("RedirText", WS_POPUP | WS_VISIBLE, 700, 500, 100, 100, 0, 0, "Text");
  pump();
  user::inject_mouse_move(750, 550);
  desktop::IWindowsSession::Pointer ptr;
  bridge.pointer(&ptr);
  uint32_t system_id = 0, pw = 0, ph = 0;
  int32_t hx = 0, hy = 0;
  List<uint32_t> argb(*system_allocator());
  CHECK(!ptr.hidden && ptr.cursor == ibeam && bridge.cursor_picture(ptr.cursor, &system_id, &argb, &pw, &ph, &hx, &hy) && system_id == 32513 && argb.count() == 0);
  t.svc(N::NtUserSetCursor, {0});
  bridge.pointer(&ptr);
  CHECK(ptr.hidden);
  user::inject_mouse_move(10, 760);      // (over nothing of the program's)
  user::inject_mouse_move(760, 560);     // back in: the class cursor again
  bridge.pointer(&ptr);
  CHECK(!ptr.hidden && ptr.cursor == ibeam);
  CHECK(t.svc(N::NtUserShowCursor, {0}) == uint64_t(uint32_t(-1)));
  bridge.pointer(&ptr);
  CHECK(ptr.hidden);
  t.svc(N::NtUserShowCursor, {1});
  // a cursor of the program's own: its picture with straight alpha and the hot spot
  {
    const uint32_t px[4] = {0x80FF0000u, 0xFF00FF00u, 0x000000FFu, 0xFFFFFFFFu};
    const uint32_t color = gdi::create_bitmap(91, 2, 2, ArrayView<const uint32_t>(px, 4));
    const uint32_t mine = user_misc::create_cursor_icon(91, false);
    const uint64_t data = t.tmp(0x1000), frame = t.tmp(0x1100);
    for (uint64_t i = 0; i < 48; ++i) t.mem().put8(data + i, 0);
    for (uint64_t i = 0; i < 40; ++i) t.mem().put8(frame + i, 0);
    t.mem().put32(data + 8, 1);         // one frame
    t.mem().put64(data + 16, frame);
    t.mem().put32(frame, 2);
    t.mem().put32(frame + 4, 2);
    t.mem().put64(frame + 8, color);
    t.mem().put32(frame + 32, 1);       // hot spot (1, 0)
    CHECK(color != 0 && mine != 0 && t.svc32(N::NtUserSetCursorIconData, {mine, 0, 0, data}) != 0);
    t.svc(N::NtUserSetCursor, {mine});
    bridge.pointer(&ptr);
    CHECK(ptr.cursor == mine && bridge.cursor_picture(mine, &system_id, &argb, &pw, &ph, &hx, &hy) && system_id == 0 && pw == 2 && ph == 2 && hx == 1 && hy == 0);
    CHECK(argb.count() == 4 && argb[0] == px[0] && argb[1] == px[1] && argb[2] == px[2] && argb[3] == px[3]);
  }

  // a GPU picture of another size is stretched over the client area
  {
    const uint8_t picture[16] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};      // B, G, R, A: red, green / blue, white
    const Rc c = t.client_rect(a);
    CHECK(user::present_to_window_locked(a, ConstByteView(picture, 16), 2, 2, 8, false));
    CHECK(pixel(a, 0, 0) == 0xFF0000 && pixel(a, c.r - 1, 0) == 0x00FF00 && pixel(a, 0, c.b - 1) == 0x0000FF && pixel(a, c.r - 1, c.b - 1) == 0xFFFFFF);
    CHECK(pixel(a, c.r / 2 - 1, c.b / 2 - 1) == 0xFF0000 && pixel(a, (c.r + 1) / 2, (c.b + 1) / 2) == 0xFFFFFF);
    CHECK(!user::present_to_window_locked(0x7777, ConstByteView(picture, 16), 2, 2, 8, false));      // (not a window: the caller shows it on the display)
  }

  // two monitors: the programs see both; a window maximizes on the one it is on
  {
    const desktop::IWindowsSession::Monitor two[2] = {{{0, 0, 512, 768}, {0, 0, 512, 740}}, {{512, 0, 512, 768}, {512, 0, 512, 768}}};
    bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(two, 2));
    const uint32_t second = user_misc::monitor_from_point(600, 10, 0);
    CHECK(second != 0 && second != user_misc::monitor_from_point(10, 10, 0) && user_misc::monitor_from_point(5000, 10, 0) == 0);
    CHECK(user_misc::monitor_from_point(5000, 10, 2) == second);      // (the nearest)
    const gdi::Rect r2 = user_misc::monitor_rect(second, false);
    CHECK(r2.left == 512 && r2.right == 1024 && user_misc::monitor_from_rect(gdi::Rect{400, 0, 700, 100}, 0) == second);      // (most of it)
    bridge.move_window(a, desktop::WindowsRect{600, 100, 300, 200});
    pump();
    bridge.window_command(a, desktop::IWindowsSession::Command::Maximize);
    pump();
    CHECK(bridge.window_info(a, &info, nullptr) && info.maximized && info.window.x == 512 && info.window.width == 512 && info.window.height == 768);
    bridge.window_command(a, desktop::IWindowsSession::Command::Restore);
    pump();
    bridge.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(one, 1));
    CHECK(user_misc::monitor_from_point(600, 10, 0) == user_misc::monitor_from_point(10, 10, 0));
  }
  (void)text;

  // close asks the program (DefWindowProc destroys it)
  bridge.take_changes(&changed, &repainted);
  bridge.window_command(b, desktop::IWindowsSession::Command::Close);
  pump();
  bridge.take_changes(&changed, &repainted);
  CHECK(has(changed, b) && !bridge.window_info(b, &info, nullptr) && !user::is_window(b));
  bridge.detach();
  user::reset_state();
  gdi::shutdown();
}

}  // namespace

int main() {
  System sys;
  test_classes();
  test_create_and_destroy();
  test_queue_basics();
  test_send_same_thread();
  test_send_between_threads();
  test_geometry_and_zorder();
  test_paint();
  test_input();
  test_hooks_and_stations();
  test_process_exit_and_bad_pointers();
  test_props_longs_text();
  test_redirection();
  test_dpi_virtualization();
  test_system_aware_scale_change();
  test_device_edge();
  test_per_monitor_dpi();
  return finish("Win32kUserTest");
}
