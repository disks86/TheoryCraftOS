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
// Window stations and desktops: create, open, close, enumerate, the process's station, the thread's desktop, the input desktop.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }
constexpr uint32_t kErrorBusy = 170, kErrorAlreadyExists = 183;
constexpr uint32_t UOI_FLAGS = 1, UOI_NAME = 2, UOI_TYPE = 3, UOI_USER_SID = 4, UOI_HEAPSIZE = 5, UOI_IO = 6;

// The last path component of the object name in an OBJECT_ATTRIBUTES (x64: Length 0, RootDirectory 8, ObjectName 16, Attributes 24).
bool attributes_name(Call& call, uint64_t address, WideString* name, uint64_t* root) {
  if (address == 0) return false;
  uint64_t name_ptr = 0;
  if (!call.read_u64(address + 8, root) || !call.read_u64(address + 16, &name_ptr)) return false;
  WideString full(S().alloc);
  if (name_ptr != 0 && !call.read_unicode_string(name_ptr, &full)) return false;
  uint64_t start = 0;
  for (uint64_t i = 0; i < full.size(); ++i) if (full[i] == u'\\') start = i + 1;
  for (uint64_t i = start; i < full.size(); ++i) name->append(full[i]);
  return true;
}

WinSta* find_station(ArrayView<const char16_t> name) {
  for (WinSta& w : S().stations.items()) if (w.name.equals(name, true)) return &w;
  return nullptr;
}

uint32_t new_handle(Call& call, WinSta* station, Desktop* desktop) {
  State& s = S();
  ObjHandle* h = s.object_handles.create();
  if (!h) return 0;
  h->pid = call.pid();
  h->station = station;
  h->desktop = desktop;
  if (station) ++station->refs;
  if (desktop) ++desktop->refs;
  return h->handle;
}

WinSta* process_station(Call& call) {
  State& s = S();
  ProcessInfo& p = process_info(call.pid());
  if (ObjHandle* h = s.object_handles.find(p.winsta)) return h->station;
  default_desktop();
  Desktop* d = s.input_desktop;
  if (!d) return nullptr;
  p.winsta = new_handle(call, d->station, nullptr);
  return d->station;
}

// HWINSTA NtUserCreateWindowStation(OBJECT_ATTRIBUTES*, ACCESS_MASK, ...)
uint64_t nt_create_window_station(Call& call) {
  State& s = S();
  WideString name(s.alloc);
  uint64_t root = 0;
  if (!attributes_name(call, call.arg(0), &name, &root)) return call.fail(kErrorInvalidParameter);
  default_desktop();
  WinSta* st = find_station(name.view());
  if (!st) {
    if (name.empty()) return call.fail(kErrorInvalidParameter);
    st = s.stations.create(s.alloc);
    if (!st) return call.fail(kErrorNotEnoughMemory);
    st->name.append(name.view());
  } else {
    call.set_last_error(kErrorAlreadyExists);
  }
  const uint32_t h = new_handle(call, st, nullptr);
  return h ? h : call.fail(kErrorNotEnoughMemory);
}
uint64_t nt_open_window_station(Call& call) {
  WideString name(S().alloc);
  uint64_t root = 0;
  if (!attributes_name(call, call.arg(0), &name, &root)) return call.fail(kErrorInvalidParameter);
  default_desktop();
  WinSta* st = find_station(name.view());
  if (!st) return call.fail(kErrorFileNotFound);
  const uint32_t h = new_handle(call, st, nullptr);
  return h ? h : call.fail(kErrorNotEnoughMemory);
}
uint64_t nt_close_window_station(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || h->pid != call.pid() || !h->station || h->desktop) return call.fail(kErrorInvalidHandle);
  if (process_info(call.pid()).winsta == h->handle) return call.fail(kErrorAccessDenied);
  if (h->station->refs) --h->station->refs;
  s.object_handles.destroy(h);
  return 1;
}
uint64_t nt_get_process_window_station(Call& call) {
  if (!process_station(call)) return 0;
  return process_info(call.pid()).winsta;
}
uint64_t nt_set_process_window_station(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || h->pid != call.pid() || !h->station || h->desktop) return call.fail(kErrorInvalidHandle);
  process_info(call.pid()).winsta = h->handle;
  return 1;
}

Desktop* find_desktop(WinSta* st, ArrayView<const char16_t> name) {
  for (Desktop& d : S().desktops.items()) if (d.station == st && d.name.equals(name, true)) return &d;
  return nullptr;
}

// the station the attributes name through their root directory handle, else the process's
WinSta* station_for(Call& call, uint64_t root) {
  if (root != 0) if (ObjHandle* h = S().object_handles.find(uint32_t(root))) if (h->station && !h->desktop) return h->station;
  return process_station(call);
}

// HDESK NtUserCreateDesktopEx(OBJECT_ATTRIBUTES*, UNICODE_STRING* device, DEVMODEW*, DWORD flags, ACCESS_MASK, ULONG heap)
uint64_t nt_create_desktop_ex(Call& call) {
  State& s = S();
  WideString name(s.alloc);
  uint64_t root = 0;
  if (!attributes_name(call, call.arg(0), &name, &root) || name.empty()) return call.fail(kErrorInvalidParameter);
  WinSta* st = station_for(call, root);
  if (!st) return call.fail(kErrorInvalidParameter);
  Desktop* d = find_desktop(st, name.view());
  if (d) {
    call.set_last_error(kErrorAlreadyExists);
  } else {
    d = s.desktops.create(s.alloc);
    if (!d) return call.fail(kErrorNotEnoughMemory);
    d->name.append(name.view());
    d->station = st;
    auto make = [&](Class* cls, bool visible) -> Window* {
      Window* w = s.windows.create(s.alloc);
      if (!w) return nullptr;
      w->cls = cls;
      if (cls) ++cls->windows;
      w->desktop = d;
      w->style = wn::WS_CLIPCHILDREN | (visible ? wn::WS_VISIBLE : 0);
      w->bounds = w->client = Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())};
      return w;
    };
    d->root = make(s.desktop_class, true);
    d->message_root = make(s.message_class, false);
    if (!d->root || !d->message_root) return call.fail(kErrorNotEnoughMemory);
    link_window(d->message_root, d->root, nullptr, true);
  }
  const uint32_t h = new_handle(call, st, d);
  return h ? h : call.fail(kErrorNotEnoughMemory);
}
uint64_t nt_open_desktop(Call& call) {
  WideString name(S().alloc);
  uint64_t root = 0;
  if (!attributes_name(call, call.arg(0), &name, &root)) return call.fail(kErrorInvalidParameter);
  WinSta* st = station_for(call, root);
  if (!st) return call.fail(kErrorFileNotFound);
  Desktop* d = find_desktop(st, name.view());
  if (!d) return call.fail(kErrorFileNotFound);
  const uint32_t h = new_handle(call, st, d);
  return h ? h : call.fail(kErrorNotEnoughMemory);
}
uint64_t nt_open_input_desktop(Call& call) {
  default_desktop();
  Desktop* d = S().input_desktop;
  if (!d) return 0;
  const uint32_t h = new_handle(call, d->station, d);
  return h ? h : call.fail(kErrorNotEnoughMemory);
}
uint64_t nt_close_desktop(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || h->pid != call.pid() || !h->desktop) return call.fail(kErrorInvalidHandle);
  for (const ThreadDesktop& t : s.thread_desktops) if (t.handle == h->handle && t.thread == thread_of(call)) return call.fail(kErrorBusy);
  if (h->desktop->refs) --h->desktop->refs;
  if (h->station && h->station->refs) --h->station->refs;
  s.object_handles.destroy(h);
  return 1;
}
uint64_t nt_switch_desktop(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || !h->desktop) return call.fail(kErrorInvalidHandle);
  default_desktop();
  if (s.initial_desktop == nullptr) s.initial_desktop = s.input_desktop;
  s.input_desktop = h->desktop;
  invalidate_screen_rect(Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())}, nullptr);
  wake_all();
  return 1;
}
uint64_t nt_get_thread_desktop(Call& call) {
  State& s = S();
  const uint32_t tid = call.arg32(0);
  Desktop* d = nullptr;
  for (const ThreadDesktop& t : s.thread_desktops) {
    if (uint32_t(t.thread) != tid) continue;
    if (ObjHandle* h = s.object_handles.find(t.handle)) {
      if (h->pid == call.pid()) return h->handle;
      d = h->desktop;
    }
  }
  if (!d) {
    bool known = false;
    for (Queue& q : s.queues) if (uint32_t(q.thread) == tid) known = true;
    if (!known && (uint64_t{tid} & 0xFFFFF) != call.pid()) return call.fail(kErrorInvalidParameter);
    default_desktop();
    d = s.initial_desktop ? s.initial_desktop : s.input_desktop;
  }
  if (!d) return 0;
  const uint32_t h = new_handle(call, d->station, d);
  return h;
}
uint64_t nt_set_thread_desktop(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || h->pid != call.pid() || !h->desktop) return call.fail(kErrorInvalidHandle);
  const uint64_t tid = thread_of(call);
  for (Window& w : s.windows.items()) if (w.thread == tid && w.parent) return call.fail(kErrorBusy);
  for (Hook& k : s.hooks.items()) if (k.thread == tid) return call.fail(kErrorBusy);
  for (ThreadDesktop& t : s.thread_desktops) if (t.thread == tid) { t.handle = h->handle; return 1; }
  ThreadDesktop t;
  t.thread = tid;
  t.handle = h->handle;
  return s.thread_desktops.push_back(t) ? 1 : call.fail(kErrorNotEnoughMemory);
}

// NTSTATUS NtUserBuildNameList(HWINSTA, ULONG size, ntuser_name_list* buffer, ULONG* ret_size): { ULONG size; ULONG count; WCHAR strings[] }, NUL-terminated names one after the other
uint64_t nt_build_name_list(Call& call) {
  State& s = S();
  default_desktop();
  WinSta* st = nullptr;
  if (call.arg(0) != 0) {
    ObjHandle* h = s.object_handles.find(call.arg32(0));
    if (!h || !h->station) return kStatusInvalidHandle;
    st = h->station;
  }
  List<char16_t> text(s.alloc);
  uint32_t count = 0;
  if (!st) {
    for (WinSta& w : s.stations.items()) { for (char16_t c : w.name.view()) text.push_back(c); text.push_back(0); ++count; }
  } else {
    for (Desktop& d : s.desktops.items()) if (d.station == st) { for (char16_t c : d.name.view()) text.push_back(c); text.push_back(0); ++count; }
  }
  const uint64_t needed = 8 + text.count() * 2;
  if (call.arg(3) != 0 && !call.write_u32(call.arg(3), uint32_t(needed))) return kStatusAccessViolation;
  if (call.arg32(1) < needed) return kStatusBufferTooSmall;
  if (!call.write_u32(call.arg(2), uint32_t(needed)) || !call.write_u32(call.arg(2) + 4, count)) return kStatusAccessViolation;
  if (text.count() && call.write_wide(call.arg(2) + 8, text.view(), uint32_t(text.count()), false) < 0) return kStatusAccessViolation;
  return kStatusSuccess;
}

// BOOL NtUserGetObjectInformation(HANDLE, INT index, void* info, DWORD len, DWORD* needed)
uint64_t nt_get_object_information(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || h->pid != call.pid()) return call.fail(kErrorInvalidHandle);
  const uint32_t index = call.arg32(1), len = call.arg32(3);
  uint8_t raw[64] = {};
  uint32_t size = 0;
  List<char16_t> text(s.alloc);
  switch (index) {
    case UOI_FLAGS: size = 12; raw[8] = uint8_t(h->flags); raw[9] = uint8_t(h->flags >> 8); if (h->station && !h->desktop) raw[8] = uint8_t(h->station->flags); break;
    case UOI_NAME: {
      const WideString& n = h->desktop ? h->desktop->name : h->station->name;
      for (char16_t c : n.view()) text.push_back(c);
      text.push_back(0);
      size = uint32_t(text.count() * 2);
      break;
    }
    case UOI_TYPE: {
      const char* t = h->desktop ? "Desktop" : "WindowStation";
      for (; *t; ++t) text.push_back(char16_t(*t));
      text.push_back(0);
      size = uint32_t(text.count() * 2);
      break;
    }
    case UOI_USER_SID: return call.fail(kErrorNotSupported);
    default: return call.fail(kErrorInvalidParameter);
  }
  if (call.arg(4) != 0) call.write_u32(call.arg(4), size);
  if (len < size) return call.fail(kErrorInsufficientBuffer);
  if (text.count()) return call.write_wide(call.arg(2), text.view(), uint32_t(text.count()), false) >= 0 ? 1 : call.fail(kErrorNoaccess);
  return call.write(call.arg(2), ConstByteView(raw, size)) ? 1 : call.fail(kErrorNoaccess);
}
uint64_t nt_set_object_information(Call& call) {
  State& s = S();
  ObjHandle* h = s.object_handles.find(call.arg32(0));
  if (!h || h->pid != call.pid()) return call.fail(kErrorInvalidHandle);
  if (call.arg32(1) != UOI_FLAGS || call.arg32(3) < 12) return call.fail(kErrorInvalidParameter);
  uint32_t flags = 0;
  if (!call.read_u32(call.arg(2) + 8, &flags)) return call.fail(kErrorNoaccess);
  h->flags = flags;
  return 1;
}

}  // namespace

void register_station_services() {
  register_handler(WindowsSyscallNumber::NtUserCreateWindowStation, nt_create_window_station);
  register_handler(WindowsSyscallNumber::NtUserOpenWindowStation, nt_open_window_station);
  register_handler(WindowsSyscallNumber::NtUserCloseWindowStation, nt_close_window_station);
  register_handler(WindowsSyscallNumber::NtUserGetProcessWindowStation, nt_get_process_window_station);
  register_handler(WindowsSyscallNumber::NtUserSetProcessWindowStation, nt_set_process_window_station);
  register_handler(WindowsSyscallNumber::NtUserCreateDesktopEx, nt_create_desktop_ex);
  register_handler(WindowsSyscallNumber::NtUserOpenDesktop, nt_open_desktop);
  register_handler(WindowsSyscallNumber::NtUserOpenInputDesktop, nt_open_input_desktop);
  register_handler(WindowsSyscallNumber::NtUserCloseDesktop, nt_close_desktop);
  register_handler(WindowsSyscallNumber::NtUserSwitchDesktop, nt_switch_desktop);
  register_handler(WindowsSyscallNumber::NtUserGetThreadDesktop, nt_get_thread_desktop);
  register_handler(WindowsSyscallNumber::NtUserSetThreadDesktop, nt_set_thread_desktop);
  register_handler(WindowsSyscallNumber::NtUserBuildNameList, nt_build_name_list);
  register_handler(WindowsSyscallNumber::NtUserGetObjectInformation, nt_get_object_information);
  register_handler(WindowsSyscallNumber::NtUserSetObjectInformation, nt_set_object_information);
}

}  // namespace process::win32k::user
