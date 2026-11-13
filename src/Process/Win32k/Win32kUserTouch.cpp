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

// Touches for the Windows programs. The desktop hands win32k each finger over a program's window (DesktopBridge::touch); the window under it at Down takes
// it for its whole life, as Windows' implicit pointer capture does:
// - Pointer messages (Windows 8): WM_POINTERENTER and WM_POINTERDOWN, WM_POINTERUPDATE as it moves, WM_POINTERUP (with POINTER_FLAG_CANCELED when the
//   desktop cancelled it) and WM_POINTERLEAVE. wParam is the pointer id and the POINTER_MESSAGE_FLAGs, lParam the screen point; GetPointerInfo and
//   GetPointerTouchInfo read the rest (pointer_touch_info). Pointer ids start at 2 (1 is the mouse's).
// - WM_TOUCH (Windows 7) instead, to a window that called RegisterTouchWindow (it or an ancestor under the finger): one TOUCHINPUT per message, read with
//   GetTouchInputInfo until CloseTouchInputHandle.
// - The first finger (primary: no other was down) that a program leaves to DefWindowProc becomes the mouse: the cursor moves there and the left button
//   goes down and up with it (promote_touch), so a program that knows nothing of touch is clicked as before.
#include "Process/Win32k/Win32kUserInternal.hpp"
#include "Utility/ByteStream.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }

struct LockGuard {
  ObjectManager* objects;
  LockGuard() : objects(system_objects()) { if (objects) objects->sync().lock(); }
  ~LockGuard() { if (objects) objects->sync().unlock(); }
};

constexpr uint32_t kWmPointerUpdate = 0x245, kWmPointerDown = 0x246, kWmPointerUp = 0x247, kWmPointerEnter = 0x249, kWmPointerLeave = 0x24A,
                   kWmTouch = 0x240;
// POINTER_FLAGS (the low half is also the POINTER_MESSAGE_FLAGs of wParam)
constexpr uint32_t kNew = 0x1, kInRange = 0x2, kInContact = 0x4, kFirstButton = 0x10, kPrimary = 0x2000, kConfidence = 0x4000, kCanceled = 0x8000,
                   kDown = 0x10000, kUpdate = 0x20000, kUp = 0x40000;
// TOUCHEVENTF_*
constexpr uint32_t kTouchMove = 0x1, kTouchDown = 0x2, kTouchUp = 0x4, kTouchInRange = 0x8, kTouchPrimary = 0x10;
constexpr uint32_t kPtTouch = 2;
constexpr uint32_t kMaxEnded = 8, kMaxInputs = 256;      // (ended contacts kept for GetPointerInfo; WM_TOUCH handles nobody closed)
constexpr uint64_t kTouchDevice = 0x7C000001u;           // the source device handle a program sees

uint64_t point_lparam(int32_t x, int32_t y) { return uint64_t(uint16_t(int16_t(x))) | uint64_t(uint16_t(int16_t(y))) << 16; }

uint32_t now_ms_state() { return now_ms_from_ns(S().last_now_ns); }

TouchContact* contact_of_desktop(uint32_t id) {
  for (TouchContact& c : S().touches) if (!c.ended && c.desktop_id == id) return &c;
  return nullptr;
}

TouchContact* contact_of_pointer(uint64_t pid, uint32_t pointer_id) {
  for (TouchContact& c : S().touches) if (c.pointer_id == pointer_id && c.pid == pid) return &c;
  return nullptr;
}

// Ended contacts beyond the few kept (the oldest first) and their touch inputs go.
void forget_old() {
  State& s = S();
  for (;;) {
    uint32_t ended = 0;
    TouchContact* oldest = nullptr;
    for (TouchContact& c : s.touches) {
      if (!c.ended) continue;
      ++ended;
      if (!oldest || c.frame < oldest->frame) oldest = &c;
    }
    if (ended <= kMaxEnded || !oldest) break;
    s.touches.erase(oldest);
  }
  for (;;) {
    uint32_t count = 0;
    TouchInputRecord* oldest = nullptr;
    for (TouchInputRecord& r : s.touch_inputs) {
      ++count;
      if (!oldest || r.handle < oldest->handle) oldest = &r;
    }
    if (count <= kMaxInputs || !oldest) break;
    s.touch_inputs.erase(oldest);
  }
}

// One event of a contact goes to its window: pointer messages, or one WM_TOUCH.
void deliver(TouchContact& c, uint8_t phase) {
  State& s = S();
  forget_old();
  c.frame = s.next_touch_frame++;
  c.time = now_ms_state();
  const uint32_t primary = c.primary ? kPrimary : 0;
  if (c.touch_api) {
    TouchInputRecord* r = s.touch_inputs.emplace();
    if (!r) return;
    r->handle = s.next_touch_input++;
    if (s.next_touch_input == 0) s.next_touch_input = 1;
    r->pid = c.pid;
    r->pointer_id = c.pointer_id;
    r->x = c.x;
    r->y = c.y;
    r->time = c.time;
    r->flags = (phase == 0 ? kTouchDown | kTouchInRange : phase == 1 ? kTouchMove | kTouchInRange : kTouchUp) | (c.primary ? kTouchPrimary : 0);
    c.flags = phase == 0 ? kDown | kInRange | kInContact | primary : phase == 1 ? kUpdate | kInRange | kInContact | primary : kUp | primary;
    if (!post_message(c.hwnd, kWmTouch, 1, r->handle)) s.touch_inputs.erase(r);
    return;
  }
  const uint32_t k = window_scale(find_window(c.hwnd));      // (an unaware window's points are its 96-dpi units)
  const uint64_t lp = point_lparam(dpi_down(c.x, k), dpi_down(c.y, k));
  auto post = [&](uint32_t message, uint32_t flags) {
    if (message != kWmPointerLeave && message != kWmPointerEnter) c.flags = flags;      // (GetPointerInfo answers for the down, update or up)
    post_message(c.hwnd, message, uint64_t(c.pointer_id) | uint64_t(flags & 0xFFFF) << 16, lp);
  };
  switch (phase) {
    case 0:
      post(kWmPointerEnter, kNew | kInRange | kInContact | kFirstButton | kConfidence | primary);
      post(kWmPointerDown, kNew | kInRange | kInContact | kFirstButton | kConfidence | primary | kDown);
      break;
    case 1: post(kWmPointerUpdate, kInRange | kInContact | kFirstButton | kConfidence | primary | kUpdate); break;
    default: {
      const uint32_t cancel = phase == 3 ? kCanceled : 0;
      post(kWmPointerUp, kConfidence | primary | kUp | cancel);
      post(kWmPointerLeave, kConfidence | primary | cancel);
      break;
    }
  }
}

}  // namespace

bool touch_input(uint32_t top_level, uint8_t phase, uint32_t id, int32_t x, int32_t y) {
  LockGuard g;
  return touch_input_locked(top_level, phase, id, x, y);
}

bool touch_input_locked(uint32_t top_level, uint8_t phase, uint32_t id, int32_t x, int32_t y) {
  if (!state_or_null() && !system_allocator()) return false;
  State& s = S();
  s.touch_seen = true;
  if (phase == 0) {
    if (contact_of_desktop(id)) return false;      // (the desktop gives each contact its own id)
    Window* top = find_window(top_level);
    Window* w = top ? window_at_point(x, y, true, 0) : nullptr;
    if (!w || top_level_of(w) != top) return false;
    Window* touch = w;
    while (touch && !touch->touch_window && touch != top) touch = touch->parent;
    const bool touch_api = touch && touch->touch_window;
    bool others = false;
    for (const TouchContact& c : s.touches) others |= !c.ended;
    TouchContact* c = s.touches.emplace();
    if (!c) return false;
    c->desktop_id = id;
    c->pointer_id = s.next_pointer_id++;
    if (s.next_pointer_id < 2) s.next_pointer_id = 2;
    c->hwnd = touch_api ? touch->handle : w->handle;
    c->pid = (touch_api ? touch : w)->pid;
    c->x = x;
    c->y = y;
    c->primary = !others;
    c->touch_api = touch_api;
    deliver(*c, 0);
    return true;
  }
  TouchContact* c = contact_of_desktop(id);
  if (!c) return false;
  c->x = x;
  c->y = y;
  if (phase == 1) {
    deliver(*c, 1);
    return true;
  }
  deliver(*c, phase == 2 ? 2 : 3);
  c->ended = true;
  return true;
}

// POINTER_TOUCH_INFO: POINTER_INFO (pointerType, pointerId, frameId, pointerFlags, sourceDevice, hwndTarget, ptPixelLocation, ptHimetricLocation,
// ptPixelLocationRaw, ptHimetricLocationRaw, dwTime, historyCount, InputData, dwKeyStates, PerformanceCount, ButtonChangeType; 96 bytes), then
// touchFlags, touchMask, rcContact, rcContactRaw, orientation, pressure (144 in all).
bool pointer_touch_info(uint64_t pid, uint32_t pointer_id, ByteView out) {
  if (!state_or_null() || out.size() != 144) return false;
  const TouchContact* c = contact_of_pointer(pid, pointer_id);
  if (!c) return false;
  out.fill(0);
  ByteWriter w(out);
  const uint32_t k = window_scale(find_window(c->hwnd));      // (an unaware window's points are its 96-dpi units)
  const int32_t px = dpi_down(c->x, k), py = dpi_down(c->y, k);
  const int32_t hx = int32_t(int64_t{px} * 2540 / 96), hy = int32_t(int64_t{py} * 2540 / 96);      // (HIMETRIC at 96 dpi)
  w.le32(kPtTouch);
  w.le32(c->pointer_id);
  w.le32(c->frame);
  w.le32(c->flags);
  w.le64(kTouchDevice);
  w.le64(c->hwnd);
  w.le32(uint32_t(px));
  w.le32(uint32_t(py));
  w.le32(uint32_t(hx));
  w.le32(uint32_t(hy));
  w.le32(uint32_t(px));
  w.le32(uint32_t(py));
  w.le32(uint32_t(hx));
  w.le32(uint32_t(hy));
  w.le32(c->time);
  w.le32(1);      // historyCount
  w.le32(0);      // InputData
  w.le32(0);      // dwKeyStates
  w.le64(0);      // PerformanceCount
  w.le32((c->flags & kDown) ? 1 : (c->flags & kUp) ? 2 : 0);      // POINTER_CHANGE_FIRSTBUTTON_DOWN / _UP
  w.le32(0);      // (padding)
  w.le32(0);      // touchFlags (TOUCH_FLAG_NONE)
  w.le32(0x4);    // touchMask: TOUCH_MASK_PRESSURE
  for (int i = 0; i < 2; ++i) {      // rcContact, rcContactRaw: a finger of 8 pixels
    w.le32(uint32_t(px - 4));
    w.le32(uint32_t(py - 4));
    w.le32(uint32_t(px + 4));
    w.le32(uint32_t(py + 4));
  }
  w.le32(0);      // orientation
  w.le32((c->flags & kInContact) ? 512 : 0);      // pressure (0 .. 1024)
  return true;
}

// TOUCHINPUT: x, y (hundredths of a pixel), hSource, dwID, dwFlags, dwMask, dwTime, dwExtraInfo, cxContact, cyContact (hundredths): 48 bytes.
uint32_t touch_input_info(uint64_t pid, uint32_t handle, ByteView out) {
  if (!state_or_null()) return 0;
  for (const TouchInputRecord& r : S().touch_inputs) {
    if (r.handle != handle || r.pid != pid) continue;
    if (out.empty()) return 1;
    if (out.size() != 48) return 0;
    ByteWriter w(out);
    const TouchContact* c = contact_of_pointer(pid, r.pointer_id);
    const uint32_t k = c ? window_scale(find_window(c->hwnd)) : 1;
    w.le32(uint32_t(dpi_down(r.x, k) * 100));
    w.le32(uint32_t(dpi_down(r.y, k) * 100));
    w.le64(kTouchDevice);
    w.le32(r.pointer_id);
    w.le32(r.flags);
    w.le32(0x4);      // dwMask: TOUCHINPUTMASKF_CONTACTAREA
    w.le32(r.time);
    w.le64(0);
    w.le32(800);
    w.le32(800);
    return 1;
  }
  return 0;
}

bool close_touch_input(uint64_t pid, uint32_t handle) {
  if (!state_or_null()) return false;
  for (TouchInputRecord& r : S().touch_inputs)
    if (r.handle == handle && r.pid == pid) {
      S().touch_inputs.erase(&r);
      return true;
    }
  return false;
}

uint64_t touch_window_state(uint32_t hwnd, uint32_t op, uint32_t flags) {
  Window* w = find_window(hwnd);
  if (!w) return 0;
  switch (op) {
    case 0: {
      const bool was = w->touch_window;
      w->touch_window = false;
      w->touch_flags = 0;
      return was ? 1 : 0;
    }
    case 1:
      w->touch_window = true;
      w->touch_flags = flags;
      return 1;
    default: return w->touch_window ? uint64_t(w->touch_flags) + 1 : 0;
  }
}

uint32_t top_level_at(int32_t x, int32_t y) {
  if (!state_or_null()) return 0;
  Window* w = window_at_point(x, y, true, 0);
  Window* top = w ? top_level_of(w) : nullptr;
  return top ? top->handle : 0;
}

bool touch_available() { return state_or_null() && S().touch_seen; }

void promote_touch(uint64_t pid, uint32_t message, uint64_t wparam, uint64_t lparam) {
  if (!state_or_null()) return;
  uint32_t pointer_id = 0;
  int32_t x = 0, y = 0;
  bool down = false, up = false;
  if (message == kWmTouch) {
    const TouchInputRecord* r = nullptr;
    for (const TouchInputRecord& t : S().touch_inputs) if (t.handle == uint32_t(lparam) && t.pid == pid) r = &t;
    if (!r) return;
    pointer_id = r->pointer_id;
    x = r->x;
    y = r->y;
    down = (r->flags & kTouchDown) != 0;
    up = (r->flags & kTouchUp) != 0;
    close_touch_input(pid, uint32_t(lparam));      // (DefWindowProc closes the handle, as on Windows)
  } else if (message == kWmPointerDown || message == kWmPointerUpdate || message == kWmPointerUp) {
    pointer_id = uint32_t(wparam & 0xFFFF);
    const TouchContact* c = contact_of_pointer(pid, pointer_id);
    const uint32_t k = c ? window_scale(find_window(c->hwnd)) : 1;      // (the program's point back in device pixels)
    x = dpi_up(int16_t(lparam & 0xFFFF), k);
    y = dpi_up(int16_t((lparam >> 16) & 0xFFFF), k);
    down = message == kWmPointerDown;
    up = message == kWmPointerUp;
  } else {
    return;
  }
  TouchContact* c = contact_of_pointer(pid, pointer_id);
  if (!c || !c->primary) return;
  if (up && !c->promoted_down) return;      // (its press was handled by the program: nothing to let go)
  if (!down && !up && !c->promoted_down) return;
  inject_move_locked(x, y);
  if (down) {
    inject_button_locked(1, true);
    c->promoted_down = true;
  } else if (up) {
    inject_button_locked(1, false);
    c->promoted_down = false;
  }
}

}  // namespace process::win32k::user
