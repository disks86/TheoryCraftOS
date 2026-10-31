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

#include "SystemBluetoothDetails.hpp"

using namespace desktop;

namespace {
void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

void address_text(const hardware::hci::BdAddr& a, char out[18]) {
  static const char hex[] = "0123456789ABCDEF";
  uint32_t n = 0;
  for (int i = 5; i >= 0; --i) {
    out[n++] = hex[a.b[i] >> 4];
    out[n++] = hex[a.b[i] & 15];
    if (i > 0) out[n++] = ':';
  }
  out[n] = 0;
}

// What kind of device it is, from what it advertised: the GAP appearance (LE) or the class of device (classic).
const char* kind_of(const hardware::BluetoothDeviceInfo& d) {
  if (d.le && d.appearance != 0) {
    switch (d.appearance >> 6) {
      case 0x01: return "Phone";
      case 0x02: return "Computer";
      case 0x03: return "Watch";
      case 0x05: return "Display";
      case 0x06: return "Remote";
      case 0x0A: return "Player";
      case 0x0D: return "Heart rate";
      case 0x0F:                                        // human interface device, the subcategory in the low six bits
        switch (d.appearance & 0x3F) {
          case 1: return "Keyboard";
          case 2: return "Mouse";
          case 3: case 4: return "Gamepad";
          case 5: return "Tablet";
          default: return "Input device";
        }
      default: break;
    }
  }
  if (!d.le && d.class_of_device != 0) {
    const uint32_t major = (d.class_of_device >> 8) & 0x1F, minor = (d.class_of_device >> 2) & 0x3F;
    switch (major) {
      case 1: return "Computer";
      case 2: return "Phone";
      case 4: return minor == 1 ? "Headset" : minor == 6 ? "Headphones" : "Audio";
      case 5: return (minor & 0x10) ? "Keyboard" : (minor & 0x20) ? "Mouse" : "Input device";
      case 7: return "Wearable";
      default: break;
    }
  }
  if (StringView(d.profiles, __builtin_strlen(d.profiles)).find(StringView("HID")) != StringView::npos) return "Input device";
  return "Device";
}

int signal_percent(int8_t rssi) {
  if (rssi == 127) return -1;
  int v = (rssi + 100) * 2;                 // -100 dBm is nothing, -50 dBm is full
  return v < 0 ? 0 : v > 100 ? 100 : v;
}

BluetoothDeviceState state_of(hardware::BluetoothLinkState s) {
  switch (s) {
    case hardware::BluetoothLinkState::Connecting: return BluetoothDeviceState::Connecting;
    case hardware::BluetoothLinkState::Pairing: return BluetoothDeviceState::Pairing;
    case hardware::BluetoothLinkState::Setup: return BluetoothDeviceState::Setup;
    case hardware::BluetoothLinkState::Connected: return BluetoothDeviceState::Connected;
    case hardware::BluetoothLinkState::Failed: return BluetoothDeviceState::Failed;
    default: return BluetoothDeviceState::Disconnected;
  }
}

BluetoothResult result_of(hardware::BluetoothResult r) {
  switch (r) {
    case hardware::BluetoothResult::Ok: return BluetoothResult::Ok;
    case hardware::BluetoothResult::Busy: return BluetoothResult::Busy;
    case hardware::BluetoothResult::NoSuchDevice: return BluetoothResult::NoSuchDevice;
    case hardware::BluetoothResult::Unsupported: return BluetoothResult::Unsupported;
    default: return BluetoothResult::NotReady;
  }
}

// connected first, then the ones being connected, then paired, then by signal
int rank(const BluetoothDeviceRow& r) {
  int rank = 0;
  if (r.state == BluetoothDeviceState::Connected) rank = 40;
  else if (r.state == BluetoothDeviceState::Connecting || r.state == BluetoothDeviceState::Pairing || r.state == BluetoothDeviceState::Setup) rank = 30;
  else if (r.paired) rank = 20;
  else if (r.supported) rank = 10;
  return rank;
}

hardware::hci::BdAddr address_from(const uint8_t key[6]) {
  hardware::hci::BdAddr a;
  for (int i = 0; i < 6; ++i) a.b[i] = key[i];
  return a;
}
}  // namespace

SystemBluetoothDetails::SystemBluetoothDetails(hardware::IBluetoothManager& manager) : manager_(manager) {}

void SystemBluetoothDetails::Ui::display_passkey(const hardware::hci::BdAddr& device, uint32_t passkey) { question.kind = BluetoothPromptKind::ShowPasskey; question.device = device; question.number = passkey; }
void SystemBluetoothDetails::Ui::confirm_numeric(const hardware::hci::BdAddr& device, uint32_t value) { question.kind = BluetoothPromptKind::ConfirmNumber; question.device = device; question.number = value; }
void SystemBluetoothDetails::Ui::request_passkey(const hardware::hci::BdAddr& device) { question.kind = BluetoothPromptKind::EnterPasskey; question.device = device; question.number = 0; }

void SystemBluetoothDetails::attach_adapters() {
  for (uint32_t i = 0; i < manager_.adapter_count() && i < kMaxAdapters; ++i) {
    hardware::IBluetoothAdapter* a = manager_.adapter(i);
    if (!a || uis_[i].adapter == a) continue;
    uis_[i].adapter = a;
    uis_[i].question = Question();
    a->set_pairing_ui(&uis_[i]);
  }
}

hardware::IBluetoothAdapter* SystemBluetoothDetails::find(uint32_t id) {
  attach_adapters();
  return id < kMaxAdapters && id < manager_.adapter_count() ? manager_.adapter(id) : nullptr;
}

bool SystemBluetoothDetails::is_bluetooth(uint32_t id) { return find(id) != nullptr; }

bool SystemBluetoothDetails::adapter(uint32_t id, BluetoothAdapterInfo* out) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a) return false;
  *out = BluetoothAdapterInfo();
  copy_text(out->name, sizeof out->name, a->name());
  address_text(a->address(), out->address);
  out->ready = a->ready();
  out->discovering = a->discovering();
  out->auto_reconnect = a->auto_reconnect();
  return true;
}

void SystemBluetoothDetails::set_auto_reconnect(uint32_t id, bool on) {
  if (find(id)) manager_.set_auto_reconnect(on);
}

bool SystemBluetoothDetails::set_discovery(uint32_t id, bool on) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a) return false;
  if (!on) {
    a->stop_discovery();
    return true;
  }
  return a->start_discovery() == hardware::BluetoothResult::Ok;
}

uint32_t SystemBluetoothDetails::devices(uint32_t id, BluetoothDeviceRow* out, uint32_t capacity) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < a->device_count() && n < capacity; ++i) {
    hardware::BluetoothDeviceInfo d;
    if (!a->device(i, &d)) continue;
    BluetoothDeviceRow& r = out[n];
    r = BluetoothDeviceRow();
    copy_text(r.name, sizeof r.name, d.name);
    address_text(d.address, r.address);
    for (int k = 0; k < 6; ++k) r.key[k] = d.address.b[k];
    copy_text(r.kind, sizeof r.kind, kind_of(d));
    r.signal_percent = static_cast<int8_t>(signal_percent(d.rssi));
    r.state = state_of(d.state);
    r.paired = d.paired;
    r.supported = d.le;
    copy_text(r.failure, sizeof r.failure, d.failure);
    copy_text(r.profiles, sizeof r.profiles, d.profiles);
    ++n;
  }
  for (uint32_t i = 1; i < n; ++i) {                                // (insertion sort: stable, and the list is short)
    const BluetoothDeviceRow row = out[i];
    uint32_t j = i;
    while (j > 0 && (rank(out[j - 1]) < rank(row) || (rank(out[j - 1]) == rank(row) && out[j - 1].signal_percent < row.signal_percent))) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = row;
  }
  return n;
}

BluetoothResult SystemBluetoothDetails::connect(uint32_t id, const uint8_t key[6]) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a) return BluetoothResult::NotReady;
  if (id < kMaxAdapters) uis_[id].question = Question();
  return result_of(a->connect(address_from(key), true));
}

void SystemBluetoothDetails::disconnect(uint32_t id, const uint8_t key[6]) {
  if (hardware::IBluetoothAdapter* a = find(id)) {
    a->disconnect(address_from(key));
    if (id < kMaxAdapters) uis_[id].question = Question();
  }
}

void SystemBluetoothDetails::forget(uint32_t id, const uint8_t key[6]) {
  if (hardware::IBluetoothAdapter* a = find(id)) a->forget(address_from(key));
}

bool SystemBluetoothDetails::prompt(uint32_t id, BluetoothPrompt* out) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a || id >= kMaxAdapters) return false;
  Question& q = uis_[id].question;
  if (q.kind == BluetoothPromptKind::None) return false;
  hardware::BluetoothDeviceInfo d;
  if (!a->find_device(q.device, &d) || d.state != hardware::BluetoothLinkState::Pairing) {      // the pairing is over (or was given up): the question goes with it
    q = Question();
    return false;
  }
  *out = BluetoothPrompt();
  out->kind = q.kind;
  out->number = q.number;
  copy_text(out->device, sizeof out->device, d.name[0] ? d.name : "the device");
  return true;
}

void SystemBluetoothDetails::answer_confirm(uint32_t id, bool accepted) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a || id >= kMaxAdapters || uis_[id].question.kind != BluetoothPromptKind::ConfirmNumber) return;
  const hardware::hci::BdAddr device = uis_[id].question.device;
  uis_[id].question = Question();
  a->confirm(device, accepted);
}

void SystemBluetoothDetails::answer_passkey(uint32_t id, uint32_t passkey) {
  hardware::IBluetoothAdapter* a = find(id);
  if (!a || id >= kMaxAdapters || uis_[id].question.kind != BluetoothPromptKind::EnterPasskey) return;
  const hardware::hci::BdAddr device = uis_[id].question.device;
  uis_[id].question = Question();
  a->passkey_entered(device, passkey);
}
