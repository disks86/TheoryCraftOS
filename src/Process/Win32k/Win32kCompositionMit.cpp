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
#include "Process/Win32k/Win32kCompositionInternal.hpp"

// NtMIT*, NtMin*: the minuser input layer and input synthesis; NtModerncore*: window targets and navigation; input spaces and the other small services (docs/win32k/Composition.md).
namespace process::win32k::composition {

namespace {

constexpr uint32_t kMaxMinuserThreads = 1024, kMaxMinuserWindows = 4096, kMaxInputSpaces = 256, kMaxNavigation = 1024, kMaxTargetsPerProcess = 4096;
constexpr uint32_t kMaxPoints = 4096;

MinuserThread* find_thread(State& s, uint64_t pid, uint64_t tid) {
  for (MinuserThread& t : s.minuser_threads.view()) if (t.pid == pid && t.tid == tid) return &t;
  return nullptr;
}
MinuserThread* current_thread(Ctx& x) { return find_thread(x.st, x.call.pid(), x.call.tid()); }

// NtMITInitMinuserThread(DWORD flags (0)) / NtMITUninitMinuserThread(): the calling thread joins the minuser input layer, or leaves it
uint64_t svc_init_thread(Call& c, Ctx& x) {
  if (c.arg32(0) != 0) return kStatusInvalidParameter;
  if (current_thread(x)) return kStatusInvalidDeviceState;
  if (x.st.minuser_threads.count() >= kMaxMinuserThreads) return kStatusQuotaExceeded;
  MinuserThread t;
  t.pid = c.pid(); t.tid = c.tid();
  return x.st.minuser_threads.push_back(t) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_uninit_thread(Call& c, Ctx& x) {
  for (uint64_t i = 0; i < x.st.minuser_threads.count(); ++i)
    if (x.st.minuser_threads[i].pid == c.pid() && x.st.minuser_threads[i].tid == c.tid()) { x.st.minuser_threads.remove_at(i); wake_all(); return kStatusSuccess; }
  return kStatusInvalidDeviceState;
}
// NtMITActivateInputProcessing() / NtMITDeactivateInputProcessing()
uint64_t svc_activate(Call& c, Ctx& x) {
  MinuserThread* t = current_thread(x);
  if (!t || t->input_active) return kStatusInvalidDeviceState;
  t->input_active = true;
  return kStatusSuccess;
}
uint64_t svc_deactivate(Call& c, Ctx& x) {
  MinuserThread* t = current_thread(x);
  if (!t || !t->input_active) return kStatusInvalidDeviceState;
  t->input_active = false;
  return kStatusSuccess;
}
// NtMinQSuspendInputProcessing(BOOLEAN suspend): nests; a resume without a suspend is an error
uint64_t svc_suspend_input(Call& c, Ctx& x) {
  MinuserThread* t = current_thread(x);
  if (!t) return kStatusInvalidDeviceState;
  if ((c.arg(0) & 0xFF) != 0) {
    if (t->suspended == 0xFFFF) return kStatusQuotaExceeded;
    ++t->suspended;
  } else {
    if (t->suspended == 0) return kStatusInvalidDeviceState;
    --t->suspended;
  }
  return kStatusSuccess;
}
// NtMinQUpdateWakeMask(DWORD mask, DWORD* previous (optional))
uint64_t svc_update_wake_mask(Call& c, Ctx& x) {
  MinuserThread* t = current_thread(x);
  if (!t) return kStatusInvalidDeviceState;
  if (c.arg(1) != 0 && !c.write_u32(c.arg(1), t->wake_mask)) return kStatusAccessViolation;
  t->wake_mask = c.arg32(0);
  return kStatusSuccess;
}
// NtMITMinuserSetInputTransformOffset(LONG x, LONG y) / NtMinGetInputTransform(LONG* x, LONG* y)
uint64_t svc_set_transform_offset(Call& c, Ctx& x) {
  MinuserThread* t = current_thread(x);
  if (!t) return kStatusInvalidDeviceState;
  t->offset_x = c.argi(0);
  t->offset_y = c.argi(1);
  return kStatusSuccess;
}
uint64_t svc_get_transform(Call& c, Ctx& x) {
  MinuserThread* t = current_thread(x);
  if (!t) return kStatusInvalidDeviceState;
  return c.write_u32(c.arg(0), uint32_t(t->offset_x)) && c.write_u32(c.arg(1), uint32_t(t->offset_y)) ? kStatusSuccess : kStatusAccessViolation;
}
// NtMITMinuserWindowCreated(DWORD hwnd) / NtMITMinuserWindowDestroyed(DWORD hwnd)
uint64_t svc_window_created(Call& c, Ctx& x) {
  if (!current_thread(x)) return kStatusInvalidDeviceState;
  const uint32_t hwnd = c.arg32(0);
  if (hwnd == 0) return kStatusInvalidParameter;
  for (const Navigation& n : x.st.minuser_windows.view()) if (n.hwnd == hwnd) return kStatusInvalidParameter;
  if (x.st.minuser_windows.count() >= kMaxMinuserWindows) return kStatusQuotaExceeded;
  Navigation n;
  n.hwnd = hwnd; n.owner = c.pid();
  return x.st.minuser_windows.push_back(n) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_window_destroyed(Call& c, Ctx& x) {
  if (!current_thread(x)) return kStatusInvalidDeviceState;
  for (uint64_t i = 0; i < x.st.minuser_windows.count(); ++i) {
    if (x.st.minuser_windows[i].hwnd != c.arg32(0)) continue;
    if (x.st.minuser_windows[i].owner != c.pid()) return kStatusAccessDenied;
    x.st.minuser_windows.remove_at(i);
    return kStatusSuccess;
  }
  return kStatusNotFound;
}

// --- input synthesis -------------------------------------------------------------------------------------------------------------------------------------------------------

// NtMITSynthesizeKeyboardInput(DWORD virtual_key, DWORD scan_code, DWORD flags (KEYEVENTF_: 1 extended, 2 key up, 4 unicode (scan_code is the UTF-16 unit), 8 scan code))
uint64_t svc_synthesize_keyboard(Call& c, Ctx& x) {
  const uint32_t vk = c.arg32(0), scan = c.arg32(1), flags = c.arg32(2);
  if ((flags & ~0xFu) != 0) return kStatusInvalidParameter;
  const bool up = (flags & 2) != 0, extended = (flags & 1) != 0, unicode = (flags & 4) != 0;
  if (unicode) {
    if (vk != 0 || scan > 0xFFFF || (flags & 8)) return kStatusInvalidParameter;
  } else if (vk == 0 || vk > 254) {
    return kStatusInvalidParameter;
  }
  const InputSink sink = x.st.input;      // (a copy: the lock is given up below)
  rim_dispatch_key(x.st, vk, scan, !up, extended, now_ns(c.process()));
  Ctx::Unlocked u(x);
  if (unicode) { if (!up && sink.character) sink.character(char32_t(scan)); }
  else if (sink.key) sink.key(vk, scan, !up, extended);
  return kStatusSuccess;
}

constexpr uint32_t kMouseMove = 0x1, kLeftDown = 0x2, kLeftUp = 0x4, kRightDown = 0x8, kRightUp = 0x10, kMiddleDown = 0x20, kMiddleUp = 0x40, kXDown = 0x80, kXUp = 0x100,
                   kWheel = 0x800, kHWheel = 0x1000, kVirtualDesk = 0x4000, kAbsolute = 0x8000, kNoCoalesce = 0x2000;
constexpr uint32_t kMouseFlagsMask = kMouseMove | kLeftDown | kLeftUp | kRightDown | kRightUp | kMiddleDown | kMiddleUp | kXDown | kXUp | kWheel | kHWheel | kVirtualDesk | kAbsolute | kNoCoalesce;

// NtMITSynthesizeMouseInput(LONG dx, LONG dy, DWORD data, DWORD flags (MOUSEEVENTF_)). While a process intercepts the mouse (NtMITEnableMouseIntercept) the window manager does not
// see synthesized mouse input from other processes; the raw input manager still does.
uint64_t svc_synthesize_mouse(Call& c, Ctx& x) {
  const int32_t dx = c.argi(0), dy = c.argi(1);
  const uint32_t data = c.arg32(2), flags = c.arg32(3);
  if (flags == 0 || (flags & ~kMouseFlagsMask) != 0) return kStatusInvalidParameter;
  if ((flags & (kXDown | kXUp)) && data != 1 && data != 2) return kStatusInvalidParameter;
  State& s = x.st;
  const InputSink sink = s.input;
  // Where the cursor is: asked of the window manager once, then followed.
  if (!s.cursor_known && sink.cursor_position) {
    int32_t cx = 0, cy = 0;
    { Ctx::Unlocked u(x); sink.cursor_position(&cx, &cy); }
    s.cursor_x = cx; s.cursor_y = cy; s.cursor_known = true;
  }
  int32_t px = s.cursor_x, py = s.cursor_y;
  if (flags & kMouseMove) {
    if (flags & kAbsolute) {
      uint32_t w = 1024, h = 768;
      if (sink.screen_size) { Ctx::Unlocked u(x); sink.screen_size(&w, &h); }
      if (dx < 0 || dx > 65535 || dy < 0 || dy > 65535) return kStatusInvalidParameter;
      px = int32_t(uint64_t(uint32_t(dx)) * w / 65536);
      py = int32_t(uint64_t(uint32_t(dy)) * h / 65536);
    } else {
      const int64_t nx = int64_t(px) + dx, ny = int64_t(py) + dy;
      if (nx < -(1ll << 30) || nx > (1ll << 30) || ny < -(1ll << 30) || ny > (1ll << 30)) return kStatusInvalidParameter;
      px = int32_t(nx); py = int32_t(ny);
    }
    s.cursor_x = px; s.cursor_y = py; s.cursor_known = true;
  }
  const int32_t wheel = (flags & (kWheel | kHWheel)) ? int32_t(data) : 0;
  rim_dispatch_mouse(s, (flags & kMouseMove) && !(flags & kAbsolute) ? dx : 0, (flags & kMouseMove) && !(flags & kAbsolute) ? dy : 0, flags & ~kMouseMove & 0xFFF, wheel, now_ns(c.process()));
  if (s.mouse_intercept_pid != 0 && s.mouse_intercept_pid != c.pid()) return kStatusSuccess;
  Ctx::Unlocked u(x);
  if ((flags & kMouseMove) && sink.mouse_move) sink.mouse_move(px, py);
  struct Button { uint32_t flag; uint32_t button; bool down; };
  const Button buttons[] = {{kLeftDown, 1, true}, {kLeftUp, 1, false}, {kRightDown, 2, true}, {kRightUp, 2, false}, {kMiddleDown, 3, true}, {kMiddleUp, 3, false},
                            {kXDown, 3 + data, true}, {kXUp, 3 + data, false}};
  if (sink.mouse_button) for (const Button& b : buttons) if (flags & b.flag) sink.mouse_button(b.button, b.down);
  if ((flags & (kWheel | kHWheel)) && sink.mouse_wheel) sink.mouse_wheel(wheel, (flags & kHWheel) != 0);
  return kStatusSuccess;
}

// NtMITSynthesizeTouchInput: this system has no touch input pipeline in its window manager.
uint64_t svc_synthesize_touch(Call& c, Ctx& x) { return kStatusNotSupported; }

// NtMITEnableMouseIntercept() / NtMITDisableMouseIntercept(): one process at a time takes over the synthesized mouse input
uint64_t svc_enable_intercept(Call& c, Ctx& x) {
  if (x.st.mouse_intercept_pid != 0 && x.st.mouse_intercept_pid != c.pid()) return kStatusDeviceBusy;
  x.st.mouse_intercept_pid = c.pid();
  return kStatusSuccess;
}
uint64_t svc_disable_intercept(Call& c, Ctx& x) {
  if (x.st.mouse_intercept_pid != c.pid()) return kStatusInvalidDeviceState;
  x.st.mouse_intercept_pid = 0;
  return kStatusSuccess;
}

// NtMITPostWindowEventMessage(DWORD hwnd, DWORD message, UINT64 wparam, UINT64 lparam) / NtMITPostMouseInputMessage(same: a mouse message for the window)
uint64_t post_to_window(Call& c, Ctx& x) {
  const InputSink& sink = x.st.input;
  if (c.arg32(0) == 0 || c.arg32(1) == 0) return kStatusInvalidParameter;
  if (!sink.post_message) return kStatusNotSupported;
  return sink.post_message(c.arg32(0), c.arg32(1), c.arg(2), c.arg(3)) ? kStatusSuccess : kStatusInvalidWindowHandle;
}
uint64_t svc_post_window_event(Call& c, Ctx& x) { return post_to_window(c, x); }
uint64_t svc_post_mouse_message(Call& c, Ctx& x) {
  const uint32_t m = c.arg32(1);
  if (m < 0x200 || m > 0x20E) return kStatusInvalidParameter;      // WM_MOUSEFIRST..WM_MOUSEHWHEEL
  return post_to_window(c, x);
}
// NtMITPostThreadEventMessage(UINT64 thread, DWORD message, UINT64 wparam, UINT64 lparam)
uint64_t svc_post_thread_event(Call& c, Ctx& x) {
  const InputSink& sink = x.st.input;
  if (c.arg(0) == 0 || c.arg32(1) == 0) return kStatusInvalidParameter;
  if (!sink.post_thread_message) return kStatusNotSupported;
  return sink.post_thread_message(c.arg(0), c.arg32(1), c.arg(2), c.arg(3)) ? kStatusSuccess : kStatusInvalidParameter;
}

// --- input spaces, cursor ---------------------------------------------------------------------------------------------------------------------------------------------------

struct RectI { int32_t left, top, right, bottom; };
struct PointI { int32_t x, y; };

InputSpace* find_space(State& s, uint32_t id) {
  for (InputSpace& sp : s.input_spaces.view()) if (sp.id == id) return &sp;
  return nullptr;
}

// NtConfigureInputSpace(DWORD space, RECT* rect (null: remove the space), DWORD flags): a region of the screen that input can be routed by
uint64_t svc_configure_input_space(Call& c, Ctx& x) {
  const uint32_t id = c.arg32(0);
  if (id == 0) return kStatusInvalidParameter;
  InputSpace* sp = find_space(x.st, id);
  if (sp && sp->owner != c.pid()) return kStatusAccessDenied;
  if (c.arg(1) == 0) {
    if (!sp) return kStatusNotFound;
    for (uint64_t i = 0; i < x.st.input_spaces.count(); ++i) if (x.st.input_spaces[i].id == id) { x.st.input_spaces.remove_at(i); break; }
    if (x.st.cursor_space == id) x.st.cursor_space = 0;
    if (x.st.pointer_space == id) x.st.pointer_space = 0;
    return kStatusSuccess;
  }
  RectI r;
  if (!c.read_struct(c.arg(1), &r)) return kStatusAccessViolation;
  if (r.right < r.left || r.bottom < r.top || c.arg32(2) > 0xFF) return kStatusInvalidParameter;
  if (sp) { sp->left = r.left; sp->top = r.top; sp->right = r.right; sp->bottom = r.bottom; sp->flags = c.arg32(2); return kStatusSuccess; }
  if (x.st.input_spaces.count() >= kMaxInputSpaces) return kStatusQuotaExceeded;
  InputSpace n;
  n.id = id; n.flags = c.arg32(2); n.left = r.left; n.top = r.top; n.right = r.right; n.bottom = r.bottom; n.owner = c.pid();
  return x.st.input_spaces.push_back(n) ? kStatusSuccess : kStatusNoMemory;
}
// NtSetCursorInputSpace(DWORD space (0: none)) / NtSetPointerDeviceInputSpace(DWORD device, DWORD space (0: none))
uint64_t svc_set_cursor_space(Call& c, Ctx& x) {
  if (c.arg32(0) != 0 && !find_space(x.st, c.arg32(0))) return kStatusNotFound;
  x.st.cursor_space = c.arg32(0);
  return kStatusSuccess;
}
uint64_t svc_set_pointer_space(Call& c, Ctx& x) {
  if (c.arg32(0) != kRimMouseDevice) return kStatusNoSuchDevice;
  if (c.arg32(1) != 0 && !find_space(x.st, c.arg32(1))) return kStatusNotFound;
  x.st.pointer_space = c.arg32(1);
  return kStatusSuccess;
}
// NtInputSpaceRegionFromPoint(LONG x, LONG y, DWORD* space): the newest space whose rectangle holds the point
uint64_t svc_space_from_point(Call& c, Ctx& x) {
  const int32_t px = c.argi(0), py = c.argi(1);
  for (uint64_t i = x.st.input_spaces.count(); i-- > 0;) {
    const InputSpace& sp = x.st.input_spaces[i];
    if (px >= sp.left && px < sp.right && py >= sp.top && py < sp.bottom) return c.write_u32(c.arg(2), sp.id) ? kStatusSuccess : kStatusAccessViolation;
  }
  return kStatusNotFound;
}
// NtMapVisualRelativePoints(DWORD from_space, DWORD to_space, POINT* points, DWORD count): points of one space (0: the screen) in the coordinates of another
uint64_t svc_map_points(Call& c, Ctx& x) {
  const uint32_t count = c.arg32(3);
  if (count == 0 || count > kMaxPoints) return kStatusInvalidParameter;
  int64_t from_x = 0, from_y = 0, to_x = 0, to_y = 0;
  if (c.arg32(0) != 0) { InputSpace* sp = find_space(x.st, c.arg32(0)); if (!sp) return kStatusNotFound; from_x = int64_t(sp->left) + sp->offset_x; from_y = int64_t(sp->top) + sp->offset_y; }
  if (c.arg32(1) != 0) { InputSpace* sp = find_space(x.st, c.arg32(1)); if (!sp) return kStatusNotFound; to_x = int64_t(sp->left) + sp->offset_x; to_y = int64_t(sp->top) + sp->offset_y; }
  List<PointI> points(x.st.allocator);
  for (uint32_t i = 0; i < count; ++i) {
    PointI p;
    if (!c.read_struct(c.arg(2) + uint64_t{i} * sizeof(PointI), &p)) return kStatusAccessViolation;
    const int64_t nx = int64_t(p.x) + from_x - to_x, ny = int64_t(p.y) + from_y - to_y;
    if (nx < INT32_MIN || nx > INT32_MAX || ny < INT32_MIN || ny > INT32_MAX) return kStatusInvalidParameter;
    p.x = int32_t(nx); p.y = int32_t(ny);
    if (!points.push_back(p)) return kStatusNoMemory;
  }
  for (uint32_t i = 0; i < count; ++i) if (!c.write_struct(c.arg(2) + uint64_t{i} * sizeof(PointI), points[i])) return kStatusAccessViolation;
  return kStatusSuccess;
}
// NtEnableOneCoreTransformMode(BOOLEAN) / NtIsOneCoreTransformMode(BOOLEAN*)
uint64_t svc_enable_transform_mode(Call& c, Ctx& x) { x.st.onecore_transform = (c.arg(0) & 0xFF) != 0; return kStatusSuccess; }
uint64_t svc_is_transform_mode(Call& c, Ctx& x) {
  const uint8_t v = x.st.onecore_transform ? 1 : 0;
  return c.write(c.arg(0), ConstByteView(&v, 1)) ? kStatusSuccess : kStatusAccessViolation;
}
// NtSetShellCursorState(DWORD state (0..3))
uint64_t svc_set_shell_cursor_state(Call& c, Ctx& x) {
  if (c.arg32(0) > 3) return kStatusInvalidParameter;
  x.st.shell_cursor_state = c.arg32(0);
  return kStatusSuccess;
}
// NtHWCursorUpdatePointer: there is no hardware cursor plane
uint64_t svc_hw_cursor(Call& c, Ctx& x) { return kStatusNotSupported; }

// --- modern core ----------------------------------------------------------------------------------------------------------------------------------------------------------

uint64_t create_target(Call& c, Ctx& x, bool dcomp) {
  const uint32_t hwnd = c.arg32(0);
  if (hwnd == 0 || c.arg(1) == 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::WindowTarget) >= kMaxTargetsPerProcess) return kStatusQuotaExceeded;
  List<HandleTable::Info> all(x.st.allocator);
  x.st.handles.list(Kind::WindowTarget, &all);
  for (const HandleTable::Info& i : all.view()) {
    const WindowTarget* t = static_cast<WindowTarget*>(i.object);
    if (t->hwnd == hwnd && t->dcomp == dcomp) return kStatusInvalidParameter;      // one target of each kind per window
  }
  SharedPointer<WindowTarget> t = make_object<WindowTarget>(x);
  if (!t) return kStatusNoMemory;
  t->hwnd = hwnd; t->dcomp = dcomp; t->owner = c.pid();
  uint32_t status;
  SharedPointer<Object> base = t;
  const uint64_t h = new_handle(x, base, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(1), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
uint64_t destroy_target(Call& c, Ctx& x, bool dcomp) {
  uint32_t status;
  WindowTarget* t = lookup<WindowTarget>(x, c.arg(0), Kind::WindowTarget, 0, &status);
  if (!t) return status;
  if (t->dcomp != dcomp) return kStatusObjectTypeMismatch;
  x.st.handles.close(c.arg(0), c.pid());
  return kStatusSuccess;
}
// NtModerncoreCreateDCompositionHwndTarget / CreateGDIHwndTarget(DWORD hwnd, PHANDLE target); DestroyDCompositionHwndTarget / DestroyGDIHwndTarget(HANDLE target)
uint64_t svc_create_dcomp_target(Call& c, Ctx& x) { return create_target(c, x, true); }
uint64_t svc_create_gdi_target(Call& c, Ctx& x) { return create_target(c, x, false); }
uint64_t svc_destroy_dcomp_target(Call& c, Ctx& x) { return destroy_target(c, x, true); }
uint64_t svc_destroy_gdi_target(Call& c, Ctx& x) { return destroy_target(c, x, false); }

// NtModerncoreGetWindowContentVisual(HANDLE target, DWORD* resource_id): the id of the visual that shows the window's content (made on the first call)
uint64_t svc_get_content_visual(Call& c, Ctx& x) {
  uint32_t status;
  WindowTarget* t = lookup<WindowTarget>(x, c.arg(0), Kind::WindowTarget, kRightQuery, &status);
  if (!t) return status;
  if (!t->dcomp) return kStatusObjectTypeMismatch;
  if (t->content_visual == 0) t->content_visual = x.st.next_visual++;
  return c.write_u32(c.arg(1), t->content_visual) ? kStatusSuccess : kStatusAccessViolation;
}
bool navigation_registered(State& s, uint32_t hwnd) {
  for (const Navigation& n : s.navigation.view()) if (n.hwnd == hwnd) return true;
  return false;
}
// NtModerncoreGetNavigationWindowVisual(HANDLE target, DWORD* resource_id): only for a window registered as a navigation window
uint64_t svc_get_navigation_visual(Call& c, Ctx& x) {
  uint32_t status;
  WindowTarget* t = lookup<WindowTarget>(x, c.arg(0), Kind::WindowTarget, kRightQuery, &status);
  if (!t) return status;
  if (!t->dcomp) return kStatusObjectTypeMismatch;
  if (!navigation_registered(x.st, t->hwnd)) return kStatusNotFound;
  if (t->navigation_visual == 0) t->navigation_visual = x.st.next_visual++;
  return c.write_u32(c.arg(1), t->navigation_visual) ? kStatusSuccess : kStatusAccessViolation;
}
// NtModerncoreRegisterNavigationWindowHandle / RegisterEnhancedNavigationWindowHandle(DWORD hwnd) / NtModerncoreUnregisterNavigationWindowHandle(DWORD hwnd)
uint64_t register_navigation(Call& c, Ctx& x, bool enhanced) {
  const uint32_t hwnd = c.arg32(0);
  if (hwnd == 0) return kStatusInvalidParameter;
  for (Navigation& n : x.st.navigation.view()) {
    if (n.hwnd != hwnd) continue;
    if (n.owner != c.pid()) return kStatusAccessDenied;
    if (n.enhanced == enhanced) return kStatusInvalidParameter;      // registered that way already
    n.enhanced = enhanced;
    return kStatusSuccess;
  }
  if (x.st.navigation.count() >= kMaxNavigation) return kStatusQuotaExceeded;
  Navigation n;
  n.hwnd = hwnd; n.enhanced = enhanced; n.owner = c.pid();
  return x.st.navigation.push_back(n) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_register_navigation(Call& c, Ctx& x) { return register_navigation(c, x, false); }
uint64_t svc_register_enhanced_navigation(Call& c, Ctx& x) { return register_navigation(c, x, true); }
uint64_t svc_unregister_navigation(Call& c, Ctx& x) {
  for (uint64_t i = 0; i < x.st.navigation.count(); ++i) {
    if (x.st.navigation[i].hwnd != c.arg32(0)) continue;
    if (x.st.navigation[i].owner != c.pid()) return kStatusAccessDenied;
    x.st.navigation.remove_at(i);
    return kStatusSuccess;
  }
  return kStatusNotFound;
}
// NtModerncoreSetNavigationServiceSid(PSID sid, DWORD length): the SID is checked and kept (at most one; 0 length clears it)
uint64_t svc_set_navigation_sid(Call& c, Ctx& x) {
  const uint32_t length = c.arg32(1);
  if (length == 0) { x.st.navigation_sid.clear(); return kStatusSuccess; }
  if (length < 8 || length > 8 + 4 * 15) return kStatusInvalidParameter;
  uint8_t sid[8 + 4 * 15];
  if (!c.read(c.arg(0), ByteView(sid, length))) return kStatusAccessViolation;
  if (sid[0] != 1 || sid[1] > 15 || length != 8u + 4u * sid[1]) return kStatusInvalidParameter;
  x.st.navigation_sid.clear();
  for (uint32_t i = 0; i < length; ++i) if (!x.st.navigation_sid.push_back(sid[i])) { x.st.navigation_sid.clear(); return kStatusNoMemory; }
  return kStatusSuccess;
}

ResizeSync* find_resize(State& s, uint32_t hwnd) {
  for (ResizeSync& r : s.resize_syncs.view()) if (r.hwnd == hwnd) return &r;
  return nullptr;
}
// NtModerncoreEnableResizeLayoutSynchronization(DWORD hwnd, BOOLEAN enable) / NtModerncoreIsResizeLayoutSynchronizationEnabled(DWORD hwnd, BOOLEAN* enabled)
uint64_t svc_enable_resize_sync(Call& c, Ctx& x) {
  const uint32_t hwnd = c.arg32(0);
  if (hwnd == 0) return kStatusInvalidParameter;
  ResizeSync* r = find_resize(x.st, hwnd);
  const bool enable = (c.arg(1) & 0xFF) != 0;
  if (r) {
    if (r->owner != c.pid()) return kStatusAccessDenied;
    r->enabled = enable;
    return kStatusSuccess;
  }
  if (!enable) return kStatusSuccess;
  SharedPointer<SyncObject> sync = make_object<SyncObject>(x);
  if (!sync) return kStatusNoMemory;
  ResizeSync n;
  n.hwnd = hwnd; n.enabled = true; n.sync = sync; n.owner = c.pid();
  if (x.st.resize_syncs.count() >= kMaxMinuserWindows) return kStatusQuotaExceeded;
  return x.st.resize_syncs.push_back(n) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_is_resize_sync(Call& c, Ctx& x) {
  ResizeSync* r = find_resize(x.st, c.arg32(0));
  const uint8_t v = r && r->enabled ? 1 : 0;
  return c.write(c.arg(1), ConstByteView(&v, 1)) ? kStatusSuccess : kStatusAccessViolation;
}
// NtModerncoreGetResizeDCompositionSynchronizationObject(DWORD hwnd, PHANDLE sync): a handle to the synchronization object of a window with resize synchronization on
uint64_t svc_get_resize_sync_object(Call& c, Ctx& x) {
  ResizeSync* r = find_resize(x.st, c.arg32(0));
  if (!r || !r->enabled) return kStatusNotFound;
  if (c.arg(1) == 0) return kStatusInvalidParameter;
  uint32_t status;
  const uint64_t h = new_handle(x, r->sync, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(1), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}

}  // namespace

void install_mit() {
  COMPOSITION_REGISTER(NtMITInitMinuserThread, svc_init_thread);
  COMPOSITION_REGISTER(NtMITUninitMinuserThread, svc_uninit_thread);
  COMPOSITION_REGISTER(NtMITActivateInputProcessing, svc_activate);
  COMPOSITION_REGISTER(NtMITDeactivateInputProcessing, svc_deactivate);
  COMPOSITION_REGISTER(NtMinQSuspendInputProcessing, svc_suspend_input);
  COMPOSITION_REGISTER(NtMinQUpdateWakeMask, svc_update_wake_mask);
  COMPOSITION_REGISTER(NtMITMinuserSetInputTransformOffset, svc_set_transform_offset);
  COMPOSITION_REGISTER(NtMinGetInputTransform, svc_get_transform);
  COMPOSITION_REGISTER(NtMITMinuserWindowCreated, svc_window_created);
  COMPOSITION_REGISTER(NtMITMinuserWindowDestroyed, svc_window_destroyed);
  COMPOSITION_REGISTER(NtMITSynthesizeKeyboardInput, svc_synthesize_keyboard);
  COMPOSITION_REGISTER(NtMITSynthesizeMouseInput, svc_synthesize_mouse);
  COMPOSITION_REGISTER(NtMITSynthesizeTouchInput, svc_synthesize_touch);
  COMPOSITION_REGISTER(NtMITEnableMouseIntercept, svc_enable_intercept);
  COMPOSITION_REGISTER(NtMITDisableMouseIntercept, svc_disable_intercept);
  COMPOSITION_REGISTER(NtMITPostWindowEventMessage, svc_post_window_event);
  COMPOSITION_REGISTER(NtMITPostMouseInputMessage, svc_post_mouse_message);
  COMPOSITION_REGISTER(NtMITPostThreadEventMessage, svc_post_thread_event);
  COMPOSITION_REGISTER(NtConfigureInputSpace, svc_configure_input_space);
  COMPOSITION_REGISTER(NtSetCursorInputSpace, svc_set_cursor_space);
  COMPOSITION_REGISTER(NtSetPointerDeviceInputSpace, svc_set_pointer_space);
  COMPOSITION_REGISTER(NtInputSpaceRegionFromPoint, svc_space_from_point);
  COMPOSITION_REGISTER(NtMapVisualRelativePoints, svc_map_points);
  COMPOSITION_REGISTER(NtEnableOneCoreTransformMode, svc_enable_transform_mode);
  COMPOSITION_REGISTER(NtIsOneCoreTransformMode, svc_is_transform_mode);
  COMPOSITION_REGISTER(NtSetShellCursorState, svc_set_shell_cursor_state);
  COMPOSITION_REGISTER(NtHWCursorUpdatePointer, svc_hw_cursor);
  COMPOSITION_REGISTER(NtModerncoreCreateDCompositionHwndTarget, svc_create_dcomp_target);
  COMPOSITION_REGISTER(NtModerncoreCreateGDIHwndTarget, svc_create_gdi_target);
  COMPOSITION_REGISTER(NtModerncoreDestroyDCompositionHwndTarget, svc_destroy_dcomp_target);
  COMPOSITION_REGISTER(NtModerncoreDestroyGDIHwndTarget, svc_destroy_gdi_target);
  COMPOSITION_REGISTER(NtModerncoreGetWindowContentVisual, svc_get_content_visual);
  COMPOSITION_REGISTER(NtModerncoreGetNavigationWindowVisual, svc_get_navigation_visual);
  COMPOSITION_REGISTER(NtModerncoreRegisterNavigationWindowHandle, svc_register_navigation);
  COMPOSITION_REGISTER(NtModerncoreRegisterEnhancedNavigationWindowHandle, svc_register_enhanced_navigation);
  COMPOSITION_REGISTER(NtModerncoreUnregisterNavigationWindowHandle, svc_unregister_navigation);
  COMPOSITION_REGISTER(NtModerncoreSetNavigationServiceSid, svc_set_navigation_sid);
  COMPOSITION_REGISTER(NtModerncoreEnableResizeLayoutSynchronization, svc_enable_resize_sync);
  COMPOSITION_REGISTER(NtModerncoreIsResizeLayoutSynchronizationEnabled, svc_is_resize_sync);
  COMPOSITION_REGISTER(NtModerncoreGetResizeDCompositionSynchronizationObject, svc_get_resize_sync_object);
}

}  // namespace process::win32k::composition
