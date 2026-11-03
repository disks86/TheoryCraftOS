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

// NtRIM*: the raw input manager. The machine has two virtual devices (the keyboard and the mouse); processes register for their input or observe it, and read packets of
// RimPacket (docs/win32k/Composition.md).
namespace process::win32k::composition {

namespace {

constexpr uint32_t kMaxRegistrationsPerProcess = 64, kMaxObserversPerProcess = 64, kDeviceMaskAll = 3, kFlagsMask = 0xFF;
constexpr uint32_t kPacketOverflow = 1;      // RimPacket::flags of the first packet after packets were dropped

struct DeviceProperties { uint32_t size, device, type, vendor, product, version, usage_page, usage, buttons, reserved; };
static_assert(sizeof(DeviceProperties) == 40, "DeviceProperties layout");

bool device_exists(uint32_t device) { return device == kRimKeyboardDevice || device == kRimMouseDevice; }
uint32_t mask_of(uint32_t device) { return device == kRimKeyboardDevice ? 1u : 2u; }

DeviceProperties properties_of(uint32_t device) {
  DeviceProperties p = {};
  p.size = sizeof(p);
  p.device = device;
  p.usage_page = 1;
  if (device == kRimKeyboardDevice) { p.type = kRimTypeKeyboard; p.usage = 6; p.buttons = 104; }
  else { p.type = kRimTypeMouse; p.usage = 2; p.buttons = 5; }
  return p;
}

// Delivers a packet to every registration and observer that wants the device.
void dispatch(State& s, const RimPacket& packet) {
  const uint32_t mask = mask_of(packet.device);
  for (RimRegistration* r : s.rim_registrations.view()) if (r->device_mask & mask) r->queue.push(packet);
  for (RimObserver* o : s.rim_observers.view()) if (o->active && (o->device_mask & mask)) o->queue.push(packet);
  wake_all();
}

// Takes the next packet of a queue, marking the first one after a drop.
bool take(RimQueue& q, RimPacket* out) {
  const uint32_t lost = q.overflow;
  if (!q.pop(out)) return false;
  if (lost != 0) { out->flags |= kPacketOverflow; q.overflow = 0; }
  return true;
}

// NtRIMGetDeviceProperties(DWORD device, DeviceProperties* out, DWORD size) (and the lock-free variant)
uint64_t svc_get_properties(Call& c, Ctx& x) {
  if (!device_exists(c.arg32(0))) return kStatusNoSuchDevice;
  if (c.arg32(2) != sizeof(DeviceProperties)) return kStatusInfoLengthMismatch;
  return c.write_struct(c.arg(1), properties_of(c.arg32(0))) ? kStatusSuccess : kStatusAccessViolation;
}

// NtRIMQueryDevicePath(DWORD device, WCHAR* path, DWORD* bytes (in: room, out: needed with the NUL))
uint64_t svc_query_path(Call& c, Ctx& x) {
  const uint32_t device = c.arg32(0);
  if (!device_exists(device)) return kStatusNoSuchDevice;
  WideString path(x.st.allocator);
  path.append_ascii(device == kRimKeyboardDevice ? "\\\\?\\ACPI#PNP0303#4&1&0#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}" : "\\\\?\\ACPI#PNP0F13#4&1&1#{378de44c-56ef-11d1-bc8c-00a0c91405dd}");
  uint32_t room = 0;
  if (!c.read_u32(c.arg(2), &room)) return kStatusAccessViolation;
  const uint32_t needed = uint32_t((path.size() + 1) * 2);
  if (room < needed) { c.write_u32(c.arg(2), needed); return kStatusBufferTooSmall; }
  if (c.write_wide(c.arg(1), path.view(), room / 2) < 0 || !c.write_u32(c.arg(2), needed)) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtRIMAreSiblingDevices(DWORD a, DWORD b, BOOLEAN* siblings): both devices hang off the same parent only if they are the same device here
uint64_t svc_are_siblings(Call& c, Ctx& x) {
  if (!device_exists(c.arg32(0)) || !device_exists(c.arg32(1))) return kStatusNoSuchDevice;
  const uint8_t siblings = c.arg32(0) == c.arg32(1) ? 1 : 0;
  return c.write(c.arg(2), ConstByteView(&siblings, 1)) ? kStatusSuccess : kStatusAccessViolation;
}

// NtRIMGetDevicePreparsedData(DWORD device, void* out, DWORD* size): the PS/2 style devices of this machine are not HID devices and have none
uint64_t svc_get_preparsed(Call& c, Ctx& x) {
  if (!device_exists(c.arg32(0))) return kStatusNoSuchDevice;
  return kStatusInvalidDeviceRequest;
}

// NtRIMEnableMonitorMappingForDevice(DWORD device, DWORD monitor, BOOLEAN enable): only pointing devices map to a monitor
uint64_t svc_enable_monitor_mapping(Call& c, Ctx& x) {
  if (!device_exists(c.arg32(0))) return kStatusNoSuchDevice;
  if (c.arg32(0) != kRimMouseDevice) return kStatusInvalidDeviceRequest;
  if (c.arg32(1) != 1 && c.arg32(1) != 0) return kStatusInvalidParameter;      // (one monitor)
  return kStatusSuccess;
}

// NtRIMDeviceIoControl(DWORD device, DWORD ioctl, void* in, DWORD in_size, void* out, DWORD out_size, DWORD* returned): the virtual devices implement no control code
uint64_t svc_device_io_control(Call& c, Ctx& x) {
  if (!device_exists(c.arg32(0))) return kStatusNoSuchDevice;
  return kStatusInvalidDeviceRequest;
}

// NtRIMSetTestModeStatus(DWORD enable)
uint64_t svc_set_test_mode(Call& c, Ctx& x) {
  if (c.arg32(0) > 1) return kStatusInvalidParameter;
  x.st.rim_test_mode = c.arg32(0);
  return kStatusSuccess;
}

// NtRIMRegisterForInput(PHANDLE reg, DWORD device_mask (1 keyboard, 2 mouse), DWORD flags, DWORD hwnd) / NtRIMRegisterForInputEx(..., DWORD usage_filter (must be 0))
uint64_t register_input(Call& c, Ctx& x) {
  const uint32_t mask = c.arg32(1), flags = c.arg32(2);
  if (c.arg(0) == 0 || mask == 0 || (mask & ~kDeviceMaskAll) != 0 || (flags & ~kFlagsMask) != 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::RimRegistration) >= kMaxRegistrationsPerProcess) return kStatusQuotaExceeded;
  SharedPointer<RimRegistration> r = make_object<RimRegistration>(x, x.st.allocator);
  if (!r) return kStatusNoMemory;
  r->owner = c.pid(); r->device_mask = mask; r->flags = flags; r->hwnd = c.arg32(3);
  if (!x.st.rim_registrations.push_back(r.get())) return kStatusNoMemory;
  uint32_t status;
  SharedPointer<Object> base = r;
  const uint64_t h = new_handle(x, base, kRightAll, &status);      // (a failure destroys r, which takes itself off the list)
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
uint64_t svc_register(Call& c, Ctx& x) { return register_input(c, x); }
uint64_t svc_register_ex(Call& c, Ctx& x) {
  if (c.arg(4) != 0) return kStatusInvalidParameter;
  if (c.bad_stack()) return kStatusAccessViolation;
  return register_input(c, x);
}
// NtRIMUnregisterForInput(HANDLE reg)
uint64_t svc_unregister(Call& c, Ctx& x) {
  uint32_t status;
  if (!lookup<RimRegistration>(x, c.arg(0), Kind::RimRegistration, 0, &status)) return status;
  x.st.handles.close(c.arg(0), c.pid());
  return kStatusSuccess;
}

// NtRIMReadInput(HANDLE reg, RimPacket* out, DWORD size, DWORD* count, DWORD timeout_ms): reads as many packets as fit; with nothing queued it waits up to the timeout
// (0: not at all, STATUS_NO_MORE_ENTRIES; 0xFFFFFFFF: forever)
uint64_t svc_read_input(Call& c, Ctx& x) {
  uint32_t status;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::RimRegistration, kRightQuery, &status);
  if (!pin) return status;
  if (c.bad_stack()) return kStatusAccessViolation;
  RimRegistration* r = static_cast<RimRegistration*>(pin.get());
  const uint32_t room = c.arg32(2) / sizeof(RimPacket);
  if (room == 0) return kStatusBufferTooSmall;
  const uint32_t timeout = c.arg32(4);
  if (r->queue.packets.count() == 0) {
    if (timeout == 0) return kStatusNoMoreEntries;
    if (!wait_until(c, timeout_ns_of_ms(timeout), [&] { return r->queue.packets.count() != 0; })) return c.process().exited() ? kStatusThreadIsTerminating : kStatusTimeout;
  }
  uint32_t n = 0;
  RimPacket p;
  while (n < room && take(r->queue, &p)) {
    if (!c.write_struct(c.arg(1) + uint64_t{n} * sizeof(RimPacket), p)) return kStatusAccessViolation;
    ++n;
  }
  return c.write_u32(c.arg(3), n) ? kStatusSuccess : kStatusAccessViolation;
}

// --- observers (they see the stream, and may not consume it) --------------------------------------------------------------------------------------------------------------

// NtRIMAddInputObserver(PHANDLE observer, DWORD device_mask, DWORD flags)
uint64_t svc_add_observer(Call& c, Ctx& x) {
  const uint32_t mask = c.arg32(1);
  if (c.arg(0) == 0 || mask == 0 || (mask & ~kDeviceMaskAll) != 0 || (c.arg32(2) & ~kFlagsMask) != 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::RimObserver) >= kMaxObserversPerProcess) return kStatusQuotaExceeded;
  SharedPointer<RimObserver> o = make_object<RimObserver>(x, x.st.allocator);
  if (!o) return kStatusNoMemory;
  o->owner = c.pid(); o->device_mask = mask; o->flags = c.arg32(2);
  if (!x.st.rim_observers.push_back(o.get())) return kStatusNoMemory;
  uint32_t status;
  SharedPointer<Object> base = o;
  const uint64_t h = new_handle(x, base, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
// NtRIMRemoveInputObserver(HANDLE observer)
uint64_t svc_remove_observer(Call& c, Ctx& x) {
  uint32_t status;
  if (!lookup<RimObserver>(x, c.arg(0), Kind::RimObserver, 0, &status)) return status;
  x.st.handles.close(c.arg(0), c.pid());
  return kStatusSuccess;
}
// NtRIMUpdateInputObserverRegistration(HANDLE observer, DWORD device_mask (0: pause), DWORD flags): what the observer watches; packets already queued stay
uint64_t svc_update_observer(Call& c, Ctx& x) {
  uint32_t status;
  RimObserver* o = lookup<RimObserver>(x, c.arg(0), Kind::RimObserver, kRightModify, &status);
  if (!o) return status;
  if ((c.arg32(1) & ~kDeviceMaskAll) != 0 || (c.arg32(2) & ~kFlagsMask) != 0) return kStatusInvalidParameter;
  o->device_mask = c.arg32(1);
  o->flags = c.arg32(2);
  o->active = c.arg32(1) != 0;
  return kStatusSuccess;
}
// NtRIMObserveNextInput(HANDLE observer, RimPacket* out, DWORD size, DWORD timeout_ms): the next packet the observer saw
uint64_t svc_observe_next(Call& c, Ctx& x) {
  uint32_t status;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::RimObserver, kRightQuery, &status);
  if (!pin) return status;
  RimObserver* o = static_cast<RimObserver*>(pin.get());
  if (c.arg32(2) < sizeof(RimPacket)) return kStatusBufferTooSmall;
  if (o->queue.packets.count() == 0) {
    if (c.arg32(3) == 0) return kStatusNoMoreEntries;
    if (!wait_until(c, timeout_ns_of_ms(c.arg32(3)), [&] { return o->queue.packets.count() != 0; })) return c.process().exited() ? kStatusThreadIsTerminating : kStatusTimeout;
  }
  RimPacket p;
  take(o->queue, &p);
  return c.write_struct(c.arg(1), p) ? kStatusSuccess : kStatusAccessViolation;
}

}  // namespace

// The packets the MIT synthesize services and the kernel's drivers produce.
void rim_dispatch_key(State& s, uint32_t virtual_key, uint32_t scan_code, bool down, bool extended, uint64_t time_ns) {
  RimPacket p;
  p.type = kRimTypeKeyboard; p.device = kRimKeyboardDevice; p.time_ns = time_ns;
  p.flags = (down ? 0 : 2u) | (extended ? 4u : 0u);
  p.a = int32_t(virtual_key); p.b = int32_t(scan_code);
  dispatch(s, p);
}
void rim_dispatch_mouse(State& s, int32_t dx, int32_t dy, uint32_t button_flags, int32_t wheel, uint64_t time_ns) {
  RimPacket p;
  p.type = kRimTypeMouse; p.device = kRimMouseDevice; p.time_ns = time_ns;
  p.flags = button_flags << 8;
  p.a = dx; p.b = dy; p.c = wheel;
  dispatch(s, p);
}

void rim_feed_key(uint32_t virtual_key, uint32_t scan_code, bool down, bool extended, uint64_t time_ns) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  objects->sync().lock();
  if (State* s = state()) rim_dispatch_key(*s, virtual_key, scan_code, down, extended, time_ns);
  objects->sync().unlock();
}
void rim_feed_mouse(int32_t dx, int32_t dy, uint32_t button_flags, int32_t wheel, uint64_t time_ns) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  objects->sync().lock();
  if (State* s = state()) rim_dispatch_mouse(*s, dx, dy, button_flags, wheel, time_ns);
  objects->sync().unlock();
}

void install_rim() {
  COMPOSITION_REGISTER(NtRIMGetDeviceProperties, svc_get_properties);
  COMPOSITION_REGISTER(NtRIMGetDevicePropertiesLockfree, svc_get_properties);
  COMPOSITION_REGISTER(NtRIMQueryDevicePath, svc_query_path);
  COMPOSITION_REGISTER(NtRIMAreSiblingDevices, svc_are_siblings);
  COMPOSITION_REGISTER(NtRIMGetDevicePreparsedData, svc_get_preparsed);
  COMPOSITION_REGISTER(NtRIMGetDevicePreparsedDataLockfree, svc_get_preparsed);
  COMPOSITION_REGISTER(NtRIMEnableMonitorMappingForDevice, svc_enable_monitor_mapping);
  COMPOSITION_REGISTER(NtRIMDeviceIoControl, svc_device_io_control);
  COMPOSITION_REGISTER(NtRIMSetTestModeStatus, svc_set_test_mode);
  COMPOSITION_REGISTER(NtRIMRegisterForInput, svc_register);
  COMPOSITION_REGISTER(NtRIMRegisterForInputEx, svc_register_ex);
  COMPOSITION_REGISTER(NtRIMUnregisterForInput, svc_unregister);
  COMPOSITION_REGISTER(NtRIMReadInput, svc_read_input);
  COMPOSITION_REGISTER(NtRIMAddInputObserver, svc_add_observer);
  COMPOSITION_REGISTER(NtRIMRemoveInputObserver, svc_remove_observer);
  COMPOSITION_REGISTER(NtRIMUpdateInputObserverRegistration, svc_update_observer);
  COMPOSITION_REGISTER(NtRIMObserveNextInput, svc_observe_next);
}

}  // namespace process::win32k::composition
