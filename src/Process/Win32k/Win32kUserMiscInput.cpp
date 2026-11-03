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

// Raw input (device registration, the device list and its properties, the records WM_INPUT hands out) and the small pointer/touch services whose answer follows from the machine
// having a mouse and a keyboard but no touch or pen device.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kRawInputDeviceSize = 16, kRawInputDeviceListSize = 16, kRawHeaderSize = 24, kDeviceInfoSize = 32;
constexpr uint32_t kRidDevicePreparsedData = 0x20000005, kRidDeviceName = 0x20000007, kRidDeviceInfo = 0x2000000B, kRidInput = 0x10000003, kRidHeader = 0x10000005;
constexpr uint32_t kRidevRemove = 1, kRidevExclude = 0x10, kRidevPageOnly = 0x20, kRidevInputSink = 0x100, kRidevValid = 0x1 | 0x10 | 0x20 | 0x100 | 0x200 | 0x400 | 0x1000 | 0x2000;
constexpr uint32_t kMouseDevice = 0x10001, kKeyboardDevice = 0x10002, kMaxRegistrations = 1024, kMaxRecords = 256;
constexpr uint32_t kErrorInsufficientBufferValue = 122;

struct RawDevice { uint64_t pid; uint16_t usage_page, usage; uint32_t flags, hwnd; };

struct RawRecord {
  uint64_t pid;
  uint32_t type, device;
  uint64_t wparam;
  bool buffered;
  ByteBuffer data;
  RawRecord(allocators::IAllocator& a, uint64_t owner, uint32_t kind, uint32_t dev, uint64_t w, bool buffer) : pid(owner), type(kind), device(dev), wparam(w), buffered(buffer), data(a) {}
};

struct InputState {
  List<RawDevice> devices;
  Registry<RawRecord> records;
  List<uint32_t> order;      // the handles of the records, oldest first
  explicit InputState(allocators::IAllocator& a) : devices(a), records(a, 0x0005), order(a) {}
};
UniquePointer<InputState> g_input;
InputState* st() { return lazy_state(g_input); }

uint64_t fail_minus_one(Call& c, uint32_t error) {
  c.set_last_error(error);
  return 0xFFFFFFFFu;
}

uint16_t le16(const uint8_t* raw, uint32_t o) { return uint16_t(raw[o] | raw[o + 1] << 8); }
uint32_t le32(const uint8_t* raw, uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; }
void put32(uint8_t* raw, uint32_t o, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) raw[o + i] = uint8_t(v >> (8 * i)); }

uint64_t svc_register_raw_input_devices(Call& c) {
  InputState* s = st();
  const uint32_t count = c.arg32(1), size = c.arg32(2);
  if (!s) return 0;
  if (size != kRawInputDeviceSize || count == 0 || count > kMaxRegistrations || c.arg(0) == 0) return c.fail(kErrorInvalidParameter);
  List<RawDevice> incoming(*c.allocator());
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t raw[kRawInputDeviceSize];
    if (!c.read(c.arg(0) + uint64_t{i} * kRawInputDeviceSize, ByteView(raw, kRawInputDeviceSize))) return c.fail(kErrorNoAccess);
    RawDevice d{c.pid(), le16(raw, 0), le16(raw, 2), le32(raw, 4), uint32_t(le32(raw, 8))};
    const uint64_t hwnd64 = uint64_t(le32(raw, 8)) | uint64_t(le32(raw, 12)) << 32;
    if ((d.flags & ~kRidevValid) != 0 || d.usage_page == 0 || (hwnd64 >> 32) != 0) return c.fail(kErrorInvalidParameter);
    if ((d.flags & kRidevRemove) && d.hwnd != 0) return c.fail(kErrorInvalidParameter);
    if ((d.flags & kRidevExclude) && !(d.flags & kRidevPageOnly) && d.usage == 0) return c.fail(kErrorInvalidParameter);
    if ((d.flags & kRidevInputSink) && d.hwnd == 0) return c.fail(kErrorInvalidParameter);
    if (d.hwnd != 0 && (!user::is_window(d.hwnd) || user::window_process(d.hwnd) != c.pid())) return c.fail(kErrorInvalidParameter);
    if (!incoming.push_back(d)) return c.fail(kErrorNotEnoughMemory);
  }
  for (const RawDevice& d : incoming.view()) {
    for (uint64_t i = 0; i < s->devices.count(); ++i) {
      const RawDevice& e = s->devices[i];
      if (e.pid == d.pid && e.usage_page == d.usage_page && e.usage == d.usage) { s->devices.remove_at(i); break; }
    }
    if (!(d.flags & kRidevRemove)) s->devices.push_back(d);
  }
  return 1;
}

uint64_t svc_get_registered_raw_input_devices(Call& c) {
  InputState* s = st();
  if (!s) return 0;
  if (c.arg(1) == 0 || c.arg32(2) != kRawInputDeviceSize) return fail_minus_one(c, kErrorInvalidParameter);
  List<RawDevice> mine(*c.allocator());
  for (const RawDevice& d : s->devices.view()) if (d.pid == c.pid()) mine.push_back(d);
  uint32_t capacity = 0;
  if (!c.read_u32(c.arg(1), &capacity)) return fail_minus_one(c, kErrorNoAccess);
  if (c.arg(0) == 0) { c.write_u32(c.arg(1), uint32_t(mine.count())); return 0; }
  if (capacity < mine.count()) { c.write_u32(c.arg(1), uint32_t(mine.count())); return fail_minus_one(c, kErrorInsufficientBufferValue); }
  for (uint64_t i = 0; i < mine.count(); ++i) {
    uint8_t raw[kRawInputDeviceSize] = {};
    raw[0] = uint8_t(mine[i].usage_page); raw[1] = uint8_t(mine[i].usage_page >> 8);
    raw[2] = uint8_t(mine[i].usage); raw[3] = uint8_t(mine[i].usage >> 8);
    put32(raw, 4, mine[i].flags);
    put32(raw, 8, mine[i].hwnd);
    if (!c.write(c.arg(0) + i * kRawInputDeviceSize, ConstByteView(raw, kRawInputDeviceSize))) return fail_minus_one(c, kErrorNoAccess);
  }
  return mine.count();
}

uint64_t svc_get_raw_input_device_list(Call& c) {
  if (c.arg(1) == 0 || c.arg32(2) != kRawInputDeviceListSize) return fail_minus_one(c, kErrorInvalidParameter);
  uint32_t capacity = 0;
  if (!c.read_u32(c.arg(1), &capacity)) return fail_minus_one(c, kErrorNoAccess);
  if (c.arg(0) == 0) { c.write_u32(c.arg(1), 2); return 0; }
  if (capacity < 2) { c.write_u32(c.arg(1), 2); return fail_minus_one(c, kErrorInsufficientBufferValue); }
  const uint64_t list[4] = {kMouseDevice, 0 /* RIM_TYPEMOUSE */, kKeyboardDevice, 1 /* RIM_TYPEKEYBOARD */};
  for (uint32_t i = 0; i < 2; ++i) {
    if (!c.write_u64(c.arg(0) + i * 16, list[2 * i]) || !c.write_u32(c.arg(0) + i * 16 + 8, uint32_t(list[2 * i + 1]))) return fail_minus_one(c, kErrorNoAccess);
  }
  return 2;
}

uint64_t svc_get_raw_input_device_info(Call& c) {
  const uint64_t handle = c.arg(0);
  const uint32_t command = c.arg32(1);
  if (handle != kMouseDevice && handle != kKeyboardDevice) return fail_minus_one(c, kErrorInvalidHandle);
  if (c.arg(3) == 0) return fail_minus_one(c, kErrorInvalidParameter);
  uint32_t capacity = 0;
  if (!c.read_u32(c.arg(3), &capacity)) return fail_minus_one(c, kErrorNoAccess);
  const bool mouse = handle == kMouseDevice;
  if (command == kRidDeviceName) {
    WideString name(*c.allocator());
    name.append_ascii(mouse ? "\\\\?\\HID#VID_0000&PID_0001#0&0&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}" : "\\\\?\\HID#VID_0000&PID_0002#0&0&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}");
    const uint32_t need = uint32_t(name.size()) + 1;
    if (c.arg(2) == 0) { c.write_u32(c.arg(3), need); return 0; }
    if (capacity < need) { c.write_u32(c.arg(3), need); return fail_minus_one(c, kErrorInsufficientBufferValue); }
    if (c.write_wide(c.arg(2), name.view(), capacity) < 0) return fail_minus_one(c, kErrorNoAccess);
    return need;
  }
  if (command == kRidDeviceInfo) {
    if (c.arg(2) == 0) { c.write_u32(c.arg(3), kDeviceInfoSize); return 0; }
    if (capacity < kDeviceInfoSize) { c.write_u32(c.arg(3), kDeviceInfoSize); return fail_minus_one(c, kErrorInsufficientBufferValue); }
    uint8_t info[kDeviceInfoSize] = {};
    put32(info, 0, kDeviceInfoSize);
    put32(info, 4, mouse ? 0 : 1);
    if (mouse) { put32(info, 8, 0); put32(info, 12, 5); put32(info, 16, 60); put32(info, 20, 1); }      // id, buttons, sample rate, horizontal wheel
    else { put32(info, 8, 4); put32(info, 12, 0); put32(info, 16, 1); put32(info, 20, 12); put32(info, 24, 3); put32(info, 28, 101); }
    if (!c.write(c.arg(2), ConstByteView(info, kDeviceInfoSize))) return fail_minus_one(c, kErrorNoAccess);
    return kDeviceInfoSize;
  }
  (void)kRidDevicePreparsedData;      // (only HID devices have preparsed data: the mouse and the keyboard do not)
  return fail_minus_one(c, kErrorInvalidParameter);
}

uint64_t svc_get_raw_input_data(Call& c) {
  InputState* s = st();
  const uint32_t command = c.arg32(1);
  if (!s) return 0;
  if (c.arg32(4) != kRawHeaderSize || c.arg(3) == 0) return fail_minus_one(c, kErrorInvalidParameter);
  RawRecord* r = (c.arg(0) >> 32) == 0 ? s->records.find(c.arg32(0)) : nullptr;
  if (!r || r->pid != c.pid()) return fail_minus_one(c, kErrorInvalidHandle);
  if (command != kRidHeader && command != kRidInput) return fail_minus_one(c, kErrorInvalidParameter);
  const uint32_t need = command == kRidHeader ? kRawHeaderSize : uint32_t(kRawHeaderSize + r->data.size());
  uint32_t capacity = 0;
  if (!c.read_u32(c.arg(3), &capacity)) return fail_minus_one(c, kErrorNoAccess);
  if (c.arg(2) == 0) { c.write_u32(c.arg(3), need); return 0; }
  if (capacity < need) { c.write_u32(c.arg(3), need); return fail_minus_one(c, kErrorInsufficientBufferValue); }
  uint8_t head[kRawHeaderSize] = {};
  put32(head, 0, r->type);
  put32(head, 4, uint32_t(kRawHeaderSize + r->data.size()));
  put32(head, 8, r->device);
  for (uint32_t i = 0; i < 8; ++i) head[16 + i] = uint8_t(r->wparam >> (8 * i));
  if (!c.write(c.arg(2), ConstByteView(head, kRawHeaderSize))) return fail_minus_one(c, kErrorNoAccess);
  if (command == kRidInput && r->data.size() != 0 && !c.write(c.arg(2) + kRawHeaderSize, r->data.view())) return fail_minus_one(c, kErrorNoAccess);
  return need;
}

void drop_record(InputState& s, uint32_t handle) {
  s.records.destroy(handle);
  for (uint64_t i = 0; i < s.order.count(); ++i) if (s.order[i] == handle) { s.order.remove_at(i); break; }
}

uint64_t svc_get_raw_input_buffer(Call& c) {
  InputState* s = st();
  if (!s) return 0;
  if (c.arg32(2) != kRawHeaderSize || c.arg(1) == 0) return fail_minus_one(c, kErrorInvalidParameter);
  uint32_t capacity = 0;
  if (!c.read_u32(c.arg(1), &capacity)) return fail_minus_one(c, kErrorNoAccess);
  uint32_t largest = 0;
  for (uint32_t h : s->order.view()) {
    RawRecord* r = s->records.find(h);
    if (r && r->pid == c.pid() && r->buffered) { const uint32_t n = uint32_t(kRawHeaderSize + r->data.size()); if (n > largest) largest = n; }
  }
  if (c.arg(0) == 0) { c.write_u32(c.arg(1), largest); return 0; }
  uint64_t offset = 0;
  uint32_t delivered = 0;
  List<uint32_t> taken(*c.allocator());
  for (uint32_t h : s->order.view()) {
    RawRecord* r = s->records.find(h);
    if (!r || r->pid != c.pid() || !r->buffered) continue;
    const uint64_t n = kRawHeaderSize + r->data.size();
    if (offset + n > capacity) {
      if (delivered == 0) return fail_minus_one(c, kErrorInsufficientBufferValue);
      break;
    }
    uint8_t head[kRawHeaderSize] = {};
    put32(head, 0, r->type);
    put32(head, 4, uint32_t(n));
    put32(head, 8, r->device);
    for (uint32_t i = 0; i < 8; ++i) head[16 + i] = uint8_t(r->wparam >> (8 * i));
    if (!c.write(c.arg(0) + offset, ConstByteView(head, kRawHeaderSize)) || (r->data.size() != 0 && !c.write(c.arg(0) + offset + kRawHeaderSize, r->data.view()))) return fail_minus_one(c, kErrorNoAccess);
    offset += (n + 7) & ~uint64_t{7};      // NEXTRAWINPUTBLOCK: the next structure starts at the next 8-byte boundary
    ++delivered;
    taken.push_back(h);
  }
  for (uint32_t h : taken.view()) drop_record(*s, h);
  return delivered;
}

uint64_t svc_enable_mouse_in_pointer(Call& c) {
  ProcessRecord* p = process_record(c.pid());
  if (!p) return 0;
  p->mouse_in_pointer = c.arg_bool(0);
  return 1;
}

uint64_t svc_is_mouse_in_pointer_enabled(Call& c) {
  ProcessRecord* p = find_process_record(c.pid());
  ThreadRecord* t = find_thread_record(c.tid());
  return (p && p->mouse_in_pointer) || (t && t->mouse_in_pointer) ? 1 : 0;
}

uint64_t svc_enable_mouse_in_pointer_for_thread(Call& c) {
  ThreadRecord* t = thread_record(c.tid(), c.pid());
  if (!t) return 0;
  t->mouse_in_pointer = true;
  return 1;
}

uint64_t svc_initialize_touch_injection(Call& c) {
  const uint32_t max = c.arg32(0), mode = c.arg32(1);
  if (max == 0 || max > 256 || mode < 1 || mode > 3) return c.fail(kErrorInvalidParameter);
  ProcessRecord* p = process_record(c.pid());
  if (!p) return 0;
  p->touch_injection = true;
  p->touch_max_contacts = max;
  return 1;
}

uint64_t svc_get_pointer_devices(Call& c) {
  if (c.arg(0) == 0) return c.fail(kErrorInvalidParameter);
  return c.write_u32(c.arg(0), 0) ? 1 : c.fail(kErrorNoAccess);      // no touch or pen devices: the mouse is not a pointer device unless it is enabled as one
}

}  // namespace

uint32_t record_raw_input(uint64_t pid, uint32_t type, uint32_t device, uint64_t wparam, ConstByteView data, bool buffered) {
  InputState* s = st();
  if (!s) return 0;
  while (s->order.count() >= kMaxRecords) drop_record(*s, s->order[0]);
  uint32_t handle = 0;
  RawRecord* r = s->records.create(&handle, pid, type, device, wparam, buffered);
  if (!r) return 0;
  if (!r->data.append(data) || !s->order.push_back(handle)) { s->records.destroy(handle); return 0; }
  return handle;
}

void input_process_exited(uint64_t pid) {
  InputState* s = g_input.get();
  if (!s) return;
  for (uint64_t i = 0; i < s->devices.count();) { if (s->devices[i].pid == pid) s->devices.remove_at(i); else ++i; }
  List<uint32_t> mine(*system_allocator());
  for (uint32_t h : s->order.view()) if (RawRecord* r = s->records.find(h)) if (r->pid == pid) mine.push_back(h);
  for (uint32_t h : mine.view()) drop_record(*s, h);
}

void install_input() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserRegisterRawInputDevices, svc_register_raw_input_devices);
  register_handler(N::NtUserGetRegisteredRawInputDevices, svc_get_registered_raw_input_devices);
  register_handler(N::NtUserGetRawInputDeviceList, svc_get_raw_input_device_list);
  register_handler(N::NtUserGetRawInputDeviceInfo, svc_get_raw_input_device_info);
  register_handler(N::NtUserGetRawInputData, svc_get_raw_input_data);
  register_handler(N::NtUserGetRawInputBuffer, svc_get_raw_input_buffer);
  register_handler(N::NtUserEnableMouseInPointer, svc_enable_mouse_in_pointer);
  register_handler(N::NtUserIsMouseInPointerEnabled, svc_is_mouse_in_pointer_enabled);
  register_handler(N::NtUserEnableMouseInPointerForThread, svc_enable_mouse_in_pointer_for_thread);
  register_handler(N::NtUserInitializeTouchInjection, svc_initialize_touch_injection);
  register_handler(N::NtUserGetPointerDevices, svc_get_pointer_devices);
}

}  // namespace process::win32k::user_misc
