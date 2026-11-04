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
// Window classes and atoms: RegisterClassEx, UnregisterClass, GetClassInfo, class longs, RegisterWindowMessage, GetAtomName.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

// WNDCLASSEXW at x64: cbSize 0, style 4, proc 8, cbClsExtra 16, cbWndExtra 20, hInstance 24, hIcon 32, hCursor 40, hbrBackground 48, lpszMenuName 56, lpszClassName 64, hIconSm 72.
constexpr uint32_t kWndClassSize = 80;

bool is_decimal(ArrayView<const char16_t> s, uint32_t from, uint32_t* value) {
  if (from >= s.size()) return false;
  uint32_t v = 0;
  for (uint64_t i = from; i < s.size(); ++i) {
    if (s[i] < u'0' || s[i] > u'9' || v > 6553) return false;
    v = v * 10 + uint32_t(s[i] - u'0');
  }
  if (v == 0 || v > 0xFFFF) return false;
  *value = v;
  return true;
}

// "#32769" names the predefined class with that atom.
uint32_t atom_from_name(ArrayView<const char16_t> name) {
  uint32_t v = 0;
  if (name.size() > 1 && name[0] == u'#' && is_decimal(name, 1, &v)) return v;
  return 0;
}

}  // namespace

// A class name argument (UNICODE_STRING*): either a text or, with a zero length and a small "pointer", an atom.
bool read_class_name(Call& call, uint64_t address, WideString* out, uint32_t* atom) {
  *atom = 0;
  uint16_t length = 0;
  uint64_t buffer = 0;
  if (!call.read_u16(address, &length) || !call.read_u64(address + 8, &buffer)) return false;
  if (length == 0 && buffer != 0 && buffer < 0x10000) { *atom = uint32_t(buffer); return true; }
  if (length == 0) return false;
  if (!call.read_wide(buffer, length / 2, out)) return false;
  *atom = atom_from_name(out->view());
  return true;
}

Class* find_class_for_process(uint64_t pid, uint32_t atom, uint64_t instance, bool check_instance) {
  for (Class& c : state().classes) {
    if (c.dead || c.atom != atom) continue;
    if (!(c.pid == pid || c.is_system || (c.style & wn::CS_GLOBALCLASS))) continue;
    if (check_instance && !c.is_system && !(c.style & wn::CS_GLOBALCLASS) && (c.instance & ~uint64_t{0xFFFF}) != (instance & ~uint64_t{0xFFFF})) continue;      // (a local class is found by its module; the low 16 bits of the handle do not count)
    return &c;
  }
  return nullptr;
}

Class* find_class(Call& call, uint32_t atom, ArrayView<const char16_t> name, uint64_t instance, bool check_instance) {
  if (atom == 0) atom = atom_find(name);
  if (atom == 0) return nullptr;
  // the process's own class first (a local class hides a global one of the same name)
  for (Class& c : state().classes) {
    if (c.dead || c.atom != atom || c.pid != call.pid()) continue;
    if (check_instance && instance != 0 && c.instance != 0 && c.instance != instance) continue;
    return &c;
  }
  return find_class_for_process(call.pid(), atom, instance, check_instance);
}

void class_window_gone(Class* cls) {
  if (cls->windows) --cls->windows;
  if (cls->windows == 0 && cls->dead) state().classes.erase(cls);
}

void drop_process_classes(uint64_t pid) {
  State& s = state();
  for (;;) {
    Class* c = nullptr;
    for (Class& x : s.classes) if (x.pid == pid && !x.is_system) { c = &x; break; }
    if (!c) break;
    s.classes.erase(c);
  }
}

void create_system_classes() {
  State& s = state();
  if (s.desktop_class) return;
  auto make = [&](uint32_t atom, const char* name) -> Class* {
    Class* c = s.classes.emplace(s.alloc);
    if (!c) return nullptr;
    c->atom = atom;
    c->name.append_ascii(name);
    c->is_system = true;
    c->style = wn::CS_GLOBALCLASS;
    c->wndproc = 0;
    return c;
  };
  s.desktop_class = make(wn::ATOM_DESKTOP, "#32769");
  if (s.desktop_class) s.desktop_class->style |= 0x8 /* CS_DBLCLKS */;
  const uint32_t message_atom = atom_add(ArrayView<const char16_t>(u"Message", 7));
  s.message_class = make(message_atom, "Message");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Class values (GetClassLong / SetClassLong and friends)

bool class_get_value(Call& call, Window* w, int32_t offset, uint32_t size, uint64_t* out) {
  Class* c = w->cls;
  if (!c) { call.set_last_error(kErrorInvalidIndex); return false; }
  if (offset >= 0) {
    if (uint64_t{uint32_t(offset)} + size > uint64_t(c->cb_class_extra)) { call.set_last_error(kErrorInvalidIndex); return false; }
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; ++i) v |= uint64_t{c->extra[uint64_t(offset) + i]} << (8 * i);
    *out = v;
    return true;
  }
  uint64_t v;
  switch (offset) {
    case wn::GCL_MENUNAME: v = 0; break;
    case wn::GCL_HBRBACKGROUND: v = c->brush; break;
    case wn::GCL_HCURSOR: v = c->cursor; break;
    case wn::GCL_HICON: v = c->icon; break;
    case wn::GCL_HMODULE: v = c->instance; break;
    case wn::GCL_CBWNDEXTRA: v = uint64_t(uint32_t(c->cb_wnd_extra)); break;
    case wn::GCL_CBCLSEXTRA: v = uint64_t(uint32_t(c->cb_class_extra)); break;
    case wn::GCL_WNDPROC: v = c->wndproc; break;
    case wn::GCL_STYLE: v = c->style; break;
    case wn::GCW_ATOM: v = c->atom; break;
    case wn::GCL_HICONSM: v = c->icon_small; break;
    default: call.set_last_error(kErrorInvalidIndex); return false;
  }
  if (size == 4) v &= 0xFFFFFFFFu;
  if (size == 2) v &= 0xFFFFu;
  *out = v;
  return true;
}

bool class_set_value(Call& call, Window* w, int32_t offset, uint32_t size, uint64_t value, uint64_t* old) {
  Class* c = w->cls;
  if (!c) { call.set_last_error(kErrorInvalidIndex); return false; }
  if (w->pid != call.pid() && offset != wn::GCL_STYLE) {      // only the owner process may change a class
    // (the other processes' classes are not writable except through the same-process rule of Windows)
    call.set_last_error(kErrorAccessDenied);
    return false;
  }
  if (offset >= 0) {
    if (uint64_t{uint32_t(offset)} + size > uint64_t(c->cb_class_extra)) { call.set_last_error(kErrorInvalidIndex); return false; }
    uint64_t prev = 0;
    for (uint32_t i = 0; i < size; ++i) {
      prev |= uint64_t{c->extra[uint64_t(offset) + i]} << (8 * i);
      c->extra[uint64_t(offset) + i] = uint8_t(value >> (8 * i));
    }
    *old = prev;
    return true;
  }
  uint64_t prev;
  switch (offset) {
    case wn::GCL_MENUNAME: prev = 0; break;
    case wn::GCL_HBRBACKGROUND: prev = c->brush; c->brush = value; break;
    case wn::GCL_HCURSOR: prev = c->cursor; c->cursor = value; break;
    case wn::GCL_HICON: prev = c->icon; c->icon = value; break;
    case wn::GCL_HMODULE: prev = c->instance; c->instance = value; break;
    case wn::GCL_CBWNDEXTRA: prev = uint64_t(uint32_t(c->cb_wnd_extra)); c->cb_wnd_extra = int32_t(value); break;
    case wn::GCL_CBCLSEXTRA: prev = uint64_t(uint32_t(c->cb_class_extra)); break;      // the extra bytes of existing windows cannot change
    case wn::GCL_WNDPROC: prev = c->wndproc; c->wndproc = value; break;
    case wn::GCL_STYLE: prev = c->style; c->style = uint32_t(value); break;
    case wn::GCW_ATOM: call.set_last_error(kErrorInvalidIndex); return false;
    case wn::GCL_HICONSM: prev = c->icon_small; c->icon_small = value; break;
    default: call.set_last_error(kErrorInvalidIndex); return false;
  }
  if (size == 4) prev &= 0xFFFFFFFFu;
  if (size == 2) prev &= 0xFFFFu;
  *old = prev;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Services

namespace {

// ATOM NtUserRegisterClassExWOW(const WNDCLASSEXW* wc, UNICODE_STRING* name, UNICODE_STRING* version, client_menu_name* menu_name, DWORD fnid, DWORD flags, DWORD* wow)
uint64_t register_class(Call& call) {
  State& s = state();
  uint8_t raw[kWndClassSize];
  if (!call.read(call.arg(0), ByteView(raw, kWndClassSize))) return call.fail(kErrorInvalidParameter);
  auto u32 = [&](uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
  auto u64 = [&](uint32_t o) { return uint64_t(u32(o)) | uint64_t(u32(o + 4)) << 32; };
  if (u32(0) != kWndClassSize) return call.fail(kErrorInvalidParameter);
  WideString name(s.alloc);
  uint32_t atom_from_arg = 0;
  if (!read_class_name(call, call.arg(1), &name, &atom_from_arg)) return call.fail(kErrorInvalidParameter);
  const int32_t cb_cls = int32_t(u32(16)), cb_wnd = int32_t(u32(20));
  if (u64(8) == 0 || cb_cls < 0 || cb_wnd < 0 || cb_cls > 0x7FFF || cb_wnd > 0x7FFF) return call.fail(kErrorInvalidParameter);
  uint32_t atom = atom_from_arg;
  if (atom == 0 || atom >= 0xC000) {
    if (atom == 0) atom = atom_add(name.view());
    if (atom == 0) return call.fail(kErrorInvalidParameter);
  }
  const uint64_t instance = u64(24);
  const uint32_t style = u32(4);
  for (Class& c : s.classes) {
    if (c.dead || c.atom != atom) continue;
    if (c.is_system || (c.pid == call.pid() && (c.instance == instance || (c.style & wn::CS_GLOBALCLASS) || (style & wn::CS_GLOBALCLASS)))) return call.fail(kErrorClassAlreadyExists);
  }
  Class* c = s.classes.emplace(s.alloc);
  if (!c) return call.fail(kErrorNotEnoughMemory);
  c->atom = atom;
  c->name.append(name.view());
  c->pid = call.pid();
  c->instance = instance;
  c->style = style;
  c->wndproc = u64(8);
  c->unicode = (call.arg32(5) & 1) == 0;
  c->cb_class_extra = cb_cls;
  c->cb_wnd_extra = cb_wnd;
  c->icon = u64(32);
  c->cursor = u64(40);
  c->brush = u64(48);
  c->icon_small = u64(72);
  c->fnid = call.arg32(4);
  c->menu_name_ptr = u64(56);
  for (int32_t i = 0; i < cb_cls; ++i) c->extra.push_back(0);
  return atom;
}

// BOOL NtUserUnregisterClass(UNICODE_STRING* name, HINSTANCE instance, client_menu_name** menu_name)
uint64_t unregister_class(Call& call) {
  WideString name(state().alloc);
  uint32_t atom = 0;
  if (!read_class_name(call, call.arg(0), &name, &atom)) return call.fail(kErrorClassDoesNotExist);
  Class* c = find_class(call, atom, name.view(), call.arg(1), true);
  if (!c || c->pid != call.pid() || c->is_system) return call.fail(kErrorClassDoesNotExist);
  if (c->windows != 0) return call.fail(kErrorClassHasWindows);
  if (call.arg(2) != 0) call.write_u64(call.arg(2), c->menu_name_ptr);
  state().classes.erase(c);
  return 1;
}

// ATOM NtUserGetClassInfoEx(HINSTANCE instance, UNICODE_STRING* name, WNDCLASSEXW* wc, client_menu_name** menu_name, BOOL ansi)
uint64_t get_class_info(Call& call) {
  WideString name(state().alloc);
  uint32_t atom = 0;
  if (!read_class_name(call, call.arg(1), &name, &atom)) return call.fail(kErrorClassDoesNotExist);
  Class* c = find_class(call, atom, name.view(), call.arg(0), true);
  if (!c) return call.fail(kErrorClassDoesNotExist);
  const uint64_t out = call.arg(2);
  if (out != 0) {
    uint8_t raw[kWndClassSize] = {};
    auto put32 = [&](uint32_t o, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) raw[o + i] = uint8_t(v >> (8 * i)); };
    auto put64 = [&](uint32_t o, uint64_t v) { put32(o, uint32_t(v)); put32(o + 4, uint32_t(v >> 32)); };
    uint32_t size = 0;
    call.read_u32(out, &size);
    put32(0, kWndClassSize);
    put32(4, c->style & ~wn::CS_GLOBALCLASS);
    put64(8, c->wndproc);
    put32(16, uint32_t(c->cb_class_extra));
    put32(20, uint32_t(c->cb_wnd_extra));
    put64(24, c->instance);
    put64(32, c->icon);
    put64(40, c->cursor);
    put64(48, c->brush);
    put64(56, 0);
    put64(64, 0);
    put64(72, c->icon_small);
    // lpszMenuName and lpszClassName belong to the caller: it knows the strings
    uint8_t keep[16];
    if (!call.read(out + 56, ByteView(keep, 16))) return call.fail(kErrorInvalidParameter);
    for (uint32_t i = 0; i < 16; ++i) raw[56 + i] = keep[i];
    if (!call.write(out, ConstByteView(raw, kWndClassSize))) return call.fail(kErrorInvalidParameter);
  }
  if (call.arg(3) != 0) call.write_u64(call.arg(3), c->menu_name_ptr);
  return c->atom;
}

// INT NtUserGetClassName(HWND hwnd, BOOL real, UNICODE_STRING* name)
uint64_t get_class_name(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w || !w->cls) return 0;
  uint16_t max_bytes = 0;
  uint64_t buffer = 0;
  if (!call.read_u16(call.arg(2) + 2, &max_bytes) || !call.read_u64(call.arg(2) + 8, &buffer)) return call.fail(kErrorInvalidParameter);
  const WideString& n = w->cls->name;
  const uint32_t capacity = max_bytes / 2;
  if (capacity == 0) return call.fail(kErrorInsufficientBuffer);
  const int32_t written = write_wide_z(call, buffer, n.view(), capacity);
  if (written < 0) return call.fail(kErrorInvalidParameter);
  call.write_u16(call.arg(2), uint16_t(written * 2));
  return uint64_t(written);
}

uint64_t set_class_long_impl(Call& call, uint32_t size, bool word) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  uint64_t old = 0;
  uint64_t value = call.arg(2);
  if (size == 4) value &= 0xFFFFFFFFu;
  if (word) value &= 0xFFFFu;
  if (!class_set_value(call, w, call.argi(1), size, value, &old)) return 0;
  return old;
}

uint64_t nt_set_class_long(Call& call) { return set_class_long_impl(call, 4, false); }
uint64_t nt_set_class_long_ptr(Call& call) { return set_class_long_impl(call, 8, false); }
uint64_t nt_set_class_word(Call& call) { return set_class_long_impl(call, 2, true); }

// ATOM NtUserRegisterWindowMessage(UNICODE_STRING* name)
uint64_t register_window_message(Call& call) {
  WideString name(state().alloc);
  uint16_t length = 0;
  uint64_t buffer = 0;
  if (!call.read_u16(call.arg(0), &length) || !call.read_u64(call.arg(0) + 8, &buffer) || length == 0 || (length & 1) || length > 510) return call.fail(kErrorInvalidParameter);
  if (!call.read_wide(buffer, length / 2, &name)) return call.fail(kErrorInvalidParameter);
  const uint32_t atom = atom_add(name.view());
  return atom ? atom : call.fail(kErrorNotEnoughMemory);
}

// ULONG NtUserGetAtomName(ATOM atom, UNICODE_STRING* name): the name goes to name->Buffer (MaximumLength bytes); answers the number of characters.
uint64_t get_atom_name(Call& call) {
  const uint32_t id = call.arg32(0) & 0xFFFF;
  WideString predefined(state().alloc);
  const WideString* name = nullptr;
  if (const Atom* a = atom_by_id(id)) name = &a->name;
  else {
    for (Class& c : state().classes) if (c.atom == id) { name = &c.name; break; }
  }
  if (!name) return call.fail(kErrorInvalidHandle);
  uint16_t max_bytes = 0;
  uint64_t buffer = 0;
  if (!call.read_u16(call.arg(1) + 2, &max_bytes) || !call.read_u64(call.arg(1) + 8, &buffer)) return call.fail(kErrorInvalidParameter);
  if (max_bytes < 2) return call.fail(kErrorInsufficientBuffer);
  const int32_t written = write_wide_z(call, buffer, name->view(), max_bytes / 2u);
  if (written < 0) return call.fail(kErrorInvalidParameter);
  call.write_u16(call.arg(1), uint16_t(written * 2));
  return uint64_t(written);
}

// NTSTATUS NtUserInitializeClientPfnArrays(const ntuser_client_func_ptr* A, *W, *workers, HINSTANCE user_module)
uint64_t initialize_client_pfn_arrays(Call& call) {
  ProcessInfo& p = process_info(call.pid());
  const uint32_t counts[3] = {17, 17, 11};
  for (uint32_t k = 0; k < 3; ++k) {
    const uint64_t address = call.arg(k);
    if (address == 0) continue;
    uint64_t values[17] = {};
    if (!call.read_array(address, values, counts[k])) return kStatusAccessViolation;
    for (uint32_t i = 0; i < counts[k]; ++i) p.client_procs[k][i] = values[i];
  }
  p.pfn_set = true;
  p.user_module = call.arg(3);
  return kStatusSuccess;
}

}  // namespace

void register_class_services() {
  register_handler(WindowsSyscallNumber::NtUserRegisterClassExWOW, register_class);
  register_handler(WindowsSyscallNumber::NtUserUnregisterClass, unregister_class);
  register_handler(WindowsSyscallNumber::NtUserGetClassInfoEx, get_class_info);
  register_handler(WindowsSyscallNumber::NtUserGetClassName, get_class_name);
  register_handler(WindowsSyscallNumber::NtUserSetClassLong, nt_set_class_long);
  register_handler(WindowsSyscallNumber::NtUserSetClassLongPtr, nt_set_class_long_ptr);
  register_handler(WindowsSyscallNumber::NtUserSetClassWord, nt_set_class_word);
  register_handler(WindowsSyscallNumber::NtUserRegisterWindowMessage, register_window_message);
  register_handler(WindowsSyscallNumber::NtUserGetAtomName, get_atom_name);
  register_handler(WindowsSyscallNumber::NtUserInitializeClientPfnArrays, initialize_client_pfn_arrays);
}

}  // namespace process::win32k::user
