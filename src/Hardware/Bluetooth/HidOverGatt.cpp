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

#include "Hardware/Bluetooth/HidOverGatt.hpp"

namespace hardware {

namespace {
constexpr uint16_t kHidService = 0x1812, kReportMap = 0x2A4B, kReport = 0x2A4D, kBootKeyboardInput = 0x2A22, kBootMouseInput = 0x2A33, kProtocolMode = 0x2A4E, kReportReference = 0x2908;

void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

// The fixed layouts of the boot protocol, as input reports: a keyboard (modifiers, one reserved byte, six keys) and a mouse (three buttons, X, Y).
hid::InputReport boot_keyboard() {
  hid::InputReport r;
  r.app_page = hid::kPageGenericDesktop;
  r.app_usage = hid::kUsageKeyboard;
  hid::InputField modifiers;
  modifiers.page = hid::kPageKeyboard;
  modifiers.usage_min = 0xE0;
  modifiers.usage_max = 0xE7;
  modifiers.bit_offset = 0;
  modifiers.bit_size = 1;
  modifiers.count = 8;
  modifiers.variable = true;
  hid::InputField keys;
  keys.page = hid::kPageKeyboard;
  keys.usage_min = 0;
  keys.usage_max = 0xFF;
  keys.bit_offset = 16;
  keys.bit_size = 8;
  keys.count = 6;
  r.fields[0] = modifiers;
  r.fields[1] = keys;
  r.field_count = 2;
  r.bits = 64;
  return r;
}

hid::InputReport boot_mouse() {
  hid::InputReport r;
  r.app_page = hid::kPageGenericDesktop;
  r.app_usage = hid::kUsageMouse;
  hid::InputField buttons;
  buttons.page = hid::kPageButton;
  buttons.usage_min = 1;
  buttons.usage_max = 3;
  buttons.bit_size = 1;
  buttons.count = 3;
  buttons.variable = true;
  hid::InputField x;
  x.page = hid::kPageGenericDesktop;
  x.usage_min = x.usage_max = hid::kUsageX;
  x.bit_offset = 8;
  x.bit_size = 8;
  x.logical_min = -127;
  x.logical_max = 127;
  x.variable = x.relative = true;
  hid::InputField y = x;
  y.usage_min = y.usage_max = hid::kUsageY;
  y.bit_offset = 16;
  r.fields[0] = buttons;
  r.fields[1] = x;
  r.fields[2] = y;
  r.field_count = 3;
  r.bits = 24;
  return r;
}
}

// ------------------------------------------------------------------ the devices

void BluetoothHidKeyboard::deliver(const hid::KeyboardState& state) {
  kernel::ScopedMutex guard(mutex_);
  if (count_ == kRing) {                                    // full: the newest state replaces the one before it (the latest state is what counts)
    ring_[(head_ + count_ - 1) % kRing] = state;
    return;
  }
  ring_[(head_ + count_) % kRing] = state;
  ++count_;
}

void BluetoothHidKeyboard::fetch() {
  for (;;) {
    hid::KeyboardState state;
    {
      kernel::ScopedMutex guard(mutex_);
      if (count_ == 0) return;
      state = ring_[head_];
      head_ = (head_ + 1) % kRing;
      --count_;
    }
    diff_.update(state, [this](uint8_t usage, bool pressed) { push(usage, pressed); });
  }
}

void BluetoothHidKeyboard::on_release_all() {
  {
    kernel::ScopedMutex guard(mutex_);
    count_ = 0;
  }
  diff_.release_all([this](uint8_t usage, bool pressed) { push(usage, pressed); });
}

void BluetoothHidMouse::deliver(const MouseEvent& event) {
  kernel::ScopedMutex guard(mutex_);
  if (count_ == kRing) {                                    // full: fold the new event into the last one
    MouseEvent& last = ring_[(head_ + count_ - 1) % kRing];
    last.dx += event.dx;
    last.dy += event.dy;
    last.wheel += event.wheel;
    last.buttons = event.buttons;
    if (event.absolute) { last.x = event.x; last.y = event.y; }
    return;
  }
  ring_[(head_ + count_) % kRing] = event;
  ++count_;
}

void BluetoothHidMouse::fetch() {
  for (;;) {
    MouseEvent e;
    {
      kernel::ScopedMutex guard(mutex_);
      if (count_ == 0) return;
      e = ring_[head_];
      head_ = (head_ + 1) % kRing;
      --count_;
    }
    last_ = e;
    push(e);
  }
}

void BluetoothHidMouse::on_release_all() {
  {
    kernel::ScopedMutex guard(mutex_);
    count_ = 0;
  }
  if (last_.buttons == 0) return;
  MouseEvent e = last_;
  e.buttons = 0;
  e.dx = e.dy = e.wheel = 0;
  last_ = e;
  push(e);
}

// ------------------------------------------------------------------ the profile

bool HidOverGatt::matches(const BluetoothDeviceInfo&, const GattDatabase& database) const { return database.find_service(Uuid::from16(kHidService)) >= 0; }

HidOverGatt::Record* HidOverGatt::record_for(uint32_t remote_id) {
  for (Record& r : records_)
    if (r.used && r.remote_id == remote_id) return &r;
  return nullptr;
}

uint32_t HidOverGatt::device_count() const {
  uint32_t n = 0;
  for (const Record& r : records_) n += r.used && r.ready && !r.gone;
  return n;
}

bool HidOverGatt::attach(IBluetoothRemote& remote) {
  Record* record = nullptr;
  {
    kernel::ScopedMutex guard(mutex_);
    for (Record& r : records_)
      if (!r.used) { record = &r; break; }
    if (!record) return false;
    record->clear();
    record->used = true;
    record->remote_id = remote.id();
    copy_text(record->label, sizeof record->label, remote.info().name[0] ? remote.info().name : "Bluetooth device");
  }
  // (the traffic below can bring notifications in: the lock is not held for it)
  const bool ok = attach_reports(remote, *record) || attach_boot(remote, *record);
  kernel::ScopedMutex guard(mutex_);
  if (!ok) { record->clear(); return false; }
  record->ready = true;
  return true;
}

bool HidOverGatt::attach_reports(IBluetoothRemote& remote, Record& record) {
  const GattDatabase& db = remote.database();
  const int service = db.find_service(Uuid::from16(kHidService));
  const int map_index = db.find_characteristic(Uuid::from16(kReportMap), Uuid::from16(kHidService));
  if (service < 0 || map_index < 0) return false;
  uint8_t descriptor[kMaxMap];
  uint32_t length = 0;
  if (remote.read(map_index, ByteView(descriptor), &length) != AttStatus::Ok || length == 0) return false;
  UniquePointer<hid::ReportMap> map = UniquePointer<hid::ReportMap>::make(allocator_);
  if (!map || !hid::parse_report_map(ConstByteView(descriptor, length), map.get())) return false;

  bool want_keyboard = false, want_mouse = false;
  int route_characteristic[kMaxReports] = {};
  for (uint32_t c = 0; c < db.characteristic_count && record.route_count < kMaxReports; ++c) {
    const GattCharacteristic& ch = db.characteristics[c];
    if (ch.service != service || ch.uuid != Uuid::from16(kReport) || !(ch.properties & gatt::kPropNotify)) continue;
    uint16_t reference = 0;
    for (uint32_t d = 0; d < db.descriptor_count; ++d)
      if (db.descriptors[d].characteristic == c && db.descriptors[d].uuid == Uuid::from16(kReportReference)) reference = db.descriptors[d].handle;
    uint8_t ref[4];
    uint32_t ref_length = 0;
    if (reference == 0 || remote.read_descriptor(reference, ByteView(ref), &ref_length) != AttStatus::Ok || ref_length < 2 || ref[1] != 1) continue;     // (input reports only)
    const hid::InputReport* report = map->find(ref[0]);
    if (!report) continue;
    Route route;
    route.value_handle = ch.value_handle;
    if (report->app_page == hid::kPageGenericDesktop && report->app_usage == hid::kUsageKeyboard && !want_keyboard) {
      if (!route.keyboard_decoder.init(*report)) continue;
      route.keyboard = true;
      want_keyboard = true;
    } else if (report->app_page == hid::kPageGenericDesktop && (report->app_usage == hid::kUsageMouse || report->app_usage == hid::kUsagePointer) && !want_mouse) {
      if (!route.pointer_decoder.init(*report)) continue;
      want_mouse = true;
    } else {
      continue;
    }
    route_characteristic[record.route_count] = static_cast<int>(c);
    record.routes[record.route_count++] = route;
  }
  if (record.route_count == 0) return false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (want_keyboard) record.keyboard = UniquePointer<BluetoothHidKeyboard>::make(allocator_);
    if (want_mouse) {
      record.mouse = UniquePointer<BluetoothHidMouse>::make(allocator_);
      for (uint32_t i = 0; i < record.route_count; ++i)
        if (!record.routes[i].keyboard && record.mouse) record.mouse->configure(record.routes[i].pointer_decoder.absolute(), record.routes[i].pointer_decoder.max_x(), record.routes[i].pointer_decoder.max_y());
    }
  }
  uint32_t subscribed = 0;
  for (uint32_t i = 0; i < record.route_count; ++i)
    if (remote.subscribe(route_characteristic[i], false, this) == AttStatus::Ok) ++subscribed;
  return subscribed > 0 && (record.keyboard || record.mouse);
}

bool HidOverGatt::attach_boot(IBluetoothRemote& remote, Record& record) {
  const GattDatabase& db = remote.database();
  const int keyboard = db.find_characteristic(Uuid::from16(kBootKeyboardInput), Uuid::from16(kHidService));
  const int mouse = db.find_characteristic(Uuid::from16(kBootMouseInput), Uuid::from16(kHidService));
  if (keyboard < 0 && mouse < 0) return false;
  record.route_count = 0;
  int route_characteristic[2] = {-1, -1};
  if (keyboard >= 0) {
    Route route;
    route.value_handle = db.characteristics[keyboard].value_handle;
    route.keyboard = true;
    route.keyboard_decoder.init(boot_keyboard());
    route_characteristic[record.route_count] = keyboard;
    record.routes[record.route_count++] = route;
  }
  if (mouse >= 0) {
    Route route;
    route.value_handle = db.characteristics[mouse].value_handle;
    route.pointer_decoder.init(boot_mouse());
    route_characteristic[record.route_count] = mouse;
    record.routes[record.route_count++] = route;
  }
  {
    kernel::ScopedMutex guard(mutex_);
    if (keyboard >= 0) record.keyboard = UniquePointer<BluetoothHidKeyboard>::make(allocator_);
    if (mouse >= 0) {
      record.mouse = UniquePointer<BluetoothHidMouse>::make(allocator_);
      if (record.mouse) record.mouse->configure(false, 0, 0);
    }
  }
  const int protocol = db.find_characteristic(Uuid::from16(kProtocolMode), Uuid::from16(kHidService));
  if (protocol >= 0) {
    const uint8_t boot = 0;
    remote.write(protocol, ConstByteView(&boot, 1), false);
  }
  uint32_t subscribed = 0;
  for (uint32_t i = 0; i < record.route_count; ++i)
    if (remote.subscribe(route_characteristic[i], false, this) == AttStatus::Ok) ++subscribed;
  return subscribed > 0;
}

void HidOverGatt::on_notification(IBluetoothRemote& remote, uint16_t value_handle, ConstByteView value, bool) {
  kernel::ScopedMutex guard(mutex_);
  Record* record = record_for(remote.id());
  if (!record || record->gone) return;
  for (uint32_t i = 0; i < record->route_count; ++i) {
    Route& route = record->routes[i];
    if (route.value_handle != value_handle) continue;
    if (route.keyboard) {
      hid::KeyboardState state;
      if (record->keyboard && route.keyboard_decoder.decode(value, &state)) record->keyboard->deliver(state);
    } else {
      MouseEvent event;
      if (record->mouse && route.pointer_decoder.decode(value, &event)) record->mouse->deliver(event);
    }
    return;
  }
}

void HidOverGatt::detach(IBluetoothRemote& remote) {
  kernel::ScopedMutex guard(mutex_);
  if (Record* record = record_for(remote.id())) record->gone = true;
}

uint32_t HidOverGatt::poll(IInputSink& sink) {
  uint32_t changes = 0;
  kernel::ScopedMutex guard(mutex_);
  for (Record& r : records_) {
    if (!r.used || (!r.ready && !r.gone)) continue;
    if (r.gone) {
      if (r.keyboard_added && r.keyboard) { r.keyboard->release_all(); sink.remove_keyboard(r.keyboard.get()); }
      if (r.mouse_added && r.mouse) { r.mouse->release_all(); sink.remove_mouse(r.mouse.get()); }
      if (r.keyboard_added || r.mouse_added) {
        char text[96];
        uint32_t at = 0;
        for (const char* p = r.label; *p && at + 1 < sizeof text; ++p) text[at++] = *p;
        for (const char* p = ": disconnected"; *p && at + 1 < sizeof text; ++p) text[at++] = *p;
        sink.notify(StringView(text, at));
        ++changes;
      }
      r.clear();
      continue;
    }
    if (r.keyboard && !r.keyboard_added) r.keyboard_added = sink.add_keyboard(r.keyboard.get());
    if (r.mouse && !r.mouse_added) r.mouse_added = sink.add_mouse(r.mouse.get());
    if ((r.keyboard_added || r.mouse_added) && !r.announced) {
      r.announced = true;
      char text[96];
      uint32_t at = 0;
      for (const char* p = r.label; *p && at + 1 < sizeof text; ++p) text[at++] = *p;
      for (const char* p = ": connected"; *p && at + 1 < sizeof text; ++p) text[at++] = *p;
      sink.notify(StringView(text, at));
      ++changes;
    }
  }
  return changes;
}

void HidOverGatt::detach_all(IInputSink& sink) {
  {
    kernel::ScopedMutex guard(mutex_);
    for (Record& r : records_)
      if (r.used) r.gone = true;
  }
  poll(sink);
}

}  // namespace hardware
