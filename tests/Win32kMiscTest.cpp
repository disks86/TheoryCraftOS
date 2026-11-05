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
#include "Win32kTestSupport.hpp"
#include "Process/Win32k/Win32kUserMisc.hpp"
#include "Kernel/Clipboard.hpp"

// Tests of the user-misc module of win32k (menus, cursors and icons, clipboard, accelerators, settings and metrics, monitors and display, keyboard layouts, input contexts, raw
// input, shell services, the NtUserCall* multiplexers). The window manager and the GDI module are other modules: the functions of their contracts that this module calls are
// given small stand-ins below, as WEAK symbols - when the real modules are linked their definitions replace the stand-ins and the checks that need the fake windows are skipped
// (the tests that do not need windows still run).

using namespace w32test;
using namespace process;
using namespace process::win32k;
using N = WindowsSyscallNumber;

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Stand-ins for the contracts of the other modules.
namespace {
struct FakeWindow { uint32_t hwnd; uint64_t pid, tid; };
struct Posted { uint32_t hwnd, message; uint64_t wparam, lparam; };
std::vector<FakeWindow> g_windows;
std::vector<Posted> g_posted;
int32_t g_cursor_x = 0, g_cursor_y = 0;
uint32_t g_focus = 0;

struct FakeBitmap { uint32_t id; int32_t w, h; std::vector<uint32_t> pixels; };
std::vector<FakeBitmap> g_bitmaps;
uint32_t g_screen_w = 1024, g_screen_h = 768;
const FakeWindow* find_window(uint32_t hwnd) { for (const FakeWindow& w : g_windows) if (w.hwnd == hwnd) return &w; return nullptr; }
}  // namespace

namespace process::win32k::gdi {
__attribute__((weak)) uint32_t screen_width() { return g_screen_w; }
__attribute__((weak)) uint32_t screen_height() { return g_screen_h; }
__attribute__((weak)) bool set_screen_size(uint32_t w, uint32_t h) {
  if (w < 16 || h < 16 || w > 16384 || h > 16384) return false;
  g_screen_w = w;
  g_screen_h = h;
  return true;
}
__attribute__((weak)) bool bitmap_info(uint32_t bitmap, int32_t* width, int32_t* height, uint32_t* bpp) {
  for (const FakeBitmap& b : g_bitmaps) if (b.id == bitmap) { *width = b.w; *height = b.h; *bpp = 32; return true; }
  return false;
}
__attribute__((weak)) bool bitmap_pixels(uint32_t bitmap, List<uint32_t>* out) {
  for (const FakeBitmap& b : g_bitmaps) if (b.id == bitmap) { for (uint32_t p : b.pixels) out->push_back(p); return true; }
  return false;
}
__attribute__((weak)) uint32_t create_bitmap(uint64_t, int32_t width, int32_t height, ArrayView<const uint32_t> argb) {
  FakeBitmap b{uint32_t(g_bitmaps.size() + 0x300), width, height, {}};
  for (uint32_t p : argb) b.pixels.push_back(p);
  g_bitmaps.push_back(b);
  return b.id;
}
}  // namespace process::win32k::gdi

namespace process::win32k::user {
__attribute__((weak)) bool is_window(uint32_t hwnd) { return find_window(hwnd) != nullptr; }
__attribute__((weak)) void set_window_menu(uint32_t hwnd, uint32_t menu) { (void)hwnd; (void)menu; }
__attribute__((weak)) uint64_t window_thread(uint32_t hwnd) { const FakeWindow* w = find_window(hwnd); return w ? w->tid : 0; }
__attribute__((weak)) uint64_t window_process(uint32_t hwnd) { const FakeWindow* w = find_window(hwnd); return w ? w->pid : 0; }
__attribute__((weak)) bool window_screen_rect(uint32_t hwnd, gdi::Rect* rect) {
  if (!find_window(hwnd)) return false;
  *rect = gdi::Rect{10, 20, 10 + int32_t(hwnd & 0xFF) * 4, 20 + 50};      // (the width depends on the window)
  return true;
}
__attribute__((weak)) uint32_t desktop_window() { return 0x10010; }
__attribute__((weak)) uint32_t focus_window() { return g_focus; }
__attribute__((weak)) bool post_message(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) {
  if (!find_window(hwnd)) return false;
  g_posted.push_back(Posted{hwnd, message, wparam, lparam});
  return true;
}
__attribute__((weak)) void inject_mouse_move(int32_t x, int32_t y) { g_cursor_x = x; g_cursor_y = y; }
__attribute__((weak)) void cursor_position(int32_t* x, int32_t* y) { *x = g_cursor_x; *y = g_cursor_y; }
__attribute__((weak)) bool adjust_window_rect(gdi::Rect* rect, uint32_t style, uint32_t, bool has_menu, uint32_t) {
  rect->left -= 8;
  rect->top -= (style ? 31 : 0) + (has_menu ? 20 : 0);
  rect->right += 8;
  rect->bottom += 8;
  return true;
}
}  // namespace process::win32k::user

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kNoAccess = 998;
using user_misc::kErrorMenuItemNotFound;
using user_misc::kErrorInvalidKeyboardHandle;
constexpr uint64_t kTeb = kScratch + 0xF000;      // the TEB of the programs (the last error is at +0x68)
constexpr uint64_t kA = kScratch, kB = kScratch + 0x400, kC = kScratch + 0x800, kD = kScratch + 0xC00, kE = kScratch + 0x1000, kF = kScratch + 0x2000, kG = kScratch + 0x3000,
                   kH = kScratch + 0x4000;

uint32_t last_error(Program& p) { return p.memory.get32(kTeb + 0x68); }
void clear_error(Program& p) { p.memory.put32(kTeb + 0x68, 0); }
void setup(Program& p) { p.process.set_gs_base(kTeb); }
void new_window(uint32_t hwnd, uint64_t pid, uint64_t tid = 0) { g_windows.push_back(FakeWindow{hwnd, pid, tid ? tid : pid}); }
void drop_window(uint32_t hwnd) {
  for (size_t i = 0; i < g_windows.size(); ++i) if (g_windows[i].hwnd == hwnd) { g_windows.erase(g_windows.begin() + i); break; }
  ObjectManager* objects = system_objects();
  objects->sync().lock();
  user_misc::window_destroyed(hwnd);
  objects->sync().unlock();
}
size_t posted_count(uint32_t hwnd, uint32_t message) {
  size_t n = 0;
  for (const Posted& p : g_posted) if (p.hwnd == hwnd && p.message == message) ++n;
  return n;
}
// The state of the module is read directly by a few checks: take the lock like a handler would.
struct Locked {
  Locked() { system_objects()->sync().lock(); }
  ~Locked() { system_objects()->sync().unlock(); }
};

uint64_t one(Program& p, uint32_t code, uint64_t arg = 0) { return p.call(N::NtUserCallOneParam, arg, code); }
uint64_t two(Program& p, uint32_t code, uint64_t a = 0, uint64_t b = 0) { return p.call(N::NtUserCallTwoParam, a, b, code); }
uint64_t none(Program& p, uint32_t code) { return p.call(N::NtUserCallNoParam, code); }

// --- menus
void put_mii(FakeMemory& m, uint64_t a, uint32_t mask, uint32_t type, uint32_t state, uint32_t id, uint64_t sub, uint64_t data, uint64_t type_data, uint32_t cch, uint32_t size = 80) {
  for (uint32_t i = 0; i < 80; ++i) m.put8(a + i, 0);
  m.put32(a, size); m.put32(a + 4, mask); m.put32(a + 8, type); m.put32(a + 12, state); m.put32(a + 16, id);
  m.put64(a + 24, sub); m.put64(a + 48, data); m.put64(a + 56, type_data); m.put32(a + 64, cch);
}
constexpr uint32_t kSet = 0, kInsert = 1, kRadio = 2, kDefault = 3, kItemId = 4, kInfoA = 5, kInfoW = 6, kState = 7, kSub = 8;
constexpr uint32_t kByPos = 0x400;
uint64_t thunk(Program& p, uint64_t menu, uint32_t pos, uint32_t flags, uint32_t method, uint64_t info = 0, uint64_t str = 0) {
  return p.call(N::NtUserThunkedMenuItemInfo, menu, pos, flags, method, info, str);
}
bool add_item(Program& p, uint64_t menu, uint32_t id, const char* text, uint32_t state = 0, uint64_t sub = 0, uint32_t type = 0) {
  p.memory.put_wide(kG, text);
  put_mii(p.memory, kF, 0x2 | 0x40 | 0x1 | (sub ? 4 : 0) | (type ? 0x100 : 0), type, state, id, sub, 0, kG, 0);
  return thunk(p, menu, 0xFFFFFFFFu, kByPos, kInsert, kF) == 1;
}
uint32_t item_state(Program& p, uint64_t menu, uint32_t id_or_pos, uint32_t flags) { return uint32_t(thunk(p, menu, id_or_pos, flags, kState)); }
uint32_t item_id(Program& p, uint64_t menu, uint32_t pos) { return uint32_t(thunk(p, menu, pos, kByPos, kItemId)); }

void test_menus() {
  Program p(100);
  setup(p);
  new_window(0x20001, 100);
  const uint64_t m = p.call(N::NtUserCreateMenu);
  CHECK(m != 0 && user_misc::is_menu(uint32_t(m)) && (m >> 32) == 0);
  CHECK(!user_misc::is_menu(0) && !user_misc::is_menu(uint32_t(m) + 0x10000) && !user_misc::is_menu(0x77));
  CHECK(add_item(p, m, 100, "&File") && add_item(p, m, 101, "Edit") && add_item(p, m, 0, "", 0, 0, 0x800));
  CHECK(one(p, 4, m) == 3);      // NtUserCallOneParam_GetMenuItemCount
  CHECK(uint32_t(one(p, 4, 0x1234)) == 0xFFFFFFFFu);      // not a menu
  CHECK(item_state(p, m, 101, 0) == 0 && item_state(p, m, 2, kByPos) == 0x800);
  CHECK(item_state(p, m, 999, 0) == 0xFFFFFFFFu && item_state(p, m, 7, kByPos) == 0xFFFFFFFFu);
  CHECK(item_id(p, m, 0) == 100 && item_id(p, m, 1) == 101 && item_id(p, m, 9) == 0xFFFFFFFFu);

  // insert before an item found by command, and by position; a position past the end appends
  p.memory.put_wide(kG, "First");
  put_mii(p.memory, kF, 0x2 | 0x40, 0, 0, 50, 0, 0, kG, 0);
  CHECK(thunk(p, m, 100, 0, kInsert, kF) == 1 && item_id(p, m, 0) == 50 && item_id(p, m, 1) == 100);
  put_mii(p.memory, kF, 0x2, 0, 0, 60, 0, 0, 0, 0);
  CHECK(thunk(p, m, 77, kByPos, kInsert, kF) == 1 && item_id(p, m, 4) == 60);
  CHECK(one(p, 4, m) == 5);
  put_mii(p.memory, kF, 0x2, 0, 0, 61, 0, 0, 0, 0);
  CHECK(thunk(p, m, 4, kByPos, kSet, kF) == 1 && item_id(p, m, 4) == 61);
  CHECK(thunk(p, m, 5, kByPos, kSet, kF) == 0 && last_error(p) == kErrorMenuItemNotFound);

  // bad pointer, bad size
  clear_error(p);
  CHECK(thunk(p, m, 0, kByPos, kInsert, 0x10) == 0 && last_error(p) == kNoAccess);
  put_mii(p.memory, kF, 0x2, 0, 0, 62, 0, 0, 0, 0, 41);
  clear_error(p);
  CHECK(thunk(p, m, 0, kByPos, kInsert, kF) == 0 && last_error(p) == kErrorInvalidParameter && one(p, 4, m) == 5);
  CHECK(thunk(p, 0x777, 0, kByPos, kInsert, kF) == 0 && last_error(p) == kErrorInvalidMenuHandle);

  // enable / check
  CHECK(p.call(N::NtUserEnableMenuItem, m, 100, 1) == 0 && p.call(N::NtUserEnableMenuItem, m, 100, 3) == 1);
  CHECK(item_state(p, m, 100, 0) == 3);
  CHECK(p.call(N::NtUserEnableMenuItem, m, 100, 0) == 3 && item_state(p, m, 100, 0) == 0);
  CHECK(uint32_t(p.call(N::NtUserEnableMenuItem, m, 4242, 0)) == 0xFFFFFFFFu);
  CHECK(p.call(N::NtUserCheckMenuItem, m, 101, 8) == 0 && item_state(p, m, 101, 0) == 8);
  CHECK(p.call(N::NtUserCheckMenuItem, m, 2, kByPos) == 8 && item_state(p, m, 2, kByPos) == 0);
  CHECK(uint32_t(p.call(N::NtUserCheckMenuItem, m, 999, 0)) == 0xFFFFFFFFu);

  // submenus: found by command recursively, a popup item reports its item count, cycles are refused
  const uint64_t sub = p.call(N::NtUserCreatePopupMenu);
  CHECK(sub != 0 && sub != m);
  CHECK(add_item(p, sub, 200, "Open") && add_item(p, m, 300, "Sub", 0, sub));
  CHECK(thunk(p, m, 5, kByPos, kSub) == sub && thunk(p, m, 0, kByPos, kSub) == 0);
  CHECK(item_id(p, m, 5) == 0xFFFFFFFFu);      // a popup item has no command id
  CHECK(item_state(p, m, 5, kByPos) == ((1u << 8) | 0x10));
  CHECK(item_state(p, m, 200, 0) == 0 && item_state(p, m, 201, 0) == 0xFFFFFFFFu);
  CHECK(p.call(N::NtUserCheckMenuItem, m, 200, 8) == 0 && item_state(p, sub, 200, 0) == 8);
  clear_error(p);
  CHECK(!add_item(p, sub, 400, "cycle", 0, m) && last_error(p) == kErrorInvalidParameter && one(p, 4, sub) == 1);
  clear_error(p);
  CHECK(!add_item(p, sub, 401, "bad", 0, 0x999) && last_error(p) == kErrorInvalidMenuHandle);

  // GetMenuItemInfoW / A: text is cut to the buffer, the length comes back, a missing buffer asks for the length
  p.memory.put_wide(kG, "x");
  put_mii(p.memory, kF, 0x2 | 0x1 | 0x40, 0, 0, 0, 0, 0, kE, 3);
  CHECK(thunk(p, m, 101, 0, kInfoW, kF) == 1 && p.memory.get32(kF + 16) == 101 && p.memory.get32(kF + 64) == 2 && p.memory.get_wide_z(kE) == "Ed");
  put_mii(p.memory, kF, 0x40, 0, 0, 0, 0, 0, 0, 0);
  CHECK(thunk(p, m, 101, 0, kInfoW, kF) == 1 && p.memory.get32(kF + 64) == 4);
  put_mii(p.memory, kF, 0x40, 0, 0, 0, 0, 0, kE, 10);
  for (int i = 0; i < 16; ++i) p.memory.put8(kE + i, 0xCC);
  CHECK(thunk(p, m, 101, 0, kInfoA, kF) == 1 && p.memory.get8(kE) == 'E' && p.memory.get8(kE + 3) == 't' && p.memory.get8(kE + 4) == 0 && p.memory.get32(kF + 64) == 4);
  put_mii(p.memory, kF, 0x1 | 0x4 | 0x2 | 0x100, 0, 0, 0, 0, 0, 0, 0);
  CHECK(thunk(p, m, 5, kByPos, kInfoW, kF) == 1 && p.memory.get64(kF + 24) == sub && p.memory.get32(kF + 16) == 300);
  put_mii(p.memory, kF, 0x100, 0, 0, 0, 0, 0, 0, 0);
  CHECK(thunk(p, m, 3, kByPos, kInfoW, kF) == 1 && p.memory.get32(kF + 8) == 0x800);
  put_mii(p.memory, kF, 0x40, 0, 0, 0, 0, 0, kE, 10, 41);
  clear_error(p);
  CHECK(thunk(p, m, 101, 0, kInfoW, kF) == 0 && last_error(p) == kErrorInvalidParameter);
  put_mii(p.memory, kF, 0x40, 0, 0, 0, 0, 0, kE, 10);
  clear_error(p);
  CHECK(thunk(p, m, 555, 0, kInfoW, kF) == 0 && last_error(p) == kErrorMenuItemNotFound);
  put_mii(p.memory, kF, 0x40, 0, 0, 0, 0, 0, 0x10, 10);
  clear_error(p);
  CHECK(thunk(p, m, 101, 0, kInfoW, kF) == 0 && last_error(p) == kNoAccess);

  // owner-draw item data and a text set through the UNICODE_STRING
  p.memory.put_unicode_string(kC, kD, "Owner");
  put_mii(p.memory, kF, 0x2 | 0x20 | 0x100 | 0x40, 0x100, 0, 90, 0, 0xABCDEF, 0, 0);
  CHECK(thunk(p, m, 0xFFFFFFFFu, kByPos, kInsert, kF, kC) == 1);
  put_mii(p.memory, kF, 0x20 | 0x100 | 0x40, 0, 0, 0, 0, 0, kE, 10);
  CHECK(thunk(p, m, 90, 0, kInfoW, kF) == 1 && p.memory.get64(kF + 48) == 0xABCDEF && p.memory.get32(kF + 8) == 0x100 && p.memory.get32(kF + 64) == 5);      // (an owner-draw item keeps the string it was given)
  p.call(N::NtUserDeleteMenu, m, 90, 0);

  // default item
  CHECK(p.call(N::NtUserSetMenuDefaultItem, m, 1, 1) == 1);      // by position: item 1 is command 100
  CHECK(thunk(p, m, 0, 0, kDefault) == 100 && thunk(p, m, 1, 0, kDefault) == 1);
  CHECK(item_state(p, m, 100, 0) == 0x1000);
  p.call(N::NtUserEnableMenuItem, m, 100, 1);
  CHECK(uint32_t(thunk(p, m, 0, 0, kDefault)) == 0xFFFFFFFFu && thunk(p, m, 0, 1, kDefault) == 100);      // a disabled default needs GMDI_USEDISABLED
  p.call(N::NtUserEnableMenuItem, m, 100, 0);
  clear_error(p);
  CHECK(p.call(N::NtUserSetMenuDefaultItem, m, 5000, 0) == 0 && last_error(p) == kErrorMenuItemNotFound);
  CHECK(p.call(N::NtUserSetMenuDefaultItem, m, 0xFFFFFFFFu, 0) == 1 && uint32_t(thunk(p, m, 0, 0, kDefault)) == 0xFFFFFFFFu);
  // GMDI_GOINTOPOPUPS: the default of the submenu
  p.call(N::NtUserSetMenuDefaultItem, sub, 200, 0);
  p.call(N::NtUserSetMenuDefaultItem, m, 300, 0);
  CHECK(thunk(p, m, 0, 0, kDefault) == 300 && thunk(p, m, 0, 2, kDefault) == 200);

  // radio check
  CHECK(thunk(p, m, 1, 3, kRadio, 2, 1) == 1);      // positions 1..3, check position 2
  CHECK(item_state(p, m, 1, kByPos) == 0 && item_state(p, m, 2, kByPos) == 0x208 && item_state(p, m, 3, kByPos) == 0x800);
  p.call(N::NtUserCheckMenuItem, m, 1, kByPos | 8);
  CHECK(thunk(p, m, 1, 4, kRadio, 4, 1) == 1 && item_state(p, m, 4, kByPos) == 0x208 && item_state(p, m, 1, kByPos) == 0);

  // RemoveMenu keeps the submenu, DeleteMenu destroys it
  CHECK(p.call(N::NtUserRemoveMenu, m, 300, 0) == 1 && user_misc::is_menu(uint32_t(sub)) && one(p, 4, m) == 5 + 0);
  CHECK(add_item(p, m, 301, "Sub", 0, sub));
  CHECK(p.call(N::NtUserDeleteMenu, m, 301, 0) == 1 && !user_misc::is_menu(uint32_t(sub)));
  clear_error(p);
  CHECK(p.call(N::NtUserDeleteMenu, m, 301, 0) == 0 && last_error(p) == kErrorMenuItemNotFound);

  // MENUINFO
  const uint64_t m2 = p.call(N::NtUserCreateMenu), inner = p.call(N::NtUserCreatePopupMenu);
  CHECK(add_item(p, m2, 1, "inner", 0, inner));
  p.memory.put32(kA, 40); p.memory.put32(kA + 4, 0x10 | 0x4 | 0x1 | 0x8); p.memory.put32(kA + 8, 0x04000000); p.memory.put32(kA + 12, 100); p.memory.put32(kA + 24, 77);
  p.memory.put64(kA + 32, 0x1234);
  CHECK(p.call(N::NtUserThunkedMenuInfo, m2, kA) == 1);
  for (int i = 0; i < 40; ++i) p.memory.put8(kB + i, 0);
  p.memory.put32(kB, 40); p.memory.put32(kB + 4, 0x1F);
  CHECK(two(p, 1, m2, kB) == 1 && p.memory.get32(kB + 8) == 0x04000000 && p.memory.get32(kB + 12) == 100 && p.memory.get32(kB + 24) == 77 && p.memory.get64(kB + 32) == 0x1234);
  p.memory.put32(kA + 4, 0x10); p.memory.put32(kA + 8, 0x1);
  clear_error(p);
  CHECK(p.call(N::NtUserThunkedMenuInfo, m2, kA) == 0 && last_error(p) == kErrorInvalidParameter);
  p.memory.put32(kA + 4, 0x4 | 0x80000000u); p.memory.put32(kA + 24, 5);
  CHECK(p.call(N::NtUserThunkedMenuInfo, m2, kA) == 1);
  p.memory.put32(kB + 4, 4);
  CHECK(two(p, 1, inner, kB) == 1 && p.memory.get32(kB + 24) == 5);
  p.memory.put32(kA, 39);
  clear_error(p);
  CHECK(p.call(N::NtUserThunkedMenuInfo, m2, kA) == 0 && last_error(p) == kErrorInvalidParameter);
  clear_error(p);
  CHECK(two(p, 1, m2, 0x10) == 0 && last_error(p) == kNoAccess);
  CHECK(two(p, 1, 0x4444, kB) == 0 && last_error(p) == kErrorInvalidMenuHandle);

  // the menu of a window goes with the window
  clear_error(p);
  CHECK(p.call(N::NtUserSetMenu, 0x99999, m2) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  CHECK(p.call(N::NtUserSetMenu, 0x20001, 0x88) == 0 && last_error(p) == kErrorInvalidMenuHandle);
  CHECK(p.call(N::NtUserSetMenu, 0x20001, m2) == 1 && user_misc::window_menu(0x20001) == m2);
  CHECK(p.call(N::NtUserSetMenu, 0x20001, 0) == 1 && user_misc::window_menu(0x20001) == 0);
  CHECK(p.call(N::NtUserSetMenu, 0x20001, m2) == 1);
  g_posted.clear();
  CHECK(p.call(N::NtUserDrawMenuBar, 0x20001) == 1 && posted_count(0x20001, 0x85) == 1);
  drop_window(0x20001);
  CHECK(!user_misc::is_menu(uint32_t(m2)) && !user_misc::is_menu(uint32_t(inner)));

  // the system menu
  new_window(0x20002, 100);
  const uint64_t sys = p.call(N::NtUserGetSystemMenu, 0x20002, 0);
  CHECK(sys != 0 && user_misc::is_menu(uint32_t(sys)) && one(p, 4, sys) == 7);
  CHECK(p.call(N::NtUserGetSystemMenu, 0x20002, 0) == sys && user_misc::window_system_menu(0x20002) == sys);
  CHECK(thunk(p, sys, 0, 0, kDefault) == 0xF060 && item_state(p, sys, 0xF120, 0) == 3 && item_state(p, sys, 5, kByPos) == 0x800);
  CHECK(p.call(N::NtUserGetSystemMenu, 0x20002, 1) == 0 && !user_misc::is_menu(uint32_t(sys)));
  const uint64_t sys2 = p.call(N::NtUserGetSystemMenu, 0x20002, 0);
  CHECK(sys2 != 0 && one(p, 4, sys2) == 7);
  clear_error(p);
  CHECK(p.call(N::NtUserGetSystemMenu, 0x99999, 0) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  const uint64_t custom = p.call(N::NtUserCreatePopupMenu);
  CHECK(p.call(N::NtUserSetSystemMenu, 0x20002, custom) == 1 && !user_misc::is_menu(uint32_t(sys2)) && p.call(N::NtUserGetSystemMenu, 0x20002, 0) == custom);
  drop_window(0x20002);
  CHECK(!user_misc::is_menu(uint32_t(custom)));

  // menus of a process go when it ends
  Program q(200);
  setup(q);
  const uint64_t qm = q.call(N::NtUserCreateMenu);
  CHECK(user_misc::is_menu(uint32_t(qm)));
  win32k::process_exited(200);
  CHECK(!user_misc::is_menu(uint32_t(qm)) && user_misc::is_menu(uint32_t(m)));
  p.call(N::NtUserSetMenuContextHelpId, m, 9);
  CHECK(p.call(N::NtUserDestroyMenu, m) == 1);
  clear_error(p);
  CHECK(p.call(N::NtUserDestroyMenu, m) == 0 && last_error(p) == kErrorInvalidMenuHandle);
}

// --- cursors and icons
uint32_t make_bitmap(int w, int h, uint32_t pixel) {
  std::vector<uint32_t> v(size_t(w) * h, pixel);
  return gdi::create_bitmap(100, w, h, ArrayView<const uint32_t>(v.data(), v.size()));
}

void put_desc(FakeMemory& m, uint64_t desc, uint64_t frames, uint32_t flags, uint32_t steps, uint32_t nframes, uint32_t delay, uint64_t seq, uint64_t rates, uint64_t rsrc) {
  m.put32(desc, flags); m.put32(desc + 4, steps); m.put32(desc + 8, nframes); m.put32(desc + 12, delay);
  m.put64(desc + 16, frames); m.put64(desc + 24, seq); m.put64(desc + 32, rates); m.put64(desc + 40, rsrc);
}
void put_frame(FakeMemory& m, uint64_t f, uint32_t w, uint32_t h, uint64_t color, uint64_t alpha, uint64_t mask, int32_t hx, int32_t hy) {
  m.put32(f, w); m.put32(f + 4, h); m.put64(f + 8, color); m.put64(f + 16, alpha); m.put64(f + 24, mask); m.put32(f + 32, uint32_t(hx)); m.put32(f + 36, uint32_t(hy));
}

void test_cursors() {
  Program p(100);
  setup(p);
  set_thread_override(1);
  const uint32_t color = make_bitmap(16, 16, 0xFF112233u), mask = make_bitmap(16, 16, 0xFF000000u);
  const uint64_t cur = one(p, 0, 0), icon = one(p, 0, 1);
  CHECK(cur != 0 && icon != 0 && cur != icon);
  p.memory.put_unicode_string(kC, kD, "user32.dll");
  p.memory.put16(kE, 0); p.memory.put16(kE + 2, 0); p.memory.put64(kE + 8, 100);      // an integer resource name (100)
  put_frame(p.memory, kB, 16, 16, color, 0, mask, 3, 4);
  put_desc(p.memory, kA, kB, 0, 0, 1, 0, 0, 0, 0);
  CHECK(p.call(N::NtUserSetCursorIconData, cur, kC, kE, kA) == 1);
  CHECK(p.call(N::NtUserSetCursorIconData, icon, 0, 0, kA) == 1);

  // GetIconInfo: copies of the bitmaps, the hotspot of a cursor and the centre of an icon
  p.memory.put_unicode_string(kD + 0x100, kD + 0x200, "");
  p.memory.put16(kD + 0x102, 64); p.memory.put64(kD + 0x108, kD + 0x300);      // a UNICODE_STRING with room for 32 characters
  CHECK(p.call(N::NtUserGetIconInfo, cur, kF, kD + 0x100, kE + 0x100, kG, 0) == 1);
  p.memory.put16(kE + 0x100, 0); p.memory.put16(kE + 0x102, 0); p.memory.put64(kE + 0x108, 0);
  CHECK(p.memory.get32(kF) == 0 && p.memory.get32(kF + 4) == 3 && p.memory.get32(kF + 8) == 4);
  CHECK(p.memory.get64(kF + 16) != 0 && p.memory.get64(kF + 16) != mask && p.memory.get64(kF + 24) != 0 && p.memory.get64(kF + 24) != color);
  CHECK(p.memory.get16(kD + 0x100) == 20 && p.memory.get_wide(kD + 0x300, 10) == "user32.dll" && p.memory.get32(kG) == 32);
  CHECK(p.call(N::NtUserGetIconInfo, icon, kF, 0, 0, 0, 0) == 1 && p.memory.get32(kF) == 1 && p.memory.get32(kF + 4) == 8 && p.memory.get32(kF + 8) == 8);
  CHECK(p.call(N::NtUserGetIconSize, icon, 0, kA + 0x100, kA + 0x104) == 1 && p.memory.get32(kA + 0x100) == 16 && p.memory.get32(kA + 0x104) == 16);
  clear_error(p);
  CHECK(p.call(N::NtUserGetIconInfo, 0x555, kF, 0, 0, 0, 0) == 0 && last_error(p) == kErrorInvalidCursorHandle);
  CHECK(p.call(N::NtUserGetIconInfo, icon, 0x10, 0, 0, 0, 0) == 0 && last_error(p) == kNoAccess);
  CHECK(p.call(N::NtUserGetIconSize, 0x555, 0, kA + 0x100, kA + 0x104) == 0 && last_error(p) == kErrorInvalidCursorHandle);

  // refused descriptions
  const uint64_t other = one(p, 0, 1);
  put_frame(p.memory, kB, 0, 16, color, 0, mask, 0, 0);
  clear_error(p);
  CHECK(p.call(N::NtUserSetCursorIconData, other, 0, 0, kA) == 0 && last_error(p) == kErrorInvalidParameter);
  put_frame(p.memory, kB, 16, 16, 0, 0, 0, 0, 0);
  CHECK(p.call(N::NtUserSetCursorIconData, other, 0, 0, kA) == 0);
  put_frame(p.memory, kB, 16, 16, 0x7777, 0, mask, 0, 0);
  CHECK(p.call(N::NtUserSetCursorIconData, other, 0, 0, kA) == 0);
  put_desc(p.memory, kA, kB, 0, 0, 0, 0, 0, 0, 0);
  CHECK(p.call(N::NtUserSetCursorIconData, other, 0, 0, kA) == 0);
  CHECK(p.call(N::NtUserSetCursorIconData, other, 0, 0, 0x10) == 0 && last_error(p) == kNoAccess);
  CHECK(p.call(N::NtUserSetCursorIconData, 0x555, 0, 0, kA) == 0 && last_error(p) == kErrorInvalidCursorHandle);

  // an animated cursor: steps, sequence, rates
  put_frame(p.memory, kB, 16, 16, color, 0, mask, 1, 1);
  put_frame(p.memory, kB + 40, 16, 16, color, 0, mask, 2, 2);
  p.memory.put32(kC + 0x100, 0); p.memory.put32(kC + 0x104, 1); p.memory.put32(kC + 0x108, 0);
  p.memory.put32(kC + 0x120, 10); p.memory.put32(kC + 0x124, 20); p.memory.put32(kC + 0x128, 30);
  put_desc(p.memory, kA, kB, 0, 3, 2, 5, kC + 0x100, kC + 0x120, 0);
  const uint64_t anim = one(p, 0, 0);
  CHECK(p.call(N::NtUserSetCursorIconData, anim, 0, 0, kA) == 1);
  CHECK(p.call(N::NtUserGetCursorFrameInfo, anim, 1, kA + 0x200, kA + 0x204) == anim && p.memory.get32(kA + 0x200) == 20 && p.memory.get32(kA + 0x204) == 3);
  clear_error(p);
  CHECK(p.call(N::NtUserGetCursorFrameInfo, anim, 3, kA + 0x200, kA + 0x204) == 0 && last_error(p) == kErrorInvalidParameter);
  p.memory.put32(kC + 0x108, 7);
  CHECK(p.call(N::NtUserSetCursorIconData, anim, 0, 0, kA) == 0);      // a sequence entry past the frames

  // the parameter of an icon
  p.memory.put64(kA + 0x300, 0xAAAA); p.memory.put64(kA + 0x308, 0x55);
  CHECK(two(p, 5, icon, kA + 0x300) == 0 && one(p, 3, icon) == 0x55);
  p.memory.put64(kA + 0x308, 0x66);
  CHECK(two(p, 5, icon, kA + 0x300) == 0x55 && one(p, 3, icon) == 0x66);
  CHECK(two(p, 5, 0x555, kA + 0x300) == 0 && last_error(p) == kErrorInvalidCursorHandle && one(p, 3, 0x555) == 0);

  // shared resources are found again and survive an ordinary destroy
  const uint64_t shared = one(p, 0, 0);
  put_frame(p.memory, kB, 16, 16, color, 0, mask, 0, 0);
  put_desc(p.memory, kA, kB, 0x8000, 0, 1, 0, 0, 0, 0xABC0);
  CHECK(p.call(N::NtUserSetCursorIconData, shared, kC, kE, kA) == 1);
  CHECK(p.call(N::NtUserFindExistingCursorIcon, kC, kE, 0xABC0) == shared);
  CHECK(p.call(N::NtUserFindExistingCursorIcon, kC, kE, 0xABC1) == 0);
  p.memory.put64(kE + 8, 101);
  CHECK(p.call(N::NtUserFindExistingCursorIcon, kC, kE, 0xABC0) == 0);
  p.memory.put64(kE + 8, 100);
  CHECK(p.call(N::NtUserDestroyCursor, shared, 0) == 1 && p.call(N::NtUserGetIconSize, shared, 0, kA + 0x100, kA + 0x104) == 1);
  CHECK(p.call(N::NtUserDestroyCursor, shared, 1) == 1);
  clear_error(p);
  CHECK(p.call(N::NtUserGetIconSize, shared, 0, kA + 0x100, kA + 0x104) == 0 && last_error(p) == kErrorInvalidCursorHandle);
  CHECK(p.call(N::NtUserDestroyCursor, shared, 1) == 0 && last_error(p) == kErrorInvalidCursorHandle);

  // the cursor of a thread
  CHECK(p.call(N::NtUserSetCursor, cur) == 0 && p.call(N::NtUserGetCursor) == cur);
  set_thread_override(2);
  CHECK(p.call(N::NtUserGetCursor) == 0 && p.call(N::NtUserSetCursor, icon) == 0 && p.call(N::NtUserGetCursor) == icon);
  set_thread_override(1);
  CHECK(p.call(N::NtUserGetCursor) == cur && p.call(N::NtUserSetCursor, 0) == cur && p.call(N::NtUserGetCursor) == 0);
  clear_error(p);
  CHECK(p.call(N::NtUserSetCursor, 0x555) == 0 && last_error(p) == kErrorInvalidCursorHandle);
  p.call(N::NtUserSetCursor, cur);
  CHECK(p.call(N::NtUserDestroyCursor, cur, 0) == 1 && p.call(N::NtUserGetCursor) == 0);

  // the show counter
  CHECK(uint32_t(p.call(N::NtUserShowCursor, 1)) == 1 && uint32_t(p.call(N::NtUserShowCursor, 0)) == 0 && uint32_t(p.call(N::NtUserShowCursor, 0)) == 0xFFFFFFFFu);
  p.memory.put32(kA, 24);
  CHECK(p.call(N::NtUserGetCursorInfo, kA) == 1 && p.memory.get32(kA + 4) == 0);
  p.call(N::NtUserShowCursor, 1);
  p.call(N::NtUserShowCursor, 1);
  CHECK(p.call(N::NtUserGetCursorInfo, kA) == 1 && p.memory.get32(kA + 4) == 1);
  p.memory.put32(kA, 20);
  clear_error(p);
  CHECK(p.call(N::NtUserGetCursorInfo, kA) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserGetCursorInfo, 0x10) == 0 && last_error(p) == kNoAccess);

  // position, clip rectangle
  CHECK(p.call(N::NtUserSetCursorPos, 10, 20) == 1 && p.call(N::NtUserGetCursorPos, kA) == 1 && p.memory.get32(kA) == 10 && p.memory.get32(kA + 4) == 20);
  clear_error(p);
  CHECK(p.call(N::NtUserGetCursorPos, 0x10) == 0 && last_error(p) == kNoAccess);
  p.memory.put32(kA, 100); p.memory.put32(kA + 4, 100); p.memory.put32(kA + 8, 200); p.memory.put32(kA + 12, 200);
  CHECK(p.call(N::NtUserClipCursor, kA) == 1);      // the cursor at (10,20) is pushed into the rectangle
  p.call(N::NtUserGetCursorPos, kB);
  CHECK(p.memory.get32(kB) == 100 && p.memory.get32(kB + 4) == 100);
  CHECK(p.call(N::NtUserSetCursorPos, 500, 500) == 1);
  p.call(N::NtUserGetCursorPos, kB);
  CHECK(p.memory.get32(kB) == 199 && p.memory.get32(kB + 4) == 199);
  CHECK(p.call(N::NtUserGetClipCursor, kB) == 1 && p.memory.get32(kB) == 100 && p.memory.get32(kB + 12) == 200);
  CHECK(user_misc::cursor_clip_rect().right == 200);
  p.memory.put32(kA + 8, 100);
  clear_error(p);
  CHECK(p.call(N::NtUserClipCursor, kA) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserClipCursor, 0) == 1 && p.call(N::NtUserGetClipCursor, kB) == 1);
  CHECK(int32_t(p.memory.get32(kB + 8)) == int32_t(gdi::screen_width()) && int32_t(p.memory.get32(kB + 12)) == int32_t(gdi::screen_height()));
  CHECK(p.call(N::NtUserClipCursor, 0x10) == 0 && last_error(p) == kNoAccess);

  // a system cursor takes over the image of another one
  const uint64_t donor = one(p, 0, 0);
  put_frame(p.memory, kB, 16, 16, color, 0, mask, 5, 6);
  put_desc(p.memory, kA, kB, 0, 0, 1, 0, 0, 0, 0);
  p.call(N::NtUserSetCursorIconData, donor, 0, 0, kA);
  CHECK(p.call(N::NtUserSetSystemCursor, donor, 32512) == 1 && p.call(N::NtUserGetIconSize, donor, 0, kA + 0x100, kA + 0x104) == 0);
  const uint32_t arrow = user_misc::system_cursor(100, 32512);
  CHECK(arrow != 0 && user_misc::system_cursor(100, 32512) == arrow && user_misc::system_icon(100, 32512) != arrow);
  CHECK(p.call(N::NtUserGetIconInfo, arrow, kF, 0, 0, 0, 0) == 1 && p.memory.get32(kF + 4) == 5 && p.memory.get32(kF + 8) == 6);
  CHECK(p.call(N::NtUserDestroyCursor, arrow, 1) == 0 && last_error(p) == kErrorAccessDenied);

  // the icons of a process go with it
  Program q(300);
  setup(q);
  const uint64_t qi = one(q, 0, 1);
  CHECK(q.call(N::NtUserGetIconSize, qi, 0, kA + 0x100, kA + 0x104) == 1);
  win32k::process_exited(300);
  CHECK(q.call(N::NtUserGetIconSize, qi, 0, kA + 0x100, kA + 0x104) == 0);
  CHECK(p.call(N::NtUserGetIconSize, icon, 0, kA + 0x100, kA + 0x104) == 1);
  set_thread_override(0);
}

// --- accelerators
void test_accelerators() {
  Program p(100);
  setup(p);
  const uint8_t entries[12] = {0x09, 0, 'S', 0, 100, 0, 0x81, 0, 0x70, 0, 101, 0};
  for (int i = 0; i < 12; ++i) p.memory.put8(kA + i, entries[i]);
  const uint64_t h = p.call(N::NtUserCreateAcceleratorTable, kA, 2);
  CHECK(h != 0 && p.call(N::NtUserCopyAcceleratorTable, h, 0, 0) == 2);
  CHECK(p.call(N::NtUserCopyAcceleratorTable, h, kB, 1) == 1 && p.memory.get8(kB) == 9 && p.memory.get16(kB + 2) == 'S' && p.memory.get16(kB + 4) == 100);
  CHECK(p.call(N::NtUserCopyAcceleratorTable, h, kC, 5) == 2 && p.memory.get8(kC + 6) == 0x81 && p.memory.get16(kC + 10) == 101);
  CHECK(p.call(N::NtUserCopyAcceleratorTable, h, kC, 0) == 0);
  List<user_misc::AcceleratorEntry> list(*system_allocator());
  {
    Locked lock;
    CHECK(user_misc::accelerator_entries(uint32_t(h), &list) && list.count() == 2 && list[1].key == 0x70 && list[1].command == 101 && list[0].virt == 9);
  }
  clear_error(p);
  CHECK(p.call(N::NtUserCreateAcceleratorTable, kA, 0) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserCreateAcceleratorTable, 0x10, 2) == 0 && last_error(p) == kNoAccess);
  CHECK(p.call(N::NtUserCopyAcceleratorTable, 0x1234, 0, 0) == 0 && last_error(p) == kErrorInvalidAccelHandle);
  CHECK(p.call(N::NtUserDestroyAcceleratorTable, h) == 1);
  CHECK(p.call(N::NtUserDestroyAcceleratorTable, h) == 0 && last_error(p) == kErrorInvalidAccelHandle);
  const uint64_t h2 = p.call(N::NtUserCreateAcceleratorTable, kA, 2);
  win32k::process_exited(100);
  CHECK(p.call(N::NtUserDestroyAcceleratorTable, h2) == 0);
}


// --- clipboard
void put_set_params(FakeMemory& m, uint64_t a, uint64_t data, uint64_t size, bool cache_only = false) {
  m.put64(a, data); m.put64(a + 8, size); m.put32(a + 16, cache_only ? 1 : 0); m.put32(a + 20, 0);
}
uint64_t set_data(Program& p, uint32_t format, uint64_t handle, const std::string& bytes, bool delayed = false) {
  for (size_t i = 0; i < bytes.size(); ++i) p.memory.put8(kH + i, uint8_t(bytes[i]));
  put_set_params(p.memory, kG + 0x100, delayed ? 0 : kH, delayed ? 0 : bytes.size());
  return p.call(N::NtUserSetClipboardData, format, handle, kG + 0x100);
}
// GetClipboardData into kH + 0x1000; params at kG + 0x200 (data_size at +16, seqno at +24).
uint64_t get_data(Program& p, uint32_t format, uint64_t capacity = 0x100) {
  p.memory.put64(kG + 0x200, kH + 0x1000); p.memory.put64(kG + 0x208, capacity); p.memory.put64(kG + 0x210, 0xFFFF); p.memory.put32(kG + 0x218, 0); p.memory.put32(kG + 0x21C, 0);
  return p.call(N::NtUserGetClipboardData, format, kG + 0x200);
}
std::vector<uint32_t> enum_formats(Program& p) {
  std::vector<uint32_t> out;
  for (uint32_t f = uint32_t(p.call(N::NtUserEnumClipboardFormats, 0)); f != 0 && out.size() < 32; f = uint32_t(p.call(N::NtUserEnumClipboardFormats, f))) out.push_back(f);
  return out;
}
std::vector<uint32_t> formats(std::initializer_list<uint32_t> l) { return std::vector<uint32_t>(l); }

void test_clipboard() {
  Program p(130);
  setup(p);
  new_window(0x30001, 130, 1);
  new_window(0x30002, 130, 2);
  set_thread_override(1);
  user_misc::set_clipboard_render_timeout_ns(20ull * 1000 * 1000);
  const uint32_t seq0 = uint32_t(p.call(N::NtUserGetClipboardSequenceNumber));
  CHECK(p.call(N::NtUserIsClipboardFormatAvailable, 1) == 0 && p.call(N::NtUserCountClipboardFormats) == 0);
  clear_error(p);
  CHECK(p.call(N::NtUserEnumClipboardFormats, 0) == 0 && last_error(p) == kErrorClipboardNotOpen);
  CHECK(p.call(N::NtUserEmptyClipboard) == 0 && last_error(p) == kErrorClipboardNotOpen);
  CHECK(p.call(N::NtUserCloseClipboard) == 0 && last_error(p) == kErrorClipboardNotOpen);
  CHECK(set_data(p, 1, 0x77, std::string("hi\0", 3)) == 0xC0000184u && last_error(p) == kErrorClipboardNotOpen);
  CHECK(get_data(p, 1) == 0 && last_error(p) == kErrorClipboardNotOpen);

  // open, a second thread is turned away
  clear_error(p);
  CHECK(p.call(N::NtUserOpenClipboard, 0x99999, 0) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  CHECK(p.call(N::NtUserOpenClipboard, 0x30001, 0) == 1 && p.call(N::NtUserGetOpenClipboardWindow) == 0x30001);
  set_thread_override(2);
  CHECK(p.call(N::NtUserOpenClipboard, 0, 0) == 0 && last_error(p) == kErrorAccessDenied);
  CHECK(p.call(N::NtUserCloseClipboard) == 0 && last_error(p) == kErrorClipboardNotOpen);
  set_thread_override(1);
  CHECK(p.call(N::NtUserOpenClipboard, 0x30001, 0) == 1);      // opening again from the same thread is allowed

  // set text: the other text formats appear
  CHECK(p.call(N::NtUserEmptyClipboard) == 1 && p.call(N::NtUserGetClipboardOwner) == 0x30001);
  CHECK(set_data(p, 1, 0x77, std::string("hi\0", 3)) == 0);
  CHECK(p.call(N::NtUserCountClipboardFormats) == 4 && p.call(N::NtUserIsClipboardFormatAvailable, 13) == 1 && p.call(N::NtUserIsClipboardFormatAvailable, 2) == 0);
  CHECK(enum_formats(p) == formats({1, 16, 7, 13}));
  CHECK(get_data(p, 13) != 0 && p.memory.get64(kG + 0x210) == 6 && p.memory.get_wide_z(kH + 0x1000) == "hi" && p.memory.get16(kH + 0x1000 + 4) == 0);
  CHECK(get_data(p, 1) == 0x77 && p.memory.get64(kG + 0x210) == 3 && p.memory.get8(kH + 0x1000) == 'h');
  CHECK(get_data(p, 16) != 0 && p.memory.get64(kG + 0x210) == 4 && p.memory.get32(kH + 0x1000) == 0x0409);
  CHECK(get_data(p, 7) != 0 && p.memory.get64(kG + 0x210) == 3 && p.memory.get8(kH + 0x1001) == 'i');
  CHECK(get_data(p, 2) == 0 && p.memory.get64(kG + 0x210) == 0xFFFF);      // not there: the block is not touched
  CHECK(get_data(p, 13, 2) != 0 && p.memory.get64(kG + 0x210) == 6 && p.memory.get8(kH + 0x1000) == 'h');      // a short buffer: the size says how much there is
  CHECK(p.memory.get32(kG + 0x218) == uint32_t(p.call(N::NtUserGetClipboardSequenceNumber)));

  // a listener hears of the change when the clipboard is closed, the viewer of the chain too
  g_posted.clear();
  CHECK(p.call(N::NtUserAddClipboardFormatListener, 0x30002) == 1 && p.call(N::NtUserAddClipboardFormatListener, 0x30002) == 1);
  CHECK(p.call(N::NtUserSetClipboardViewer, 0x30001) == 0 && posted_count(0x30001, 0x308) == 1);
  CHECK(p.call(N::NtUserCloseClipboard) == 1);
  CHECK(posted_count(0x30002, 0x31D) == 1 && posted_count(0x30001, 0x308) == 2 && p.call(N::NtUserGetOpenClipboardWindow) == 0);
  CHECK(p.call(N::NtUserOpenClipboard, 0x30001, 0) == 1 && p.call(N::NtUserCloseClipboard) == 1 && posted_count(0x30002, 0x31D) == 1);      // nothing changed: no message
  CHECK(uint32_t(p.call(N::NtUserGetClipboardSequenceNumber)) > seq0);
  CHECK(p.call(N::NtUserSetClipboardViewer, 0x30002) == 0x30001 && p.call(N::NtUserGetClipboardViewer) == 0x30002);
  CHECK(p.call(N::NtUserChangeClipboardChain, 0x30002, 0x30001) == 1 && p.call(N::NtUserGetClipboardViewer) == 0x30001);
  g_posted.clear();
  CHECK(p.call(N::NtUserChangeClipboardChain, 0x30002, 0) == 1 && posted_count(0x30001, 0x30D) == 1);      // not the first viewer: asked along the chain
  clear_error(p);
  CHECK(p.call(N::NtUserRemoveClipboardFormatListener, 0x30001) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserAddClipboardFormatListener, 0x99999) == 0 && last_error(p) == kErrorInvalidWindowHandle);

  // Unicode text first: the ANSI formats are made from it (cp1252)
  CHECK(p.call(N::NtUserOpenClipboard, 0x30001, 0) == 1);
  g_posted.clear();
  CHECK(p.call(N::NtUserEmptyClipboard) == 1 && posted_count(0x30001, 0x307) == 1);      // WM_DESTROYCLIPBOARD to the previous owner
  const std::string uni("c\0a\0f\0\xe9\0\xac\x20\0\0", 12);
  CHECK(set_data(p, 13, 0, uni) == 0);
  CHECK(enum_formats(p) == formats({13, 16, 1, 7}));
  CHECK(get_data(p, 1) != 0 && p.memory.get64(kG + 0x210) == 6 && p.memory.get8(kH + 0x1000 + 3) == 0xE9 && p.memory.get8(kH + 0x1000 + 4) == 0x80 && p.memory.get8(kH + 0x1000 + 5) == 0);
  CHECK(get_data(p, 13) == 1 && p.memory.get64(kG + 0x210) == 12);      // set without a handle: 1
  // an explicit ANSI text replaces the synthesized one and keeps its place
  CHECK(set_data(p, 1, 0x88, std::string("plain\0", 6)) == 0);
  CHECK(enum_formats(p) == formats({13, 1, 16, 7}) && get_data(p, 1) == 0x88);
  // the handle can be recorded later without the data
  put_set_params(p.memory, kG + 0x100, 0, 0, true);
  CHECK(p.call(N::NtUserSetClipboardData, 1, 0x99, kG + 0x100) == 0 && get_data(p, 1) == 0x99 && p.memory.get64(kG + 0x210) == 6);

  // metafile and bitmap families are listed the way Windows does
  CHECK(p.call(N::NtUserEmptyClipboard) == 1);
  CHECK(set_data(p, 14, 5, "emf") == 0 && set_data(p, 8, 6, "dib") == 0);
  CHECK(enum_formats(p) == formats({14, 8, 3, 2, 17}));
  CHECK(get_data(p, 3) == 0 && p.memory.get64(kG + 0x210) == 0);      // (their conversion is up to user mode)

  // delayed rendering: the owner is asked, the asker waits and gives up
  CHECK(p.call(N::NtUserEmptyClipboard) == 1);
  CHECK(set_data(p, 0xC001, 0, "", true) == 0 && p.call(N::NtUserCountClipboardFormats) == 1);
  g_posted.clear();
  CHECK(get_data(p, 0xC001) == 0 && posted_count(0x30001, 0x305) == 1 && g_posted[0].wparam == 0xC001);
  CHECK(set_data(p, 0xC001, 0, "data") == 0 && get_data(p, 0xC001) == 1 && p.memory.get64(kG + 0x210) == 4);

  // priority list, updated formats, bad arguments
  CHECK(p.call(N::NtUserEmptyClipboard) == 1 && set_data(p, 13, 0, std::string("a\0\0\0", 4)) == 0);
  p.memory.put32(kA, 2); p.memory.put32(kA + 4, 13); p.memory.put32(kA + 8, 1);
  CHECK(p.call(N::NtUserGetPriorityClipboardFormat, kA, 3) == 13);
  p.memory.put32(kA + 4, 3);
  CHECK(uint32_t(p.call(N::NtUserGetPriorityClipboardFormat, kA, 2)) == 0xFFFFFFFFu);
  clear_error(p);
  CHECK(p.call(N::NtUserGetPriorityClipboardFormat, kA, 0) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserGetUpdatedClipboardFormats, kB, 2, kC) == 0 && last_error(p) == kErrorInsufficientBuffer && p.memory.get32(kC) == 4);
  CHECK(p.call(N::NtUserGetUpdatedClipboardFormats, kB, 4, kC) == 1 && p.memory.get32(kB) == 13 && p.memory.get32(kB + 4) == 16);
  CHECK(p.call(N::NtUserGetUpdatedClipboardFormats, 0x10, 4, kC) == 0 && last_error(p) == kNoAccess);
  // errors of SetClipboardData
  CHECK(set_data(p, 0, 1, "x") == 0xC000000Du && last_error(p) == kErrorInvalidParameter);
  put_set_params(p.memory, kG + 0x100, kH, 65ull << 20);
  CHECK(p.call(N::NtUserSetClipboardData, 5, 1, kG + 0x100) == 0xC0000017u);
  put_set_params(p.memory, kG + 0x100, 0x10, 4);
  CHECK(p.call(N::NtUserSetClipboardData, 5, 1, kG + 0x100) == 0xC0000005u && last_error(p) == kNoAccess);
  CHECK(p.call(N::NtUserSetClipboardData, 5, 1, 0x10) == 0xC0000005u);
  CHECK(p.call(N::NtUserCountClipboardFormats) == 4);

  // the bridge to a text clipboard of the kernel
  List<char16_t> text(*system_allocator());
  {
    Locked lock;
    const uint32_t before = user_misc::clipboard_sequence();
    const char16_t abc[] = u"abc";
    CHECK(user_misc::clipboard_set_unicode_text(ArrayView<const char16_t>(abc, 3)) && user_misc::clipboard_sequence() > before);
    CHECK(user_misc::clipboard_unicode_text(&text) && text.count() == 3 && text[2] == u'c');
  }
  CHECK(p.call(N::NtUserCloseClipboard) == 1);
  CHECK(p.call(N::NtUserGetClipboardOwner) == 0);

  // windows that go: the viewer and the listeners are forgotten
  g_posted.clear();
  drop_window(0x30002);
  CHECK(p.call(N::NtUserOpenClipboard, 0x30001, 0) == 1 && p.call(N::NtUserEmptyClipboard) == 1 && set_data(p, 13, 0, std::string("b\0\0\0", 4)) == 0 && p.call(N::NtUserCloseClipboard) == 1);
  CHECK(posted_count(0x30002, 0x31D) == 0);
  // a delayed format cannot be rendered once its owner window is gone
  CHECK(p.call(N::NtUserOpenClipboard, 0x30001, 0) == 1 && p.call(N::NtUserEmptyClipboard) == 1 && set_data(p, 0xC002, 0, "", true) == 0 && p.call(N::NtUserCloseClipboard) == 1);
  CHECK(p.call(N::NtUserCountClipboardFormats) == 1);
  drop_window(0x30001);
  CHECK(p.call(N::NtUserCountClipboardFormats) == 0 && p.call(N::NtUserGetClipboardOwner) == 0);

  // the bridge: the clipboard of the desktop's programs and the Windows clipboard are one
  {
    kernel::Clipboard kc(*system_allocator());
    user_misc::install_clipboard_bridge(&kc);
    String got(*system_allocator());
    // a Windows program copies text: it is on the desktop's clipboard when the program closes the clipboard
    CHECK(p.call(N::NtUserOpenClipboard, 0, 0) == 1 && p.call(N::NtUserEmptyClipboard) == 1 && set_data(p, 13, 0, std::string("h\0i\0\0\0", 6)) == 0);
    CHECK(!kc.has_text());      // (not before: the clipboard is still being filled)
    CHECK(p.call(N::NtUserCloseClipboard) == 1 && kc.get_text(&got) && got.view().equals("hi"));
    // the desktop copies: the Windows clipboard has it at the next look, and the format count and the sequence number change
    const uint32_t seq = uint32_t(p.call(N::NtUserGetClipboardSequenceNumber));
    CHECK(kc.set_text("from the desktop \xC3\xA9"));
    CHECK(p.call(N::NtUserIsClipboardFormatAvailable, 13) == 1 && uint32_t(p.call(N::NtUserGetClipboardSequenceNumber)) > seq);
    {
      Locked lock;
      List<char16_t> units(*system_allocator());
      CHECK(user_misc::clipboard_unicode_text(&units) && units.count() == 18 && units[0] == u'f' && units[17] == 0xE9);
    }
    // a picture of the desktop is a CF_DIB; a CF_DIB of a program is a picture of the desktop
    const uint32_t pixels[4] = {0xFF112233u, 0xFF445566u, 0xFF778899u, 0xFFAABBCCu};
    CHECK(kc.set_image(pixels, 2, 2));
    CHECK(p.call(N::NtUserIsClipboardFormatAvailable, 8) == 1 && p.call(N::NtUserIsClipboardFormatAvailable, 13) == 0);
    std::string dib;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) dib.push_back(char(v >> (8 * i))); };
    put32(40); put32(1); put32(1); dib.push_back(1); dib.push_back(0); dib.push_back(32); dib.push_back(0); put32(0); put32(4); put32(0); put32(0); put32(0); put32(0);
    put32(0x00FF8040u);      // one pixel (no alpha: opaque)
    CHECK(p.call(N::NtUserOpenClipboard, 0, 0) == 1 && p.call(N::NtUserEmptyClipboard) == 1 && set_data(p, 8, 0, dib) == 0 && p.call(N::NtUserCloseClipboard) == 1);
    uint32_t w = 0, h = 0, one = 0;
    CHECK(kc.image_size(&w, &h) && w == 1 && h == 1 && kc.get_image(&one, 1) && one == 0xFFFF8040u);
    // emptying on the Windows side empties the other
    CHECK(p.call(N::NtUserOpenClipboard, 0, 0) == 1 && p.call(N::NtUserEmptyClipboard) == 1 && p.call(N::NtUserCloseClipboard) == 1 && kc.format() == kernel::ClipboardFormat::Empty);
    user_misc::install_clipboard_bridge(nullptr);
  }

  // a process that ends while it has the clipboard open lets go of it
  new_window(0x30003, 131, 3);
  Program q(131);
  setup(q);
  set_thread_override(3);
  CHECK(q.call(N::NtUserOpenClipboard, 0x30003, 0) == 1 && q.call(N::NtUserEmptyClipboard) == 1 && set_data(q, 13, 0, std::string("z\0\0\0", 4)) == 0);
  win32k::process_exited(131);
  set_thread_override(1);
  CHECK(p.call(N::NtUserGetOpenClipboardWindow) == 0 && p.call(N::NtUserOpenClipboard, 0, 0) == 1 && p.call(N::NtUserCloseClipboard) == 1);
  drop_window(0x30003);
  set_thread_override(0);
}

// --- settings, metrics, colours
uint32_t g_solid_serial = 0x900, g_last_solid_color = 0;
uint32_t fake_solid_object(uint64_t, uint32_t color) { g_last_solid_color = color; return ++g_solid_serial; }
uint64_t spi(Program& p, uint32_t action, uint32_t ui, uint64_t pv) { return p.call(N::NtUserSystemParametersInfo, action, ui, pv, 0); }

void test_settings() {
  Program p(140);
  setup(p);
  set_thread_override(1);
  // simple settings: defaults, round trips, ranges
  CHECK(spi(p, 1, 0, kA) == 1 && p.memory.get32(kA) == 1);
  CHECK(spi(p, 2, 0, 0) == 1 && spi(p, 1, 0, kA) == 1 && p.memory.get32(kA) == 0);
  CHECK(spi(p, 2, 5, 0) == 1 && spi(p, 1, 0, kA) == 1 && p.memory.get32(kA) == 1);      // a BOOL is 0 or 1
  CHECK(spi(p, 0x70, 0, kA) == 1 && p.memory.get32(kA) == 10);
  CHECK(spi(p, 0x71, 0, 5) == 1 && spi(p, 0x70, 0, kA) == 1 && p.memory.get32(kA) == 5);
  clear_error(p);
  CHECK(spi(p, 0x71, 0, 21) == 0 && last_error(p) == kErrorInvalidSpiValue && spi(p, 0x70, 0, kA) == 1 && p.memory.get32(kA) == 5);
  spi(p, 0x71, 0, 10);
  CHECK(spi(p, 0x0A, 0, kA) == 1 && p.memory.get32(kA) == 31 && spi(p, 0x0B, 32, 0) == 0 && spi(p, 0x0B, 12, 0) == 1);
  CHECK(spi(p, 0x68, 0, kA) == 1 && p.memory.get32(kA) == 3);      // wheel scroll lines
  CHECK(spi(p, 0x1042, 0, kA) == 1 && p.memory.get32(kA) == 1 && spi(p, 0x1043, 0, 0) == 1 && spi(p, 0x1042, 0, kA) == 1 && p.memory.get32(kA) == 0);      // the value of a 0x1000-class set is the pointer
  CHECK(spi(p, 0x2006, 0, kA) == 1 && p.memory.get32(kA) == 1 && spi(p, 0x2007, 0, 0) == 0);
  clear_error(p);
  CHECK(spi(p, 0xDEAD, 0, kA) == 0 && last_error(p) == kErrorInvalidSpiValue);
  CHECK(spi(p, 1, 0, 0x10) == 0 && last_error(p) == kNoAccess && spi(p, 1, 0, 0) == 0);
  CHECK(spi(p, 3, 0, kA) == 1 && p.memory.get32(kA) == 6 && p.memory.get32(kA + 4) == 10 && p.memory.get32(kA + 8) == 1);
  p.memory.put32(kA, 7); p.memory.put32(kA + 4, 11); p.memory.put32(kA + 8, 2);
  CHECK(spi(p, 4, 0, kA) == 1);
  CHECK(spi(p, 3, 0, kB) == 1 && p.memory.get32(kB) == 7 && p.memory.get32(kB + 8) == 2);
  CHECK(spi(p, 4, 0, 0x10) == 0);
  // icon spacing: a NULL pointer sets, a pointer gets
  CHECK(spi(p, 0x0D, 0, kA) == 1 && p.memory.get32(kA) == 75 && spi(p, 0x0D, 100, 0) == 1 && spi(p, 0x0D, 0, kA) == 1 && p.memory.get32(kA) == 100);
  CHECK(spi(p, 0x0D, 5, 0) == 0);
  // wallpaper
  p.memory.put_wide(kA, "C:\\bg.bmp");
  CHECK(spi(p, 0x14, 0, kA) == 1 && spi(p, 0x73, 20, kB) == 1 && p.memory.get_wide_z(kB) == "C:\\bg.bmp");
  clear_error(p);
  CHECK(spi(p, 0x73, 3, kB) == 0 && last_error(p) == kErrorInsufficientBuffer);
  // structures
  for (int i = 0; i < 504; ++i) p.memory.put8(kA + i, 0);
  p.memory.put32(kA, 504);
  CHECK(spi(p, 0x29, 504, kA) == 1 && p.memory.get32(kA) == 504 && p.memory.get32(kA + 8) == 17 && p.memory.get32(kA + 24) == uint32_t(-12) && p.memory.get32(kA + 500) == 4);
  CHECK(p.memory.get_wide_z(kA + 24 + 28) == "Segoe UI" && p.memory.get32(kA + 24 + 16) == 400 && p.memory.get32(kA + 216) == 19);
  clear_error(p);
  p.memory.put32(kB, 500);
  CHECK(spi(p, 0x29, 500, kB) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(spi(p, 0x29, 504, kB) == 0);      // cbSize in the block and the size argument must agree with the real size
  p.memory.put32(kA + 8, 20);
  CHECK(spi(p, 0x2A, 504, kA) == 1 && one(p, 9, 2) == 20 && user_misc::system_metrics(2, 96) == 20);      // SM_CXVSCROLL follows
  p.memory.put32(kA + 8, 17);
  spi(p, 0x2A, 504, kA);
  // at another DPI
  for (int i = 0; i < 504; ++i) p.memory.put8(kA + i, 0);
  p.memory.put32(kA, 504);
  CHECK(p.call(N::NtUserSystemParametersInfoForDpi, 0x29, 504, kA, 0, 144) == 1 && p.memory.get32(kA + 8) == 26 && p.memory.get32(kA + 24) == uint32_t(-18) && p.memory.get32(kA + 4) == 2);
  clear_error(p);
  CHECK(p.call(N::NtUserSystemParametersInfoForDpi, 1, 0, kB, 0, 144) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserSystemParametersInfoForDpi, 0x29, 504, kA, 0, 0) == 0);
  p.memory.put32(kA, 108);
  CHECK(spi(p, 0x2D, 108, kA) == 1 && p.memory.get32(kA + 4) == 100 && p.memory.get32(kA + 12) == 1 && p.memory.get32(kA + 16) == uint32_t(-12));
  p.memory.put32(kA, 20);
  CHECK(spi(p, 0x2B, 20, kA) == 1 && p.memory.get32(kA + 4) == 160 && p.memory.get32(kA + 16) == 8);
  p.memory.put32(kA, 8);
  CHECK(spi(p, 0x3A, 8, kA) == 1 && p.memory.get32(kA + 4) == 506);      // sticky keys
  p.memory.put32(kA, 7);
  CHECK(spi(p, 0x3A, 7, kA) == 0);
  p.memory.put32(kA, 16);
  CHECK(spi(p, 0x42, 16, kA) == 1 && p.memory.get32(kA + 4) == 0x4A);      // high contrast
  // the work area
  CHECK(spi(p, 0x30, 0, kA) == 1 && int32_t(p.memory.get32(kA + 8)) == int32_t(gdi::screen_width()) && int32_t(p.memory.get32(kA + 12)) == int32_t(gdi::screen_height()));
  p.memory.put32(kA, 0); p.memory.put32(kA + 4, 0); p.memory.put32(kA + 8, 1024); p.memory.put32(kA + 12, 728);
  CHECK(spi(p, 0x2F, 0, kA) == 1 && spi(p, 0x30, 0, kB) == 1 && p.memory.get32(kB + 12) == 728);
  CHECK(user_misc::monitor_rect(user_misc::monitor_handle(), true).bottom == 728 && user_misc::monitor_rect(user_misc::monitor_handle(), false).bottom == 768);
  p.memory.put32(kA + 8, 0);
  CHECK(spi(p, 0x2F, 0, kA) == 0);
  // metrics of Windows 10
  CHECK(user_misc::system_metrics(0, 96) == 1024 && user_misc::system_metrics(1, 96) == 768 && user_misc::system_metrics(4, 96) == 23 && user_misc::system_metrics(15, 96) == 20);
  CHECK(user_misc::system_metrics(5, 96) == 1 && user_misc::system_metrics(45, 96) == 2 && user_misc::system_metrics(49, 96) == 16 && user_misc::system_metrics(11, 96) == 32);
  CHECK(user_misc::system_metrics(92, 96) == 4 && user_misc::system_metrics(33, 96) == 4 && user_misc::system_metrics(80, 96) == 1 && user_misc::system_metrics(36, 96) == 4);
  CHECK(user_misc::system_metrics(78, 96) == 1024 && user_misc::system_metrics(59, 96) == 1040 && user_misc::system_metrics(0x1000, 96) == 0 && user_misc::system_metrics(500, 96) == 0);
  CHECK(user_misc::system_metrics(4, 0) == 23 && user_misc::system_metrics(4, 144) == 35 && user_misc::system_metrics(2, 192) == 34 && user_misc::system_metrics(0, 192) == 1024);
  CHECK(user_misc::system_metrics(80, 192) == 1 && user_misc::system_metrics(36, 192) == 4);      // counts and the double click size are not scaled
  CHECK(two(p, 3, 4, 144) == 35 && one(p, 9, 4) == 23 && p.call(N::NtUserScaleSystemMetricForDPIWithoutCache, 4, 144) == 35);
  CHECK(one(p, 9, 23) == 0 && p.call(N::NtUserSwapMouseButton, 1) == 0 && user_misc::system_metrics(23, 96) == 1 && p.call(N::NtUserSwapMouseButton, 0) == 1);
  // colours
  CHECK(user_misc::system_color(5) == 0xFFFFFF && user_misc::system_color(13) == 0xD77800 && user_misc::system_color(31) == 0 && user_misc::system_color(15) == 0xF0F0F0);
  CHECK(one(p, 6, 5) == 0xFFFFFF && one(p, 6, 99) == 0);
  p.memory.put32(kA, 5); p.memory.put32(kA + 4, 1); p.memory.put32(kA + 16, 0x112233); p.memory.put32(kA + 20, 0xFF445566u);
  CHECK(p.call(N::NtUserSetSysColors, 2, kA, kA + 16) == 1 && user_misc::system_color(5) == 0x112233 && user_misc::system_color(1) == 0x445566);
  p.memory.put32(kA + 8, 31);
  clear_error(p);
  CHECK(p.call(N::NtUserSetSysColors, 3, kA, kA + 16) == 0 && last_error(p) == kErrorInvalidParameter && user_misc::system_color(5) == 0x112233);
  CHECK(p.call(N::NtUserSetSysColors, 2, 0x10, kA + 16) == 0 && last_error(p) == kNoAccess && p.call(N::NtUserSetSysColors, 99, kA, kA + 16) == 0);
  p.memory.put32(kA + 16, 0xFFFFFF); p.memory.put32(kA + 20, 0);
  CHECK(p.call(N::NtUserSetSysColors, 2, kA, kA + 16) == 1 && user_misc::system_color(1) == 0);
  // the brushes need the GDI module (not part of the contract): without it the call fails
  clear_error(p);
  CHECK(one(p, 7, 5) == 0 && last_error(p) == kErrorNotSupported && one(p, 8, 5) == 0);
  user_misc::set_solid_brush_factory(fake_solid_object);
  user_misc::set_solid_pen_factory(fake_solid_object);
  const uint64_t brush = one(p, 7, 5), pen = one(p, 8, 5);
  CHECK(brush != 0 && pen != 0 && brush != pen && one(p, 7, 5) == brush && one(p, 8, 5) == pen && one(p, 7, 6) != brush);      // made once per colour
    p.memory.put32(kA, 5); p.memory.put32(kA + 16, 0x010203);
  CHECK(p.call(N::NtUserSetSysColors, 1, kA, kA + 16) == 1 && one(p, 7, 5) != brush && g_last_solid_color == 0x010203);      // a changed colour gets a new brush
  p.memory.put32(kA + 16, 0xFFFFFF);
  p.call(N::NtUserSetSysColors, 1, kA, kA + 16);
  clear_error(p);
  CHECK(one(p, 7, 99) == 0 && last_error(p) == kErrorInvalidParameter);
  user_misc::set_solid_brush_factory(nullptr);
  user_misc::set_solid_pen_factory(nullptr);
  // double click time
  CHECK(p.call(N::NtUserGetDoubleClickTime) == 500 && p.call(N::NtUserSetDoubleClickTime, 600) == 1 && p.call(N::NtUserGetDoubleClickTime) == 600);
  clear_error(p);
  CHECK(p.call(N::NtUserSetDoubleClickTime, 5001) == 0 && last_error(p) == kErrorInvalidParameter && p.call(N::NtUserSetDoubleClickTime, 0) == 1 && p.call(N::NtUserGetDoubleClickTime) == 500);
  CHECK(p.call(N::NtUserUpdatePerUserSystemParameters, 0, 1) == 1);
  set_thread_override(0);
}

// --- monitors and the display
void put_devmode(FakeMemory& m, uint64_t a, uint32_t fields, uint32_t w, uint32_t h, uint32_t bpp) {
  for (uint32_t i = 0; i < 220; ++i) m.put8(a + i, 0);
  m.put16(a + 68, 220); m.put32(a + 72, fields); m.put32(a + 168, bpp); m.put32(a + 172, w); m.put32(a + 176, h);
}

void test_monitors() {
  Program p(150);
  setup(p);
  set_thread_override(1);
  gdi::set_screen_size(1024, 768);
  const uint32_t mon = user_misc::monitor_handle();
  CHECK(mon == 0x10001 && user_misc::is_monitor(mon) && !user_misc::is_monitor(0) && !user_misc::is_monitor(0x10002));

  // the contract
  CHECK(user_misc::monitor_from_rect(gdi::Rect{10, 10, 20, 20}, 0) == mon && user_misc::monitor_from_rect(gdi::Rect{2000, 2000, 2100, 2100}, 0) == 0);
  CHECK(user_misc::monitor_from_rect(gdi::Rect{2000, 2000, 2100, 2100}, 2) == mon && user_misc::monitor_from_rect(gdi::Rect{-50, 0, -10, 20}, 1) == mon);
  CHECK(user_misc::monitor_from_point(5, 5, 0) == mon && user_misc::monitor_from_point(-1, 5, 0) == 0 && user_misc::monitor_from_point(1024, 5, 2) == mon);
  CHECK(user_misc::dpi_for_monitor(mon) == 96 && user_misc::dpi_for_monitor(0) == 0);
  const gdi::Rect screen = user_misc::monitor_rect(mon, false);
  CHECK(screen.left == 0 && screen.right == 1024 && screen.bottom == 768 && user_misc::monitor_rect(0, false).right == 0);

  // GetMonitorInfo through NtUserCallTwoParam
  p.memory.put32(kA, 104);
  CHECK(two(p, 2, mon, kA) == 1 && p.memory.get32(kA + 12) == 1024 && p.memory.get32(kA + 16) == 768 && p.memory.get32(kA + 36) == 1 && p.memory.get_wide_z(kA + 40) == "\\\\.\\DISPLAY1");
  p.memory.put32(kB, 40);
  CHECK(two(p, 2, mon, kB) == 1 && p.memory.get32(kB + 4) == 0 && p.memory.get32(kB + 12) == 1024 && p.memory.get32(kB + 28) == 1024);
  p.memory.put32(kB, 41);
  clear_error(p);
  CHECK(two(p, 2, mon, kB) == 0 && last_error(p) == kErrorInvalidParameter);
  p.memory.put32(kB, 40);
  CHECK(two(p, 2, 0x10002, kB) == 0 && last_error(p) == kErrorInvalidMonitorHandle);
  CHECK(two(p, 2, mon, 0x10) == 0 && last_error(p) == kNoAccess);
  // MonitorFromRect, the primary and the virtual screen through the multiplexers
  p.memory.put32(kC, 5); p.memory.put32(kC + 4, 5); p.memory.put32(kC + 8, 50); p.memory.put32(kC + 12, 50);
  CHECK(two(p, 4, kC, 0) == mon);
  p.memory.put32(kC, 5000); p.memory.put32(kC + 8, 6000);
  CHECK(two(p, 4, kC, 0) == 0 && two(p, 4, kC, 1) == mon);
  CHECK(one(p, 5, kC) == 1 && p.memory.get32(kC + 8) == 1024 && p.memory.get32(kC + 12) == 768);
  CHECK(one(p, 10, kC) == 1 && two(p, 8, kC + 0x20, 0) == 1 && p.memory.get32(kC + 0x28) == 1024);
  CHECK(one(p, 5, 0x10) == 0 && two(p, 4, 0x10, 0) == 0);

  // display modes
  put_devmode(p.memory, kD, 0, 0, 0, 0);
  CHECK(p.call(N::NtUserEnumDisplaySettings, 0, 0xFFFFFFFFu, kD, 0) == 1);
  CHECK(p.memory.get16(kD + 68) == 220 && p.memory.get32(kD + 168) == 32 && p.memory.get32(kD + 172) == 1024 && p.memory.get32(kD + 176) == 768 && p.memory.get32(kD + 184) == 60);
  CHECK(p.memory.get_wide_z(kD) == "\\\\.\\DISPLAY1" && (p.memory.get32(kD + 72) & 0x180000) == 0x180000);
  CHECK(p.call(N::NtUserEnumDisplaySettings, 0, 0, kD, 0) == 1 && p.memory.get32(kD + 172) == 640 && p.memory.get32(kD + 176) == 480);
  uint32_t modes = 0, prev_w = 0;
  for (; p.call(N::NtUserEnumDisplaySettings, 0, modes, kD, 0) == 1 && modes < 100; ++modes) {
    CHECK(p.memory.get32(kD + 172) >= prev_w);
    prev_w = p.memory.get32(kD + 172);
  }
  CHECK(modes >= 10 && modes < 100);
  p.memory.put_unicode_string(kE, kE + 0x40, "\\\\.\\DISPLAY1");
  CHECK(p.call(N::NtUserEnumDisplaySettings, kE, 0xFFFFFFFFu, kD, 0) == 1);
  p.memory.put_unicode_string(kE, kE + 0x40, "\\\\.\\DISPLAY9");
  CHECK(p.call(N::NtUserEnumDisplaySettings, kE, 0xFFFFFFFFu, kD, 0) == 0);
  CHECK(p.call(N::NtUserEnumDisplaySettings, 0, 0xFFFFFFFFu, 0x10, 0) == 0);

  // changing the mode resizes the screen surface
  put_devmode(p.memory, kD, 0x80000 | 0x100000 | 0x40000, 800, 600, 32);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 2, 0) == 0 && gdi::screen_width() == 1024);      // CDS_TEST: nothing changes
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 0, 0) == 0 && gdi::screen_width() == 800 && gdi::screen_height() == 600);
  CHECK(user_misc::system_metrics(0, 96) == 800 && user_misc::monitor_rect(mon, false).bottom == 600 && user_misc::monitor_rect(mon, true).bottom == 600);
  put_devmode(p.memory, kD, 0x80000 | 0x100000 | 0x40000, 800, 600, 16);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 0, 0) == 0xFFFFFFFEu);      // DISP_CHANGE_BADMODE
  put_devmode(p.memory, kD, 0x80000 | 0x100000, 4, 4, 0);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 0, 0) == 0xFFFFFFFEu);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 0x1000, 0) == 0xFFFFFFFCu);      // DISP_CHANGE_BADFLAGS
  p.memory.put_unicode_string(kE, kE + 0x40, "\\\\.\\DISPLAY2");
  CHECK(p.call32(N::NtUserChangeDisplaySettings, kE, 0, 0, 0, 0) == 0xFFFFFFFBu);      // DISP_CHANGE_BADPARAM
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, 0x10, 0, 0, 0) == 0xFFFFFFFBu);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, 0, 0, 0, 0) == 0 && gdi::screen_width() == 1024 && gdi::screen_height() == 768);      // back to the registry mode
  put_devmode(p.memory, kD, 0x80000 | 0x100000, 1280, 720, 0);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 1, 0) == 0 && p.call(N::NtUserEnumDisplaySettings, 0, 0xFFFFFFFEu, kD, 0) == 1 && p.memory.get32(kD + 172) == 1280);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, 0, 0, 0, 0) == 0 && gdi::screen_width() == 1280);      // (the registry mode is the one just stored)
  put_devmode(p.memory, kD, 0x80000 | 0x100000, 1024, 768, 0);
  CHECK(p.call32(N::NtUserChangeDisplaySettings, 0, kD, 0, 1, 0) == 0 && gdi::screen_width() == 1024);

  // display devices
  const uint64_t dd = kG;
  p.memory.put32(dd, 840);
  CHECK(p.call32(N::NtUserEnumDisplayDevices, 0, 0, dd, 0) == 0 && p.memory.get_wide_z(dd + 4) == "\\\\.\\DISPLAY1" && (p.memory.get32(dd + 324) & 5) == 5);
  CHECK(p.call32(N::NtUserEnumDisplayDevices, 0, 1, dd, 0) == 0xC0000001u);
  p.memory.put_unicode_string(kE, kE + 0x40, "\\\\.\\DISPLAY1");
  CHECK(p.call32(N::NtUserEnumDisplayDevices, kE, 0, dd, 0) == 0 && p.memory.get_wide_z(dd + 4) == "\\\\.\\DISPLAY1\\Monitor0" && p.memory.get32(dd + 324) == 1);
  CHECK(p.call32(N::NtUserEnumDisplayDevices, kE, 1, dd, 0) == 0xC0000001u);
  p.memory.put32(dd, 100);
  CHECK(p.call32(N::NtUserEnumDisplayDevices, 0, 0, dd, 0) == 0xC0000001u);

  // display configuration
  CHECK(p.call32(N::NtUserGetDisplayConfigBufferSizes, 2, kA, kA + 4) == 0 && p.memory.get32(kA) == 1 && p.memory.get32(kA + 4) == 2);
  CHECK(p.call32(N::NtUserGetDisplayConfigBufferSizes, 0, kA, kA + 4) == 87 && p.call32(N::NtUserGetDisplayConfigBufferSizes, 2, 0, kA + 4) == 87);
  p.memory.put32(kA, 1); p.memory.put32(kA + 4, 2);
  CHECK(p.call32(N::NtUserQueryDisplayConfig, 2, kA, kB, kA + 4, kC, 0) == 0);
  CHECK(p.memory.get32(kB + 68) == 1 && p.memory.get32(kB + 16) == 1 && p.memory.get32(kC) == 1 && p.memory.get32(kC + 16) == 1024 && p.memory.get32(kC + 20) == 768);
  CHECK(p.memory.get32(kC + 64) == 2 && p.memory.get32(kC + 64 + 40) == 1024 && p.memory.get32(kC + 64 + 44) == 768);
  p.memory.put32(kA, 0); p.memory.put32(kA + 4, 2);
  CHECK(p.call32(N::NtUserQueryDisplayConfig, 2, kA, kB, kA + 4, kC, 0) == 122 && p.memory.get32(kA) == 1);
  p.memory.put32(kA, 1); p.memory.put32(kA + 4, 2);
  CHECK(p.call32(N::NtUserQueryDisplayConfig, 4, kA, kB, kA + 4, kC, kD) == 0 && p.memory.get32(kD) == 1);
  CHECK(p.call32(N::NtUserQueryDisplayConfig, 4, kA, kB, kA + 4, kC, 0) == 87 && p.call32(N::NtUserQueryDisplayConfig, 8, kA, kB, kA + 4, kC, 0) == 87);
  for (int i = 0; i < 20; ++i) p.memory.put8(kE + i, 0);
  p.memory.put32(kE, 1); p.memory.put32(kE + 4, 84); p.memory.put32(kE + 8, 1);
  CHECK(p.call32(N::NtUserDisplayConfigGetDeviceInfo, kE) == 0 && p.memory.get_wide_z(kE + 20) == "\\\\.\\DISPLAY1");
  p.memory.put32(kE, 2); p.memory.put32(kE + 4, 420);
  CHECK(p.call32(N::NtUserDisplayConfigGetDeviceInfo, kE) == 0 && p.memory.get_wide_z(kE + 36) == "Generic PnP Monitor");
  p.memory.put32(kE + 4, 100);
  CHECK(p.call32(N::NtUserDisplayConfigGetDeviceInfo, kE) == 0xC000000Du);
  p.memory.put32(kE, 77); p.memory.put32(kE + 4, 500);
  CHECK(p.call32(N::NtUserDisplayConfigGetDeviceInfo, kE) == 0xC000000Du);
  p.memory.put32(kE, 1); p.memory.put32(kE + 4, 84); p.memory.put32(kE + 8, 9);
  CHECK(p.call32(N::NtUserDisplayConfigGetDeviceInfo, kE) == 0xC0000001u);

  // DPI: monitor, awareness of the process and the thread
  CHECK(p.call(N::NtUserGetDpiForMonitor, mon, 0, kA, kA + 4) == 1 && p.memory.get32(kA) == 96 && p.memory.get32(kA + 4) == 96);
  clear_error(p);
  CHECK(p.call(N::NtUserGetDpiForMonitor, 0x777, 0, kA, kA + 4) == 0 && last_error(p) == kErrorInvalidMonitorHandle);
  CHECK(p.call(N::NtUserGetDpiForMonitor, mon, 3, kA, kA + 4) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserGetDpiForMonitor, mon, 0, 0x10, kA + 4) == 0 && last_error(p) == kNoAccess);
  CHECK(p.call(N::NtUserGetDpiForCurrentProcess) == 96 && p.call(N::NtUserGetSystemDpiForProcess, ~0ull) == 96);
  CHECK(p.call(N::NtUserGetProcessDpiAwarenessContext, ~0ull) == 0x6010);      // unaware, 96 dpi
  clear_error(p);
  CHECK(p.call(N::NtUserSetProcessDpiAwarenessContext, 0x77, 0) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserSetProcessDpiAwarenessContext, 0x22 | 0x40000000, 0) == 0);      // (GDI scaling belongs to unaware only)
  CHECK(p.call(N::NtUserSetProcessDpiAwarenessContext, 0x11, 0) == 1 && p.call(N::NtUserGetProcessDpiAwarenessContext, 0) == 0x6011);
  CHECK(p.call(N::NtUserSetProcessDpiAwarenessContext, 0x22, 0) == 0 && last_error(p) == kErrorAccessDenied);      // only once
  // the thread: the previous context comes back (flagged as the process's when the thread had none)
  CHECK(one(p, 12, 0x12) == (0x6011u | 0x80000000u) && one(p, 12, 0x22) == 0x12 && one(p, 12, 0) == 0x22 && one(p, 12, 0) == (0x6011u | 0x80000000u));
  clear_error(p);
  CHECK(one(p, 12, 0x77) == 0 && last_error(p) == kErrorInvalidParameter);
  // a system DPI of 144: unaware threads keep seeing 96, aware ones see 144
  user_misc::set_system_dpi(144);
  CHECK(one(p, 9, 2) == 26 && p.call(N::NtUserGetDpiForCurrentProcess) == 144 && user_misc::dpi_for_monitor(mon) == 144);
  Program unaware(151);
  setup(unaware);
  CHECK(one(unaware, 9, 2) == 17 && unaware.call(N::NtUserGetDpiForCurrentProcess) == 96 && unaware.call(N::NtUserGetSystemDpiForProcess, ~0ull) == 96);
  CHECK(p.call(N::NtUserGetDpiForMonitor, mon, 0, kA, kA + 4) == 1 && p.memory.get32(kA) == 144);
  user_misc::set_system_dpi(96);
  // physical <-> logical points: one monitor, the same coordinates
  new_window(0x40001, 150, 1);
  p.memory.put32(kA, 7); p.memory.put32(kA + 4, 9);
  CHECK(p.call(N::NtUserLogicalToPerMonitorDPIPhysicalPoint, 0x40001, kA) == 1 && p.call(N::NtUserPerMonitorDPIPhysicalToLogicalPoint, 0x40001, kA) == 1 && p.memory.get32(kA) == 7);
  CHECK(p.call(N::NtUserLogicalToPerMonitorDPIPhysicalPoint, 0x99999, kA) == 0 && p.call(N::NtUserLogicalToPerMonitorDPIPhysicalPoint, 0x40001, 0x10) == 0);
  drop_window(0x40001);
  set_thread_override(0);
}

// --- keyboard
uint64_t map_vk(Program& p, uint32_t code, uint32_t type) { return p.call(N::NtUserMapVirtualKeyEx, code, type, 0x04090409); }
// ToUnicodeEx with modifiers: returns the count, the characters are at kC.
int tou(Program& p, uint32_t vk, uint32_t scan, bool shift, bool ctrl, bool alt, bool caps = false, uint32_t size = 4) {
  for (int i = 0; i < 256; ++i) p.memory.put8(kB + i, 0);
  if (shift) p.memory.put8(kB + 0x10, 0x80);
  if (ctrl) p.memory.put8(kB + 0x11, 0x80);
  if (alt) p.memory.put8(kB + 0x12, 0x80);
  if (caps) p.memory.put8(kB + 0x14, 1);
  for (int i = 0; i < 4; ++i) p.memory.put16(kC + 2 * i, 0xAAAA);
  return int(p.call32(N::NtUserToUnicodeEx, vk, scan, kB, kC, size, 0, 0x04090409));
}
std::string key_name(Program& p, uint32_t lparam, uint32_t size = 40) {
  for (int i = 0; i < 40; ++i) p.memory.put16(kD + 2 * i, 0xCCCC);
  const uint32_t n = uint32_t(p.call(N::NtUserGetKeyNameText, lparam, kD, size));
  std::string s = p.memory.get_wide(kD, n);
  if (size != 0 && p.memory.get16(kD + 2 * n) != 0) s += "<unterminated>";
  return s;
}

void test_keyboard() {
  Program p(160);
  setup(p);
  set_thread_override(1);
  // MapVirtualKeyEx
  CHECK(map_vk(p, 0x2A, 1) == 0x10 && map_vk(p, 0x36, 1) == 0x10 && map_vk(p, 0x2A, 3) == 0xA0 && map_vk(p, 0x36, 3) == 0xA1 && map_vk(p, 0x1D, 3) == 0xA2);
  CHECK(map_vk(p, 0xE01D, 3) == 0xA3 && map_vk(p, 0xE038, 3) == 0xA5 && map_vk(p, 0xE037, 3) == 0x2C && map_vk(p, 0x37, 3) == 0x6A && map_vk(p, 0x45, 3) == 0x90);
  CHECK(map_vk(p, 0xE11D, 3) == 0x13 && map_vk(p, 0xE046, 3) == 0x03 && map_vk(p, 0x136, 3) == 0 && map_vk(p, 0xE036, 3) == 0 && map_vk(p, 0x7F, 3) == 0);
  CHECK(map_vk(p, 0x10, 0) != 0 && map_vk(p, 0xA0, 0) == map_vk(p, 0x10, 0) && map_vk(p, 0xA3, 0) == 0x1D && map_vk(p, 0xA3, 4) == 0xE01D && map_vk(p, 0xA5, 4) == 0xE038);
  CHECK(map_vk(p, 0xA1, 4) == 0x36 && map_vk(p, 0x26, 4) == 0x48 && map_vk(p, 0x2C, 4) == 0x54 && map_vk(p, 0x6A, 4) == 0x37 && map_vk(p, 0x90, 4) == 0x45 && map_vk(p, 0xA2, 4) == 0x1D);
  CHECK(map_vk(p, 'A', 0) == 0x1E && map_vk(p, 0x1E, 1) == 'A' && map_vk(p, 'A', 2) == 'A' && map_vk(p, '7', 2) == '7' && map_vk(p, 0xBD, 2) == '-' && map_vk(p, 0x20, 2) == ' ');
  CHECK(map_vk(p, 0x47, 1) == 0x24 && map_vk(p, 0x60, 0) == 0x52 && map_vk(p, 0x67, 0) == 0x47 && map_vk(p, 0x67, 2) == '7' && map_vk(p, 0x70, 0) == 0x3B && map_vk(p, 0x0D, 0) == 0x1C);
  CHECK(map_vk(p, 0x0D, 2) == 13 && map_vk(p, 0x1B, 2) == 0x1B && map_vk(p, 0x70, 2) == 0 && map_vk(p, 0, 0) == 0 && map_vk(p, 0xFF, 0) == 0);
  clear_error(p);
  CHECK(map_vk(p, 'A', 9) == 0 && last_error(p) == kErrorInvalidParameter);
  // VkKeyScanEx: the lowest virtual key that makes the character (1 shift, 2 ctrl)
  auto scan = [&](char16_t c) { return uint32_t(p.call(N::NtUserVkKeyScanEx, c, 0x04090409)) & 0xFFFF; };
  CHECK(scan('a') == 0x41 && scan('A') == 0x141 && scan('1') == 0x31 && scan('!') == 0x131 && scan(' ') == 0x20 && scan('\r') == 0x0D && scan('\n') == 0x20D && scan('\t') == 0x09);
  CHECK(scan(1) == 0x241 && scan(0x1B) == 0x1B && scan('@') == 0x132 && scan('[') == 0xDB && scan('{') == 0x1DB && scan('.') == 0xBE && scan('*') == 0x138 && scan(0x7F) == 0x208);
  CHECK(scan(0) == 0x332 && scan(0x1E) == 0x336 && scan(0x20AC) == 0xFFFF && scan(0x8) == 0x08 && scan('~') == 0x1C0);
  // ToUnicodeEx (the cases of Wine's test)
  CHECK(tou(p, 'A', 0x1E, false, false, false) == 1 && p.memory.get16(kC) == 'a' && p.memory.get16(kC + 2) == 0);
  CHECK(tou(p, 'A', 0x1E, true, false, false) == 1 && p.memory.get16(kC) == 'A' && tou(p, 'A', 0x1E, false, false, false, true) == 1 && p.memory.get16(kC) == 'A');
  CHECK(tou(p, 'A', 0x1E, true, false, false, true) == 1 && p.memory.get16(kC) == 'a' && tou(p, 'A', 0x1E, false, false, true) == 1 && p.memory.get16(kC) == 'a');
  CHECK(tou(p, 'A', 0x1E, false, true, false) == 1 && p.memory.get16(kC) == 1 && tou(p, 'A', 0x1E, true, true, false) == 1 && p.memory.get16(kC) == 1);
  CHECK(tou(p, 'A', 0x1E, true, true, true) == 0 && tou(p, 'A', 0x1E, false, true, true) == 0);
  CHECK(tou(p, 0x0D, 0x1C, false, false, false) == 1 && p.memory.get16(kC) == '\r' && p.memory.get16(kC + 2) == 0);
  CHECK(tou(p, 0x0D, 0x1C, false, true, false) == 1 && p.memory.get16(kC) == '\n' && tou(p, 0x0D, 0x1C, true, true, false) == 0);
  CHECK(tou(p, 0x09, 0x0F, false, false, false) == 1 && p.memory.get16(kC) == '\t' && tou(p, 0x09, 0x0F, false, true, false) == 0 && tou(p, 0x09, 0x0F, true, true, false) == 0);
  CHECK(tou(p, '4', 5, false, true, false) == 0 && tou(p, '4', 5, true, true, false) == 0 && tou(p, '4', 5, true, false, false) == 1 && p.memory.get16(kC) == '$');
  CHECK(tou(p, '2', 3, true, true, false) == 1 && p.memory.get16(kC) == 0 && tou(p, 0xDB, 0x1A, false, true, false) == 1 && p.memory.get16(kC) == 0x1B);
  CHECK(tou(p, 0xDC, 0x2B, false, true, false) == 1 && p.memory.get16(kC) == 0x1C && tou(p, 0xDD, 0x1B, false, true, false) == 1 && p.memory.get16(kC) == 0x1D);
  CHECK(tou(p, '6', 7, true, true, false) == 1 && p.memory.get16(kC) == 0x1E && tou(p, 0xBD, 0x0C, true, true, false) == 1 && p.memory.get16(kC) == 0x1F && tou(p, 0xC0, 0x29, false, true, false) == 0);
  CHECK(tou(p, 0x20, 0x39, false, true, false) == 1 && p.memory.get16(kC) == ' ' && tou(p, 0x20, 0x39, true, false, false) == 1);
  CHECK(tou(p, 0xBE, 0x34, false, false, false) == 1 && p.memory.get16(kC) == '.' && tou(p, 0x61, 0x4F, false, false, false) == 1 && p.memory.get16(kC) == '1');
  CHECK(tou(p, 0x70, 0x3B, false, false, false) == 0 && tou(p, 'A', 0x1E | 0x8000, false, false, false) == 0 && tou(p, 0x100, 0, false, false, false) == 0);
  CHECK(tou(p, 'A', 0x1E, false, false, false, false, 1) == 1 && p.memory.get16(kC) == 'a' && p.memory.get16(kC + 2) == 0xAAAA);      // no room for the NUL
  CHECK(tou(p, 'A', 0x1E, false, false, false, false, 0) == 0);
  CHECK(p.call32(N::NtUserToUnicodeEx, 'A', 0x1E, 0, kC, 4, 0, 0x04090409) == 0);      // no key state
  clear_error(p);
  CHECK(p.call32(N::NtUserToUnicodeEx, 'A', 0x1E, 0x10, kC, 4, 0, 0x04090409) == 0 && last_error(p) == kNoAccess);
  CHECK(p.call32(N::NtUserToUnicodeEx, 'A', 0x1E, kB, 0x10, 4, 0, 0x04090409) == 0 && last_error(p) == kNoAccess);
  // key names (the quirks of Windows included)
  CHECK(key_name(p, 0x1D << 16) == "Ctrl" && key_name(p, 0x1E << 16) == "A" && key_name(p, 0x39 << 16) == "Space" && key_name(p, 0x01 << 16) == "Esc" && key_name(p, 0x3B << 16) == "F1");
  CHECK(key_name(p, 0x00370000) == "Num *" && key_name(p, 0x01370000) == "Prnt Scrn" && key_name(p, 0x02370000) == "Num *" && key_name(p, 0xe0370000) == "Num *" && key_name(p, 0xe1370000) == "Prnt Scrn");
  CHECK(key_name(p, 0x00450000) == "Pause" && key_name(p, 0x01450000) == "Num Lock" && key_name(p, 0x02450000) == "Pause" && key_name(p, 0xe1450000) == "Num Lock");
  CHECK(key_name(p, 0x00460000) == "Scroll Lock" && key_name(p, 0x01460000) == "Break" && key_name(p, 0x01480000) == "Up" && key_name(p, 0x00480000) == "Num 8");
  CHECK(key_name(p, 0x011d0000) == "Right Ctrl" && key_name(p, 0x021d0000) == "Ctrl" && key_name(p, 0xe01d0000) == "Ctrl" && key_name(p, 0xe11d0000) == "Right Ctrl");
  CHECK(key_name(p, 0x01380000) == "Right Alt" && key_name(p, 0x00380000) == "Alt" && key_name(p, 0x00360000) == "Right Shift" && key_name(p, 0x02360000) == "Shift" && key_name(p, 0x01530000) == "Delete");
  CHECK(key_name(p, 0x7F << 16) == "" && key_name(p, 0x017F0000) == "" && key_name(p, 0) == "");
  CHECK(key_name(p, 0x1D << 16, 4) == "Ctr" && key_name(p, 0x1D << 16, 5) == "Ctrl" && key_name(p, 0x1D << 16, 1) == "" && key_name(p, 0x1D << 16, 2) == "C");
  p.memory.put16(kD, 0xCCCC);
  CHECK(p.call(N::NtUserGetKeyNameText, 0x1D << 16, kD, 0) == 0 && p.memory.get16(kD) == 0xCCCC);      // nothing is written for a zero size
  CHECK(p.call(N::NtUserGetKeyNameText, 0x1D << 16, 0x10, 10) == 0);
  // layouts
  CHECK(p.call(N::NtUserGetKeyboardLayout, 0) == 0x04090409 && p.call(N::NtUserGetKeyboardLayout, 1) == 0x04090409 && p.call(N::NtUserGetKeyboardLayout, 99) == 0x04090409);
  CHECK(p.call(N::NtUserGetKeyboardLayoutList, 0, 0) == 1);
  p.memory.put64(kA, 0xFFFF); p.memory.put64(kA + 8, 0xFFFF);
  CHECK(p.call(N::NtUserGetKeyboardLayoutList, 5, kA) == 1 && p.memory.get64(kA) == 0x04090409 && p.memory.get64(kA + 8) == 0xFFFF);
  CHECK(p.call(N::NtUserGetKeyboardLayoutList, 1, kA) == 1 && p.call(N::NtUserGetKeyboardLayoutList, 1, 0x10) == 0);
  CHECK(p.call(N::NtUserGetKeyboardLayoutName, kA) == 1 && p.memory.get_wide_z(kA) == "00000409" && p.call(N::NtUserGetKeyboardLayoutName, 0x10) == 0);
  CHECK(p.call(N::NtUserActivateKeyboardLayout, 0x04090409, 0) == 0x04090409 && p.call(N::NtUserActivateKeyboardLayout, 1, 0) == 0x04090409);
  clear_error(p);
  CHECK(p.call(N::NtUserActivateKeyboardLayout, 0x04070407, 0) == 0 && last_error(p) == kErrorInvalidKeyboardHandle);
  p.memory.put_unicode_string(kE, kE + 0x40, "00000409");
  CHECK(p.call(N::NtUserLoadKeyboardLayoutEx, 0, 0, 0, kE, 0, 1) == 0x04090409 && p.call(N::NtUserGetKeyboardLayoutList, 0, 0) == 1);
  p.memory.put_unicode_string(kE, kE + 0x40, "00000407");
  CHECK(p.call(N::NtUserLoadKeyboardLayoutEx, 0, 0, 0, kE, 0, 0) == 0 && last_error(p) == kErrorFileNotFound);
  p.memory.put_unicode_string(kE, kE + 0x40, "zz");
  CHECK(p.call(N::NtUserLoadKeyboardLayoutEx, 0, 0, 0, kE, 0, 0) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserLoadKeyboardLayoutEx, 0, 0, 0, 0, 0x409, 0) == 0x04090409);
  CHECK(p.call(N::NtUserUnloadKeyboardLayout, 0x04090409) == 0 && last_error(p) == kErrorInvalidParameter);      // the last layout stays
  CHECK(p.call(N::NtUserUnloadKeyboardLayout, 0x04070407) == 0 && last_error(p) == kErrorInvalidKeyboardHandle);
  CHECK(p.call(N::NtUserGetKeyboardType, 0) == 4 && p.call(N::NtUserGetKeyboardType, 1) == 0 && p.call(N::NtUserGetKeyboardType, 2) == 12);

  // input contexts
  new_window(0x50001, 160, 1);
  new_window(0x50002, 160, 2);
  const uint64_t ctx = p.call(N::NtUserCreateInputContext, 0x1000);
  CHECK(ctx != 0 && p.call(N::NtUserQueryInputContext, ctx, 0) == 0x1000 && p.call(N::NtUserQueryInputContext, ctx, 1) == 1);
  CHECK(p.call(N::NtUserUpdateInputContext, ctx, 0, 0x2000) == 1 && p.call(N::NtUserQueryInputContext, ctx, 0) == 0x2000);
  clear_error(p);
  CHECK(p.call(N::NtUserQueryInputContext, ctx, 9) == 0 && last_error(p) == kErrorInvalidParameter && p.call(N::NtUserQueryInputContext, 0x555, 0) == 0 && last_error(p) == kErrorInvalidHandle);
  CHECK(p.call(N::NtUserAssociateInputContext, 0x50001, ctx, 0) == 0 && user_misc::window_input_context(0x50001) == ctx);      // AICR_OK
  CHECK(p.call(N::NtUserAssociateInputContext, 0x50002, ctx, 0) == 2);      // AICR_FAILED: the window belongs to another thread
  CHECK(p.call(N::NtUserAssociateInputContext, 0x99999, ctx, 0) == 2 && p.call(N::NtUserAssociateInputContext, 0x50001, 0x555, 0) == 2);
  g_focus = 0x50001;
  CHECK(p.call(N::NtUserAssociateInputContext, 0x50001, ctx, 0) == 1);      // AICR_FOCUS_CHANGED
  g_focus = 0;
  CHECK(p.call(N::NtUserAssociateInputContext, 0x50001, 0, 0x10) == 0 && user_misc::window_input_context(0x50001) == 0);
  p.call(N::NtUserAssociateInputContext, 0x50001, ctx, 0);
  Program other(161);
  setup(other);
  CHECK(other.call(N::NtUserDestroyInputContext, ctx) == 0 && last_error(p) != 0);
  CHECK(p.call(N::NtUserDestroyInputContext, ctx) == 1 && user_misc::window_input_context(0x50001) == 0 && p.call(N::NtUserQueryInputContext, ctx, 0) == 0);
  CHECK(p.call(N::NtUserSetAppImeLevel, 0x50001, 3) == 1 && p.call(N::NtUserGetAppImeLevel, 0x50001) == 3 && p.call(N::NtUserGetAppImeLevel, 0x50002) == 0);
  clear_error(p);
  CHECK(p.call(N::NtUserSetAppImeLevel, 0x99999, 3) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  // switching the IME off for a window is an association with nothing; a window nothing was associated with uses the default context of its thread
  CHECK(user_misc::window_input_context_raw(0x50001) == 0xFFFFFFFFu && user_misc::window_input_context_raw(0x50002) == 0xFFFFFFFFu);
  const uint64_t c3 = p.call(N::NtUserCreateInputContext, 7), c4 = p.call(N::NtUserCreateInputContext, 8);
  CHECK(c3 != 0 && c4 != 0 && c3 != c4 && p.call(N::NtUserQueryInputContext, c3, 2) == 160);
  CHECK(p.call(N::NtUserAssociateInputContext, 0x50001, c3, 0) == 0 && user_misc::window_input_context_raw(0x50001) == c3);
  CHECK(p.call(N::NtUserAssociateInputContext, 0x50001, 0, 0) == 0 && user_misc::window_input_context_raw(0x50001) == 0);
  // the contexts of a thread
  p.memory.put32(kD, 0);
  CHECK(p.call(N::NtUserBuildHimcList, 0, 8, kE, kD) == 2 && p.memory.get32(kD) == 2);
  CHECK((p.memory.get64(kE) == c3 && p.memory.get64(kE + 8) == c4) || (p.memory.get64(kE) == c4 && p.memory.get64(kE + 8) == c3));
  CHECK(p.call(N::NtUserBuildHimcList, 0, 1, kE, kD) == 1 && p.memory.get32(kD) == 2);      // room for one: the other is counted
  CHECK(p.call(N::NtUserBuildHimcList, 0, 0, 0, kD) == 0 && p.memory.get32(kD) == 2);
  CHECK(p.call(N::NtUserBuildHimcList, 7, 8, kE, kD) == 0 && p.memory.get32(kD) == 0);      // another thread has none
  CHECK(p.call(N::NtUserBuildHimcList, 0xFFFFFFFFu, 8, kE, kD) == 2);
  p.call(N::NtUserUpdateInputContext, c4, 1, 7);
  CHECK(p.call(N::NtUserBuildHimcList, 7, 8, kE, kD) == 1 && p.memory.get64(kE) == c4);
  // hot keys
  CHECK(p.call(N::NtUserGetImeHotKey, 0x10, kE, kE + 4, kE + 8) == 0);
  CHECK(p.call(N::NtUserSetImeHotKey, 0x10, 3, 0x20, 0x04090409, 0) == 1 && p.call(N::NtUserSetImeHotKey, 0x11, 1, 0x21, 0x04090409, 0) == 1);
  CHECK(p.call(N::NtUserGetImeHotKey, 0x10, kE, kE + 4, kE + 8) == 1 && p.memory.get32(kE) == 3 && p.memory.get32(kE + 4) == 0x20 && p.memory.get64(kE + 8) == 0x04090409);
  CHECK(p.call(N::NtUserSetImeHotKey, 0x10, 5, 0x22, 0x04090409, 0) == 1 && p.call(N::NtUserGetImeHotKey, 0x10, kE, 0, 0) == 1 && p.memory.get32(kE) == 5);
  CHECK(p.call(N::NtUserSetImeHotKey, 0x10, 0, 0, 0, 1) == 1 && p.call(N::NtUserGetImeHotKey, 0x10, kE, kE + 4, kE + 8) == 0);
  clear_error(p);
  CHECK(p.call(N::NtUserSetImeHotKey, 0x10, 0, 0, 0, 1) == 0 && last_error(p) == kErrorInvalidParameter && p.call(N::NtUserSetImeHotKey, 0x10, 0, 0, 0, 9) == 0);
  CHECK(p.call(N::NtUserSetImeHotKey, 0, 0, 0, 0, 2) == 1 && p.call(N::NtUserGetImeHotKey, 0x11, kE, kE + 4, kE + 8) == 0);
  // IMEINFOEX: by layout, by file name
  for (uint32_t i = 0; i < 352; i += 4) p.memory.put32(kF + i, 0);
  p.memory.put64(kF, 0x04090409);
  clear_error(p);
  CHECK(p.call(N::NtUserGetImeInfoEx, kF, 0) == 0 && last_error(p) == kErrorInvalidKeyboardHandle && p.call(N::NtUserGetImeInfoEx, kF, 5) == 0);
  p.memory.put_wide(kF + 188, "Test.IME");
  p.memory.put_wide(kF + 88, "A test IME");
  CHECK(p.call(N::NtUserSetImeInfoEx, kF) == 1 && p.call(N::NtUserSetImeInfoEx, kF) == 1);      // (a second call replaces)
  for (uint32_t i = 0; i < 352; i += 4) p.memory.put32(kG + i, 0);
  p.memory.put64(kG, 0x04090409);
  CHECK(p.call(N::NtUserGetImeInfoEx, kG, 0) == 1 && p.memory.get_wide_z(kG + 88) == "A test IME" && p.memory.get_wide_z(kG + 188) == "Test.IME");
  for (uint32_t i = 0; i < 352; i += 4) p.memory.put32(kG + i, 0);
  p.memory.put_wide(kG + 188, "test.ime");
  CHECK(p.call(N::NtUserGetImeInfoEx, kG, 1) == 1 && p.memory.get64(kG) == 0x04090409);      // by file name, ignoring case
  p.memory.put_wide(kG + 188, "other.ime");
  CHECK(p.call(N::NtUserGetImeInfoEx, kG, 1) == 0);
  p.memory.put64(kF, 0x11110000);
  CHECK(p.call(N::NtUserSetImeInfoEx, kF) == 0 && last_error(p) == kErrorInvalidKeyboardHandle);      // neither loaded nor an IME layout
  p.memory.put64(kF, 0xE0010411);
  CHECK(p.call(N::NtUserSetImeInfoEx, kF) == 1);
  // status and the small ones
  CHECK(p.call(N::NtUserNotifyIMEStatus, 0x50001, 1, 9) == 1 && p.call(N::NtUserNotifyIMEStatus, 0x50001, 0, 0) == 1);
  clear_error(p);
  CHECK(p.call(N::NtUserNotifyIMEStatus, 0x99999, 1, 9) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  CHECK(p.call(N::NtUserGetIMEShowStatus) == 0 && p.call(N::NtUserBroadcastImeShowStatusChange, 0x50001, 1) == 1 && p.call(N::NtUserGetIMEShowStatus) == 1 && p.call(N::NtUserCheckImeShowStatusInThread, 0x50001) == 1);
  CHECK(p.call(N::NtUserBroadcastImeShowStatusChange, 0x50001, 0) == 1 && p.call(N::NtUserGetIMEShowStatus) == 0);
  CHECK(p.call(N::NtUserNlsKbdSendIMENotification, 1, 2) == 1 && p.call(N::NtUserUpdatePerUserImmEnabling) == 1);
  CHECK(p.call(N::NtUserSetImeOwnerWindow, 0x50001, 1) == 1 && p.call(N::NtUserSetImeOwnerWindow, 0x99999, 1) == 0);
  CHECK(p.call(N::NtUserSetThreadLayoutHandles, 0x04090409, 0) == 1 && p.call(N::NtUserSetThreadLayoutHandles, 0x12345678, 0) == 0 && last_error(p) == kErrorInvalidKeyboardHandle);
  p.call(N::NtUserDestroyInputContext, c3);
  p.call(N::NtUserDestroyInputContext, c4);
  drop_window(0x50001);
  drop_window(0x50002);
  const uint64_t ctx2 = p.call(N::NtUserCreateInputContext, 5);
  win32k::process_exited(160);
  CHECK(p.call(N::NtUserQueryInputContext, ctx2, 0) == 0);
  // a thread without IME cannot make contexts; the process ending lifts that
  clear_error(p);
  CHECK(p.call(N::NtUserDisableThreadIme, 0) == 1);
  CHECK(p.call(N::NtUserCreateInputContext, 1) == 0 && last_error(p) == kErrorAccessDenied);
  win32k::process_exited(160);
  CHECK(p.call(N::NtUserCreateInputContext, 1) != 0);
  win32k::process_exited(160);
  set_thread_override(0);
}

// --- raw input and pointer services
void put_rid(FakeMemory& m, uint64_t a, uint16_t page, uint16_t usage, uint32_t flags, uint64_t hwnd) {
  m.put16(a, page); m.put16(a + 2, usage); m.put32(a + 4, flags); m.put64(a + 8, hwnd);
}

void test_input() {
  Program p(170);
  setup(p);
  set_thread_override(1);
  new_window(0x60001, 170, 1);
  new_window(0x60002, 171, 5);
  clear_error(p);
  put_rid(p.memory, kA, 1, 6, 0x100, 0x60001);
  put_rid(p.memory, kA + 16, 1, 2, 0, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kA, 2, 16) == 1);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kA, 2, 15) == 0 && last_error(p) == kErrorInvalidParameter && p.call(N::NtUserRegisterRawInputDevices, kA, 0, 16) == 0);
  put_rid(p.memory, kB, 1, 6, 0x100, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 0);      // RIDEV_INPUTSINK needs a window
  put_rid(p.memory, kB, 0, 6, 0, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 0);      // no usage page
  put_rid(p.memory, kB, 1, 6, 0, 0x60002);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 0);      // the window of another process
  put_rid(p.memory, kB, 1, 6, 1, 0x60001);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 0);      // RIDEV_REMOVE with a window
  put_rid(p.memory, kB, 1, 6, 0x4000, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 0);      // unknown flag
  CHECK(p.call(N::NtUserRegisterRawInputDevices, 0x10, 1, 16) == 0 && last_error(p) == kNoAccess);
  CHECK(p.call(N::NtUserGetRegisteredRawInputDevices, 0, kC, 16) == 0 && p.memory.get32(kC) == 2);
  p.memory.put32(kC, 1);
  CHECK(p.call32(N::NtUserGetRegisteredRawInputDevices, kD, kC, 16) == 0xFFFFFFFFu && p.memory.get32(kC) == 2 && last_error(p) == kErrorInsufficientBuffer);
  CHECK(p.call(N::NtUserGetRegisteredRawInputDevices, kD, kC, 16) == 2 && p.memory.get16(kD) == 1 && p.memory.get16(kD + 2) == 6 && p.memory.get32(kD + 4) == 0x100 && p.memory.get64(kD + 8) == 0x60001);
  // replacing and removing
  put_rid(p.memory, kB, 1, 6, 0, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 1 && p.call(N::NtUserGetRegisteredRawInputDevices, 0, kC, 16) == 0 && p.memory.get32(kC) == 2);
  put_rid(p.memory, kB, 1, 6, 1, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kB, 1, 16) == 1 && p.call(N::NtUserGetRegisteredRawInputDevices, 0, kC, 16) == 0 && p.memory.get32(kC) == 1);
  // the device list
  CHECK(p.call(N::NtUserGetRawInputDeviceList, 0, kC, 16) == 0 && p.memory.get32(kC) == 2);
  p.memory.put32(kC, 1);
  CHECK(p.call32(N::NtUserGetRawInputDeviceList, kD, kC, 16) == 0xFFFFFFFFu && p.memory.get32(kC) == 2);
  CHECK(p.call(N::NtUserGetRawInputDeviceList, kD, kC, 16) == 2 && p.memory.get64(kD) == 0x10001 && p.memory.get32(kD + 8) == 0 && p.memory.get64(kD + 16) == 0x10002 && p.memory.get32(kD + 24) == 1);
  CHECK(p.call32(N::NtUserGetRawInputDeviceList, kD, kC, 24) == 0xFFFFFFFFu);
  // device info
  p.memory.put32(kC, 0);
  CHECK(p.call(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x2000000B, 0, kC) == 0 && p.memory.get32(kC) == 32);
  CHECK(p.call(N::NtUserGetRawInputDeviceInfo, 0x10002, 0x2000000B, kD, kC) == 32 && p.memory.get32(kD) == 32 && p.memory.get32(kD + 4) == 1 && p.memory.get32(kD + 28) == 101);
  CHECK(p.call(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x2000000B, kD, kC) == 32 && p.memory.get32(kD + 4) == 0 && p.memory.get32(kD + 12) == 5);
  p.memory.put32(kC, 0);
  CHECK(p.call(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x20000007, 0, kC) == 0 && p.memory.get32(kC) > 20);
  const uint32_t chars = p.memory.get32(kC);
  p.memory.put32(kC, chars - 1);
  CHECK(p.call32(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x20000007, kD, kC) == 0xFFFFFFFFu && p.memory.get32(kC) == chars);
  CHECK(p.call32(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x20000007, kD, kC) == chars && p.memory.get_wide_z(kD).substr(0, 8) == "\\\\?\\HID#");
  clear_error(p);
  CHECK(p.call32(N::NtUserGetRawInputDeviceInfo, 0x777, 0x2000000B, kD, kC) == 0xFFFFFFFFu && last_error(p) == kErrorInvalidHandle);
  CHECK(p.call32(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x20000005, kD, kC) == 0xFFFFFFFFu && p.call32(N::NtUserGetRawInputDeviceInfo, 0x10001, 0x2000000B, kD, 0) == 0xFFFFFFFFu);
  // records
  uint8_t mouse[24] = {};
  mouse[0] = 1; mouse[12] = 5; mouse[16] = 0xFB;
  uint32_t h1, h2;
  {
    Locked lock;
    h1 = user_misc::record_raw_input(170, 0, 0x10001, 1, ConstByteView(mouse, 24), true);
    h2 = user_misc::record_raw_input(170, 0, 0x10001, 0, ConstByteView(mouse, 24), false);
  }
  CHECK(h1 != 0 && h2 != 0 && h1 != h2);
  p.memory.put32(kC, 0);
  CHECK(p.call(N::NtUserGetRawInputData, h2, 0x10000003, 0, kC, 24) == 0 && p.memory.get32(kC) == 48);
  CHECK(p.call(N::NtUserGetRawInputData, h2, 0x10000005, 0, kC, 24) == 0 && p.memory.get32(kC) == 24);
  p.memory.put32(kC, 47);
  CHECK(p.call32(N::NtUserGetRawInputData, h2, 0x10000003, kD, kC, 24) == 0xFFFFFFFFu && p.memory.get32(kC) == 48 && last_error(p) == kErrorInsufficientBuffer);
  CHECK(p.call(N::NtUserGetRawInputData, h2, 0x10000003, kD, kC, 24) == 48 && p.memory.get32(kD) == 0 && p.memory.get32(kD + 4) == 48 && p.memory.get64(kD + 8) == 0x10001);
  CHECK(p.memory.get8(kD + 24) == 1 && p.memory.get8(kD + 24 + 12) == 5 && p.memory.get8(kD + 24 + 16) == 0xFB);
  CHECK(p.call(N::NtUserGetRawInputData, h2, 0x10000005, kD, kC, 24) == 24);
  CHECK(p.call32(N::NtUserGetRawInputData, h2, 0x10000003, kD, kC, 16) == 0xFFFFFFFFu && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call32(N::NtUserGetRawInputData, 0x555, 0x10000003, kD, kC, 24) == 0xFFFFFFFFu && last_error(p) == kErrorInvalidHandle);
  CHECK(p.call32(N::NtUserGetRawInputData, h2, 0x10000001, kD, kC, 24) == 0xFFFFFFFFu);
  Program other(171);
  setup(other);
  CHECK(other.call32(N::NtUserGetRawInputData, h2, 0x10000003, kD, kC, 24) == 0xFFFFFFFFu && last_error(other) == kErrorInvalidHandle);
  // the buffer holds only the buffered records
  p.memory.put32(kC, 0);
  CHECK(p.call(N::NtUserGetRawInputBuffer, 0, kC, 24) == 0 && p.memory.get32(kC) == 48);
  p.memory.put32(kC, 10);
  CHECK(p.call32(N::NtUserGetRawInputBuffer, kD, kC, 24) == 0xFFFFFFFFu);
  p.memory.put32(kC, 0x100);
  CHECK(p.call(N::NtUserGetRawInputBuffer, kD, kC, 24) == 1 && p.memory.get32(kD + 4) == 48 && p.memory.get8(kD + 24 + 12) == 5);
  CHECK(p.call(N::NtUserGetRawInputBuffer, kD, kC, 24) == 0);
  // pointer services
  CHECK(p.call(N::NtUserIsMouseInPointerEnabled) == 0 && p.call(N::NtUserEnableMouseInPointer, 1) == 1 && p.call(N::NtUserIsMouseInPointerEnabled) == 1 && p.call(N::NtUserEnableMouseInPointer, 0) == 1 && p.call(N::NtUserIsMouseInPointerEnabled) == 0);
  CHECK(p.call(N::NtUserEnableMouseInPointerForThread) == 1 && p.call(N::NtUserIsMouseInPointerEnabled) == 1);
  set_thread_override(2);
  CHECK(p.call(N::NtUserIsMouseInPointerEnabled) == 0);
  set_thread_override(1);
  clear_error(p);
  CHECK(p.call(N::NtUserInitializeTouchInjection, 0, 1) == 0 && last_error(p) == kErrorInvalidParameter && p.call(N::NtUserInitializeTouchInjection, 10, 7) == 0 && p.call(N::NtUserInitializeTouchInjection, 10, 1) == 1);
  p.memory.put32(kA, 77);
  CHECK(p.call(N::NtUserGetPointerDevices, kA, 0) == 1 && p.memory.get32(kA) == 0 && p.call(N::NtUserGetPointerDevices, 0, 0) == 0 && p.call(N::NtUserGetPointerDevices, 0x10, 0) == 0);
  // what a process registered goes with it
  win32k::process_exited(170);
  put_rid(p.memory, kA, 1, 6, 0, 0);
  CHECK(p.call(N::NtUserRegisterRawInputDevices, kA, 1, 16) == 1);
  win32k::process_exited(170);
  CHECK(p.call(N::NtUserGetRegisteredRawInputDevices, 0, kC, 16) == 0 && p.memory.get32(kC) == 0);
  CHECK(p.call32(N::NtUserGetRawInputData, h1, 0x10000003, kD, kC, 24) == 0xFFFFFFFFu);
  drop_window(0x60001);
  drop_window(0x60002);
  set_thread_override(0);
}

// --- shell and process services, the multiplexers
void test_shell_and_calls() {
  Program p(180);
  setup(p);
  set_thread_override(1);
  new_window(0x70001, 180, 1);
  new_window(0x70002, 180, 1);
  new_window(0x70003, 181, 9);
  clear_error(p);
  CHECK(none(p, 4) == 0 && none(p, 3) == 0 && none(p, 5) == 0);
  CHECK(p.call(N::NtUserSetShellWindowEx, 0x99999, 0) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  CHECK(p.call(N::NtUserSetShellWindowEx, 0x70003, 0) == 0 && last_error(p) == kErrorAccessDenied);
  CHECK(p.call(N::NtUserSetShellWindowEx, 0x70001, 0x70002) == 1 && none(p, 4) == 0x70001);
  CHECK(p.call(N::NtUserSetShellWindowEx, 0x70002, 0) == 0 && last_error(p) == kErrorAccessDenied);      // there is a shell
  CHECK(p.call(N::NtUserSetProgmanWindow, 0x70002) == 0x70002 && none(p, 3) == 0x70002 && p.call(N::NtUserSetTaskmanWindow, 0x70002) == 0x70002 && none(p, 5) == 0x70002);
  CHECK(p.call(N::NtUserSetProgmanWindow, 0x99999) == 0 && last_error(p) == kErrorInvalidWindowHandle && none(p, 3) == 0x70002);
  CHECK(p.call(N::NtUserSetProgmanWindow, 0) == 0 && none(p, 3) == 0);
  drop_window(0x70001);
  CHECK(none(p, 4) == 0 && none(p, 5) == 0x70002 && p.call(N::NtUserSetShellWindowEx, 0x70002, 0) == 1);
  // shell hooks
  CHECK(p.call(N::NtUserRegisterShellHookWindow, 0x70002) == 1 && p.call(N::NtUserRegisterShellHookWindow, 0x70002) == 1);
  CHECK(p.call(N::NtUserRegisterShellHookWindow, 0x70003) == 0 && last_error(p) == kErrorAccessDenied && p.call(N::NtUserRegisterShellHookWindow, 0x99999) == 0);
  List<uint32_t> hooks(*system_allocator());
  {
    Locked lock;
    user_misc::shell_hook_windows(&hooks);
  }
  CHECK(hooks.count() == 1 && hooks[0] == 0x70002);
  CHECK(p.call(N::NtUserDeregisterShellHookWindow, 0x70002) == 1 && p.call(N::NtUserDeregisterShellHookWindow, 0x70002) == 0 && last_error(p) == kErrorInvalidParameter);
  // process default layout
  CHECK(p.call(N::NtUserGetProcessDefaultLayout, kA) == 1 && p.memory.get32(kA) == 0 && p.call(N::NtUserSetProcessDefaultLayout, 1) == 1);
  CHECK(p.call(N::NtUserGetProcessDefaultLayout, kA) == 1 && p.memory.get32(kA) == 1 && p.call(N::NtUserSetProcessDefaultLayout, 0x100) == 0 && p.call(N::NtUserGetProcessDefaultLayout, 0x10) == 0);
  // shutdown block reasons
  p.memory.put_wide(kB, "Saving");
  CHECK(p.call(N::NtUserShutdownBlockReasonCreate, 0x70002, kB) == 1);
  p.memory.put32(kC, 0);
  CHECK(p.call(N::NtUserShutdownBlockReasonQuery, 0x70002, 0, kC) == 0 && last_error(p) == kErrorInsufficientBuffer && p.memory.get32(kC) == 7);
  p.memory.put32(kC, 3);
  CHECK(p.call(N::NtUserShutdownBlockReasonQuery, 0x70002, kD, kC) == 0 && p.memory.get32(kC) == 7);
  p.memory.put32(kC, 20);
  CHECK(p.call(N::NtUserShutdownBlockReasonQuery, 0x70002, kD, kC) == 1 && p.memory.get_wide_z(kD) == "Saving");
  p.memory.put_wide(kB, "");
  CHECK(p.call(N::NtUserShutdownBlockReasonCreate, 0x70002, kB) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserShutdownBlockReasonCreate, 0x70003, kB) == 0 && p.call(N::NtUserShutdownBlockReasonCreate, 0x70002, 0) == 0);
  CHECK(p.call(N::NtUserShutdownReasonDestroy, 0x70002) == 1 && p.call(N::NtUserShutdownReasonDestroy, 0x70002) == 0 && last_error(p) == 1235);
  CHECK(p.call(N::NtUserShutdownBlockReasonQuery, 0x70002, kD, kC) == 0 && last_error(p) == 1235);
  // display affinity
  CHECK(p.call(N::NtUserGetWindowDisplayAffinity, 0x70002, kA) == 1 && p.memory.get32(kA) == 0);
  CHECK(p.call(N::NtUserSetWindowDisplayAffinity, 0x70002, 0x11) == 1 && p.call(N::NtUserGetWindowDisplayAffinity, 0x70002, kA) == 1 && p.memory.get32(kA) == 0x11);
  CHECK(p.call(N::NtUserSetWindowDisplayAffinity, 0x70002, 5) == 0 && last_error(p) == kErrorInvalidParameter && p.call(N::NtUserSetWindowDisplayAffinity, 0x70003, 1) == 0 && last_error(p) == kErrorAccessDenied);
  CHECK(p.call(N::NtUserSetWindowDisplayAffinity, 0x99999, 1) == 0 && p.call(N::NtUserGetWindowDisplayAffinity, 0x70002, 0x10) == 0);
  // beep, last input
  uint32_t beeps, last;
  user_misc::note_beep(&beeps, &last);
  CHECK(p.call(N::NtUserMessageBeep, 0x30) == 1);
  uint32_t beeps2, last2;
  user_misc::note_beep(&beeps2, &last2);
  CHECK(beeps2 == beeps + 1 && last2 == 0x30);
  const uint64_t t0 = none(p, 2);
  CHECK(t0 != 0 && none(p, 2) == t0);
  user_misc::note_last_input(12345);
  CHECK(none(p, 2) == 12345);
  drop_window(0x70002);
  drop_window(0x70003);

  // the multiplexers
  CHECK(none(p, 0) == 0x10010 && none(p, 1) == 0x100008 && none(p, 6) == 1);
  clear_error(p);
  CHECK(none(p, 77) == 0 && last_error(p) == kErrorInvalidParameter && one(p, 99) == 0 && two(p, 99) == 0);
  CHECK(one(p, 1, 0) == 0 && one(p, 13, 0) == 0 && one(p, 14, 0) == 0);      // services of other modules answer as a failing call
  CHECK(one(p, 11, 0) == 1 && one(p, 11, 1) == 0 && one(p, 11, 1) == 1);      // SetKeyboardAutoRepeat gives the previous value
  CHECK(one(p, 2, 0x1234) == 0 && two(p, 0, 0xABCD, 1) == 0xABCD);
  p.memory.put32(kA, 10); p.memory.put32(kA + 4, 20); p.memory.put32(kA + 8, 110); p.memory.put32(kA + 12, 220);
  p.memory.put32(kB, 0x00CF0000); p.memory.put32(kB + 4, 0); p.memory.put32(kB + 8, 1); p.memory.put32(kB + 12, 96);
  CHECK(two(p, 7, kA, kB) == 1 && p.memory.get32(kA) == uint32_t(10 - 8) && p.memory.get32(kA + 4) == uint32_t(20 - 31 - 20) && p.memory.get32(kA + 8) == 118);
  CHECK(two(p, 7, 0x10, kB) == 0 && last_error(p) == kNoAccess);
  // icons and thread records through NtUserCallNoParam_ExitingThread / ThreadDetach
  CHECK(p.call(N::NtUserSetCursor, 0) == 0);
  CHECK(none(p, 7) == 0 && none(p, 8) == 0);
  set_thread_override(0);
}

// Every service this module registers must be there, and no number may be registered twice.
#define MISC_SERVICES(X) \
  X(CreateAcceleratorTable) X(CopyAcceleratorTable) X(DestroyAcceleratorTable) X(CallNoParam) X(CallOneParam) X(CallTwoParam) \
  X(OpenClipboard) X(CloseClipboard) X(EmptyClipboard) X(SetClipboardData) X(GetClipboardData) X(EnumClipboardFormats) \
  X(IsClipboardFormatAvailable) X(CountClipboardFormats) X(GetClipboardOwner) X(GetOpenClipboardWindow) X(GetClipboardViewer) X(GetClipboardSequenceNumber) \
  X(SetClipboardViewer) X(ChangeClipboardChain) X(AddClipboardFormatListener) X(RemoveClipboardFormatListener) X(GetUpdatedClipboardFormats) X(GetPriorityClipboardFormat) \
  X(SetCursorIconData) X(FindExistingCursorIcon) X(DestroyCursor) X(GetIconInfo) X(GetIconSize) X(GetCursorFrameInfo) \
  X(SetCursor) X(GetCursor) X(ShowCursor) X(SetSystemCursor) X(GetCursorInfo) X(GetCursorPos) \
  X(SetCursorPos) X(ClipCursor) X(GetClipCursor) X(RegisterRawInputDevices) X(GetRegisteredRawInputDevices) X(GetRawInputDeviceList) \
  X(GetRawInputDeviceInfo) X(GetRawInputData) X(GetRawInputBuffer) X(EnableMouseInPointer) X(IsMouseInPointerEnabled) X(EnableMouseInPointerForThread) \
  X(InitializeTouchInjection) X(GetPointerDevices) X(GetKeyboardLayout) X(GetKeyboardLayoutList) X(GetKeyboardLayoutName) X(GetKeyboardType) \
  X(ActivateKeyboardLayout) X(LoadKeyboardLayoutEx) X(UnloadKeyboardLayout) X(MapVirtualKeyEx) X(VkKeyScanEx) X(ToUnicodeEx) \
  X(GetKeyNameText) X(CreateInputContext) X(DestroyInputContext) X(UpdateInputContext) X(QueryInputContext) X(AssociateInputContext) \
  X(GetAppImeLevel) X(SetAppImeLevel) X(GetImeInfoEx) X(SetImeInfoEx) X(GetImeHotKey) X(SetImeHotKey) X(BuildHimcList) X(NotifyIMEStatus) \
  X(DisableThreadIme) X(BroadcastImeShowStatusChange) X(GetIMEShowStatus) X(CheckImeShowStatusInThread) X(NlsKbdSendIMENotification) X(UpdatePerUserImmEnabling) \
  X(SetImeOwnerWindow) X(SetThreadLayoutHandles) X(CreateMenu) X(CreatePopupMenu) X(DestroyMenu) X(RemoveMenu) \
  X(DeleteMenu) X(CheckMenuItem) X(EnableMenuItem) X(HiliteMenuItem) X(SetMenuDefaultItem) X(SetMenuContextHelpId) \
  X(SetMenuFlagRtoL) X(ThunkedMenuInfo) X(ThunkedMenuItemInfo) X(SetMenu) X(DrawMenuBar) X(GetSystemMenu) \
  X(SetSystemMenu) X(EnumDisplaySettings) X(ChangeDisplaySettings) X(EnumDisplayDevices) X(GetDisplayConfigBufferSizes) X(QueryDisplayConfig) \
  X(DisplayConfigGetDeviceInfo) X(GetDpiForMonitor) X(GetDpiForCurrentProcess) X(GetSystemDpiForProcess) X(GetProcessDpiAwarenessContext) X(SetProcessDpiAwarenessContext) \
  X(LogicalToPerMonitorDPIPhysicalPoint) X(PerMonitorDPIPhysicalToLogicalPoint) X(SystemParametersInfo) X(SystemParametersInfoForDpi) X(SetSysColors) X(GetDoubleClickTime) \
  X(SetDoubleClickTime) X(SwapMouseButton) X(UpdatePerUserSystemParameters) X(ScaleSystemMetricForDPIWithoutCache) X(SetShellWindowEx) X(SetProgmanWindow) \
  X(SetTaskmanWindow) X(RegisterShellHookWindow) X(DeregisterShellHookWindow) X(GetProcessDefaultLayout) X(SetProcessDefaultLayout) X(ShutdownBlockReasonCreate) \
  X(ShutdownBlockReasonQuery) X(ShutdownReasonDestroy) X(SetWindowDisplayAffinity) X(GetWindowDisplayAffinity) X(MessageBeep) X(DwmValidateWindow) X(DwmWindowNotificationsEnabled) \
  X(DwmLockScreenUpdates) X(DwmKernelStartup) X(DwmKernelShutdown) X(SetWindowCompositionAttribute) X(GetWindowCompositionAttribute)
// NtTcosDwm(op, a, b, c, d): the service of dwmapi.dll.
void test_dwm() {
  Program p(190);
  setup(p);
  set_thread_override(1);
  new_window(0x71001, 190, 1);
  new_window(0x71002, 190, 1);
  new_window(0x71003, 191, 9);
  auto dwm = [&](uint32_t op, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) { return p.call32(N::NtTcosDwm, op, a, b, c, d); };
  const uint32_t kOk = 0, kNotFound = 0xC0000225u, kBadWindow = 0xC0000300u, kDenied = 0xC0000022u, kBadParam = 0xC000000Du, kMismatch = 0xC0000004u, kBadHandle = 0xC0000008u;
  // values of windows
  p.memory.put32(kA, 0x1234);
  CHECK(dwm(2, 0x71001, 20, kB, 4) == kNotFound);
  CHECK(dwm(1, 0x71001, 20, kA, 4) == kOk && dwm(2, 0x71001, 20, kB, 4) == kOk && p.memory.get32(kB) == 0x1234);
  CHECK(dwm(2, 0x71001, 20, kB, 8) == kMismatch && dwm(2, 0x71002, 20, kB, 4) == kNotFound);
  CHECK(dwm(1, 0x71003, 20, kA, 4) == kDenied && dwm(1, 0x99999, 20, kA, 4) == kBadWindow && dwm(2, 0x99999, 20, kB, 4) == kBadWindow);
  CHECK(dwm(1, 0x71001, 20, kA, 0) == kBadParam && dwm(1, 0x71001, 20, kA, 65) == kBadParam);
  p.memory.put32(kA, 0x5678);
  CHECK(dwm(1, 0x71001, 20, kA, 4) == kOk && dwm(2, 0x71001, 20, kB, 4) == kOk && p.memory.get32(kB) == 0x5678);      // replaced, not added
  CHECK(dwm(2, 0x71003, 20, kB, 4) == kNotFound);      // anyone may read
  CHECK(dwm(2, 0x71002, 9, kB, 16) == kOk && int32_t(p.memory.get32(kB)) == 10 && int32_t(p.memory.get32(kB + 8)) == 10 + 0x02 * 4);      // the extended frame bounds are the window rectangle
  CHECK(dwm(2, 0x71002, 9, kB, 8) == kMismatch);
  // the window goes: its values go with it
  drop_window(0x71002);
  CHECK(dwm(2, 0x71002, 9, kB, 16) == kBadWindow);
  new_window(0x71002, 190, 1);
  CHECK(dwm(2, 0x71002, 20, kB, 4) == kNotFound);
  // thumbnails
  CHECK(dwm(3, 0x71001, 0x71001, kC) == kBadParam && dwm(3, 0x71003, 0x71002, kC) == kDenied && dwm(3, 0x71001, 0x99999, kC) == kBadWindow && dwm(3, 0x99999, 0x71002, kC) == kBadWindow);
  CHECK(dwm(3, 0x71001, 0x71002, kC) == kOk);
  const uint32_t thumbnail = p.memory.get32(kC);
  CHECK(thumbnail != 0);
  for (uint32_t i = 0; i < 48; ++i) p.memory.put8(kD + i, uint8_t(i));
  CHECK(dwm(5, thumbnail, kD) == kOk && dwm(5, 0xDEAD, kD) == kBadHandle);
  CHECK(dwm(6, thumbnail, kE) == kOk && int32_t(p.memory.get32(kE)) == 4 * 2 && int32_t(p.memory.get32(kE + 4)) == 50);
  Program other(191);
  other.process.set_gs_base(kTeb);
  CHECK(other.call32(N::NtTcosDwm, 5, thumbnail, kD) == kBadHandle && other.call32(N::NtTcosDwm, 4, thumbnail) == kBadHandle);      // another process's thumbnail is none of its business
  CHECK(dwm(4, thumbnail) == kOk && dwm(4, thumbnail) == kBadHandle && dwm(6, thumbnail, kE) == kBadHandle);
  CHECK(dwm(3, 0x71001, 0x71002, kC) == kOk && p.memory.get32(kC) != thumbnail);      // (a new handle: the generation moved on)
  const uint32_t second = p.memory.get32(kC);
  drop_window(0x71001);      // the destination goes: so does the thumbnail
  CHECK(dwm(6, second, kE) == kBadHandle);
  new_window(0x71001, 190, 1);
  CHECK(dwm(3, 0x71001, 0x71002, kC) == kOk);
  drop_window(0x71002);      // the source goes: the thumbnail stays but has nothing to show
  CHECK(dwm(6, p.memory.get32(kC), kE) == kBadWindow);
  CHECK(dwm(99) == kBadParam);
  // the services of the compositor
  CHECK(p.call(N::NtUserDwmValidateWindow, 0x71001, 0) == 1 && p.call(N::NtUserDwmValidateWindow, 0x99999, 0) == 0 && last_error(p) == kErrorInvalidWindowHandle);
  CHECK(p.call(N::NtUserDwmWindowNotificationsEnabled, 0x71001) == 1);
  clear_error(p);
  CHECK(p.call(N::NtUserDwmLockScreenUpdates, 0) == 0 && last_error(p) == kErrorInvalidParameter);
  CHECK(p.call(N::NtUserDwmLockScreenUpdates, 1) == 1 && p.call(N::NtUserDwmLockScreenUpdates, 1) == 1 && p.call(N::NtUserDwmLockScreenUpdates, 0) == 1 && p.call(N::NtUserDwmLockScreenUpdates, 0) == 1);
  CHECK(p.call32(N::NtUserDwmKernelShutdown) == 0xC0000184u);
  CHECK(p.call32(N::NtUserDwmKernelStartup) == kOk && p.call32(N::NtUserDwmKernelStartup) == kDenied && p.call32(N::NtUserDwmKernelShutdown) == kOk);
  // composition attributes: {attrib, pad, data, size}
  p.memory.put32(kF, 0x7);
  p.memory.put32(kF + 4, 0);
  p.memory.put64(kF + 8, kG);
  p.memory.put64(kF + 16, 4);
  p.memory.put32(kG, 0xABCD);
  clear_error(p);
  CHECK(p.call(N::NtUserGetWindowCompositionAttribute, 0x71001, kF) == 0 && last_error(p) == kErrorInvalidParameter);      // never set
  CHECK(p.call(N::NtUserSetWindowCompositionAttribute, 0x71001, kF) == 1);
  p.memory.put32(kG, 0);
  CHECK(p.call(N::NtUserGetWindowCompositionAttribute, 0x71001, kF) == 1 && p.memory.get32(kG) == 0xABCD);
  CHECK(p.call(N::NtUserSetWindowCompositionAttribute, 0x71003, kF) == 0 && last_error(p) == kErrorAccessDenied);
  p.memory.put32(kF, 0);
  CHECK(p.call(N::NtUserSetWindowCompositionAttribute, 0x71001, kF) == 0 && last_error(p) == kErrorInvalidParameter);
  // a process that ends takes its thumbnails with it
  new_window(0x71002, 190, 1);
  CHECK(dwm(3, 0x71001, 0x71002, kC) == kOk);
  const uint32_t third = p.memory.get32(kC);
  CHECK(dwm(6, third, kE) == kOk);
  win32k::process_exited(190);
  CHECK(dwm(6, third, kE) == kBadHandle);
  drop_window(0x71001);
  drop_window(0x71002);
  drop_window(0x71003);
}

void test_registration() {
  CHECK(duplicate_registrations() == 0);
  static const WindowsSyscallNumber numbers[] = {
#define LIST(name) N::NtUser##name,
      MISC_SERVICES(LIST)
#undef LIST
  };
  for (WindowsSyscallNumber n : numbers) CHECK(handler_of(uint32_t(n)) != nullptr);
  CHECK(registered_count() >= sizeof(numbers) / sizeof(numbers[0]));
}

}  // namespace

namespace process::win32k {
// (this test runs without the other modules: their installers are empty here)
void install_gdi() {}
void install_user() {}
void install_composition() {}
}  // namespace process::win32k

int main() {
  System sys;      // (one system for all the tests: the module keeps its state for the lifetime of the allocator it was made with)
  gdi::set_screen_size(1024, 768);
  test_registration();
  test_menus();
  test_cursors();
  test_accelerators();
  test_clipboard();
  test_settings();
  test_monitors();
  test_keyboard();
  test_input();
  test_shell_and_calls();
  test_dwm();
  return finish("Win32kMiscTest");
}
