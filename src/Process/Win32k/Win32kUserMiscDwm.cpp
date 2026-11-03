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
#include "Process/Win32k/Win32kUserMisc.hpp"

// The desktop window manager's side of dwmapi.dll. Windows keeps this state in dwm.exe, which dwmapi reaches over ALPC; this OS composes in the kernel, so the state lives here and
// dwmapi.dll asks for it with NtTcosDwm (outside the win32k service range, registered with register_dwm_handler):
//   SetProp(hwnd, key, data, size)     per-window values: the DWMWA_* attributes, the frame margins, the blur-behind record, the iconic bitmaps (the caller must own the window)
//   GetProp(hwnd, key, out, size)      the same back; the size must be the one that was stored; DWM_PROP_EXTENDED_FRAME_BOUNDS is the window rectangle
//   RegisterThumbnail(dest, src, &h)   thumbnails: a live copy of `src` shown in `dest` (the caller must own `dest`; a window cannot show itself)
//   UnregisterThumbnail(h), UpdateThumbnail(h, props), ThumbnailSourceSize(h, &SIZE)
// The real NtUserDwm* services (the ones win32k itself offers the compositor) and Get/SetWindowCompositionAttribute are here too, as recorded state.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kOpSetProp = 1, kOpGetProp = 2, kOpRegisterThumbnail = 3, kOpUnregisterThumbnail = 4, kOpUpdateThumbnail = 5, kOpThumbnailSourceSize = 6;
constexpr uint32_t kPropExtendedFrameBounds = 9;      // DWMWA_EXTENDED_FRAME_BOUNDS: computed
constexpr uint32_t kMaxPropSize = 64, kThumbnailPropsSize = 48, kMaxProps = 4096, kCompositionAttributeBase = 0x1000, kMaxCompositionAttribute = 0x40;

struct Prop {
  uint32_t hwnd = 0, key = 0, size = 0;
  uint8_t data[kMaxPropSize] = {};
};
struct Thumbnail {
  uint32_t dest, src;
  uint64_t pid;
  uint8_t props[kThumbnailPropsSize] = {};
  Thumbnail(allocators::IAllocator&, uint32_t d, uint32_t s, uint64_t p) : dest(d), src(s), pid(p) {}
};
struct DwmState {
  List<Prop> props;
  Registry<Thumbnail> thumbnails;
  bool kernel_started = false;
  uint32_t screen_locks = 0;
  explicit DwmState(allocators::IAllocator& a) : props(a), thumbnails(a, 0x40) {}
};
UniquePointer<DwmState> g_dwm;
DwmState* st() { return lazy_state(g_dwm); }

Prop* find_prop(DwmState& s, uint32_t hwnd, uint32_t key) {
  for (Prop& p : s.props.view()) if (p.hwnd == hwnd && p.key == key) return &p;
  return nullptr;
}

uint64_t set_prop(Call& c, DwmState& s, uint32_t hwnd, uint32_t key, uint64_t data, uint32_t size) {
  if (size == 0 || size > kMaxPropSize) return kStatusInvalidParameter;
  if (!user::is_window(hwnd)) return kStatusInvalidWindowHandle;
  if (user::window_process(hwnd) != c.pid()) return kStatusAccessDenied;
  uint8_t bytes[kMaxPropSize];
  if (!c.read(data, ByteView(bytes, size))) return kStatusAccessViolation;
  Prop* p = find_prop(s, hwnd, key);
  if (!p) {
    if (s.props.count() >= kMaxProps) return kStatusNoMemory;
    if (!s.props.push_back(Prop{})) return kStatusNoMemory;
    p = s.props.get(s.props.count() - 1);
    p->hwnd = hwnd;
    p->key = key;
  }
  p->size = size;
  for (uint32_t i = 0; i < size; ++i) p->data[i] = bytes[i];
  return kStatusSuccess;
}

uint64_t get_prop(Call& c, DwmState& s, uint32_t hwnd, uint32_t key, uint64_t out, uint32_t size) {
  if (!user::is_window(hwnd)) return kStatusInvalidWindowHandle;
  if (key == kPropExtendedFrameBounds) {
    gdi::Rect r;
    if (size != sizeof(r)) return kStatusInfoLengthMismatch;
    if (!user::window_screen_rect(hwnd, &r)) return kStatusInvalidWindowHandle;
    return c.write_struct(out, r) ? kStatusSuccess : kStatusAccessViolation;
  }
  const Prop* p = find_prop(s, hwnd, key);
  if (!p) return kStatusNotFound;
  if (p->size != size) return kStatusInfoLengthMismatch;
  return c.write(out, ConstByteView(p->data, p->size)) ? kStatusSuccess : kStatusAccessViolation;
}

Thumbnail* thumbnail_of(Call& c, DwmState& s, uint32_t handle) {
  Thumbnail* t = s.thumbnails.find(handle);
  return t && t->pid == c.pid() ? t : nullptr;
}

uint64_t svc_tcos_dwm(Call& c) {
  DwmState* s = st();
  if (!s) return kStatusNoMemory;
  const uint32_t op = c.arg32(0);
  switch (op) {
    case kOpSetProp: return set_prop(c, *s, c.arg32(1), c.arg32(2), c.arg(3), c.arg32(4));
    case kOpGetProp: return get_prop(c, *s, c.arg32(1), c.arg32(2), c.arg(3), c.arg32(4));
    case kOpRegisterThumbnail: {
      const uint32_t dest = c.arg32(1), src = c.arg32(2);
      if (!user::is_window(dest) || !user::is_window(src)) return kStatusInvalidWindowHandle;
      if (dest == src) return kStatusInvalidParameter;
      if (user::window_process(dest) != c.pid()) return kStatusAccessDenied;
      uint32_t handle = 0;
      if (!s->thumbnails.create(&handle, dest, src, c.pid())) return kStatusNoMemory;
      if (!c.write_u64(c.arg(3), handle)) { s->thumbnails.destroy(handle); return kStatusAccessViolation; }
      return kStatusSuccess;
    }
    case kOpUnregisterThumbnail: return thumbnail_of(c, *s, c.arg32(1)) && s->thumbnails.destroy(c.arg32(1)) ? kStatusSuccess : kStatusInvalidHandle;
    case kOpUpdateThumbnail: {
      Thumbnail* t = thumbnail_of(c, *s, c.arg32(1));
      if (!t) return kStatusInvalidHandle;
      uint8_t bytes[kThumbnailPropsSize];
      if (!c.read(c.arg(2), ByteView(bytes, sizeof(bytes)))) return kStatusAccessViolation;
      for (uint32_t i = 0; i < sizeof(bytes); ++i) t->props[i] = bytes[i];
      return kStatusSuccess;
    }
    case kOpThumbnailSourceSize: {
      Thumbnail* t = thumbnail_of(c, *s, c.arg32(1));
      if (!t) return kStatusInvalidHandle;
      gdi::Rect r;
      if (!user::is_window(t->src) || !user::window_screen_rect(t->src, &r)) return kStatusInvalidWindowHandle;
      const int32_t size[2] = {r.right - r.left, r.bottom - r.top};
      return c.write_array(c.arg(2), size, 2) ? kStatusSuccess : kStatusAccessViolation;
    }
    default: return kStatusInvalidParameter;
  }
}

// ---- The services of win32k that belong to the compositor.
// NtUserDwmValidateWindow(hwnd, flags) / NtUserDwmWindowNotificationsEnabled(hwnd): BOOL; a window that exists is valid and is told about composition changes.
uint64_t svc_validate_window(Call& c) { return user::is_window(c.arg32(0)) ? 1 : c.fail(kErrorInvalidWindowHandle); }
// NtUserDwmLockScreenUpdates(BOOL lock): nested; the compositor only skips frames while it is above zero. False for an unlock without a lock.
uint64_t svc_lock_screen_updates(Call& c) {
  DwmState* s = st();
  if (!s) return 0;
  if (c.arg_bool(0)) { ++s->screen_locks; return 1; }
  if (s->screen_locks == 0) return c.fail(kErrorInvalidParameter);
  --s->screen_locks;
  return 1;
}
// NtUserDwmKernelStartup / NtUserDwmKernelShutdown: the compositor announces itself; one at a time.
uint64_t svc_kernel_startup(Call& c) {
  DwmState* s = st();
  if (!s) return kStatusNoMemory;
  if (s->kernel_started) return kStatusAccessDenied;
  s->kernel_started = true;
  return kStatusSuccess;
}
uint64_t svc_kernel_shutdown(Call&) {
  DwmState* s = st();
  if (!s || !s->kernel_started) return kStatusInvalidDeviceState;
  s->kernel_started = false;
  s->screen_locks = 0;
  return kStatusSuccess;
}

// NtUserSetWindowCompositionAttribute(hwnd, WINCOMPATTRDATA*) / NtUserGetWindowCompositionAttribute(hwnd, WINCOMPATTRDATA*): {DWORD attrib; PVOID data; SIZE_T size}.
struct CompositionAttribute { uint32_t attrib, pad; uint64_t data, size; };
bool read_attribute(Call& c, CompositionAttribute* a) {
  if (!c.read_struct(c.arg(1), a)) return false;
  return a->attrib != 0 && a->attrib < kMaxCompositionAttribute && a->size <= kMaxPropSize;
}
uint64_t svc_set_composition_attribute(Call& c) {
  DwmState* s = st();
  CompositionAttribute a;
  if (!s) return 0;
  if (!read_attribute(c, &a)) return c.fail(kErrorInvalidParameter);
  const uint64_t status = set_prop(c, *s, c.arg32(0), kCompositionAttributeBase + a.attrib, a.data, uint32_t(a.size));
  if (status == kStatusSuccess) return 1;
  return c.fail(status == kStatusInvalidWindowHandle ? kErrorInvalidWindowHandle : status == kStatusAccessDenied ? kErrorAccessDenied : kErrorInvalidParameter);
}
uint64_t svc_get_composition_attribute(Call& c) {
  DwmState* s = st();
  CompositionAttribute a;
  if (!s) return 0;
  if (!read_attribute(c, &a)) return c.fail(kErrorInvalidParameter);
  const uint64_t status = get_prop(c, *s, c.arg32(0), kCompositionAttributeBase + a.attrib, a.data, uint32_t(a.size));
  if (status == kStatusSuccess) return 1;
  return c.fail(status == kStatusInvalidWindowHandle ? kErrorInvalidWindowHandle : kErrorInvalidParameter);
}

}  // namespace

void dwm_window_destroyed(uint32_t hwnd) {
  DwmState* s = g_dwm.get();
  if (!s) return;
  for (uint64_t i = 0; i < s->props.count();) { if (s->props[i].hwnd == hwnd) s->props.remove_at(i); else ++i; }
  List<uint32_t> handles(*system_allocator());
  s->thumbnails.handles(&handles);
  for (uint32_t h : handles.view()) if (Thumbnail* t = s->thumbnails.find(h)) if (t->dest == hwnd) s->thumbnails.destroy(h);
}

void dwm_process_exited(uint64_t pid) {
  DwmState* s = g_dwm.get();
  if (!s) return;
  List<uint32_t> handles(*system_allocator());
  s->thumbnails.handles(&handles);
  for (uint32_t h : handles.view()) if (Thumbnail* t = s->thumbnails.find(h)) if (t->pid == pid) s->thumbnails.destroy(h);
}

void install_dwm() {
  using N = WindowsSyscallNumber;
  register_dwm_handler(svc_tcos_dwm);
  register_handler(N::NtUserDwmValidateWindow, svc_validate_window);
  register_handler(N::NtUserDwmWindowNotificationsEnabled, svc_validate_window);
  register_handler(N::NtUserDwmLockScreenUpdates, svc_lock_screen_updates);
  register_handler(N::NtUserDwmKernelStartup, svc_kernel_startup);
  register_handler(N::NtUserDwmKernelShutdown, svc_kernel_shutdown);
  register_handler(N::NtUserSetWindowCompositionAttribute, svc_set_composition_attribute);
  register_handler(N::NtUserGetWindowCompositionAttribute, svc_get_composition_attribute);
}

}  // namespace process::win32k::user_misc
