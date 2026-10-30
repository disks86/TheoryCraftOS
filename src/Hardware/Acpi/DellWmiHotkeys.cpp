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

#include "Hardware/Acpi/DellWmiHotkeys.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace hardware {

namespace {
// 9DBB5994-A997-11DA-B012-B622A1EF5492 as it lies in a _WDG entry (the first three fields little endian).
constexpr uint8_t kDellEventGuid[16] = {0x94, 0x59, 0xBB, 0x9D, 0x97, 0xA9, 0xDA, 0x11, 0xB0, 0x12, 0xB6, 0x22, 0xA1, 0xEF, 0x54, 0x92};
constexpr uint8_t kWdgFlagEvent = 0x08;
constexpr uint32_t kWdgEntryBytes = 20;

// The key for (type, code); 0 for the events that are not keys. Brightness is marked with kBrightnessUp / Down so that the caller can drop them.
uint8_t key_for(uint16_t type, uint16_t code) {
  if (type == 0x0000) {
    switch (code) {
      case 0x0109: return key::kMute;
      case 0x0150: return kDellMicMute;
      case 0xE005: return key::kBrightnessDown;
      case 0xE006: return key::kBrightnessUp;
      default: return 0;
    }
  }
  if (type == 0x0010) {
    switch (code) {
      case 0x0057: return key::kBrightnessDown;
      case 0x0058: return key::kBrightnessUp;
      case 0x0109: return key::kMute;
      case 0x0150: return kDellMicMute;
      default: return 0;
    }
  }
  return 0;
}

bool word_at(ConstByteView buffer, uint64_t index, uint16_t* out) {
  if (buffer.size() < 2 || index > buffer.size() / 2 - 1) return false;
  *out = static_cast<uint16_t>(buffer[index * 2] | (buffer[index * 2 + 1] << 8));
  return true;
}
}  // namespace

uint32_t dell_wmi_decode(ConstByteView buffer, uint8_t* keys, uint32_t capacity) {
  uint32_t found = 0;
  uint64_t at = 0;                                                    // in words
  uint16_t length;
  const uint64_t words = buffer.size() / 2;
  while (at < words && word_at(buffer, at, &length) && length != 0) {
    const uint64_t end = at + 1 + length;                             // (one past the entry)
    if (end > words) break;                                           // an entry that does not fit
    uint16_t type = 0, code = 0;
    word_at(buffer, at + 1, &type);
    if (type == 0x0000) {
      if (length >= 2 && word_at(buffer, at + 2, &code)) {
        const uint8_t k = key_for(type, code);
        if (k && found < capacity) keys[found++] = k;
      }
    } else if (type == 0x0010 || type == 0x0011) {
      for (uint64_t w = at + 2; w < end; ++w) {
        if (!word_at(buffer, w, &code)) break;
        const uint8_t k = key_for(type, code);
        if (k && found < capacity) keys[found++] = k;
      }
    }
    at = end;
  }
  return found;
}

uint32_t DellWmiHotkeys::scan() {
  count_ = 0;
  // the devices: anywhere below the root, PNP0C14
  acpi::Node* stack[32];
  uint32_t depth = 0;
  stack[depth++] = vm_.root();
  uint32_t visited = 0;
  while (depth != 0 && visited < 4096) {
    acpi::Node* parent = stack[--depth];
    for (acpi::Node* c = vm_.first_child(parent); c; c = vm_.next_sibling(c)) {
      ++visited;
      if (vm_.is_device(c) && vm_.device_has_id(c, "PNP0C14") && count_ < kMaxDevices) {
        acpi::Value wdg;
        uint8_t table[kWdgEntryBytes * 16];
        uint32_t size = 0;
        if (acpi::Node* node = vm_.find("_WDG", c); node && vm_.parent(node) == c && vm_.evaluate(node, nullptr, 0, &wdg) &&
            wdg.kind == acpi::ValueKind::Buffer && wdg.bytes) {
          size = wdg.length < sizeof table ? wdg.length : sizeof table;
          for (uint32_t i = 0; i < size; ++i) table[i] = wdg.bytes[i];
        }
        for (uint32_t at = 0; at + kWdgEntryBytes <= size; at += kWdgEntryBytes) {
          bool same = true;
          for (uint32_t i = 0; i < 16; ++i) same = same && table[at + i] == kDellEventGuid[i];
          if (same && (table[at + 19] & kWdgFlagEvent)) {
            devices_[count_].node = c;
            devices_[count_].notify_id = table[at + 16];
            ++count_;
            break;
          }
        }
      }
      if (depth < 32) stack[depth++] = c;
    }
  }
  return count_;
}

void DellWmiHotkeys::on_notify(acpi::Node* device, uint32_t value) {
  if (!sink_) return;
  uint8_t keys[8];
  uint32_t n = 0;
  {
    kernel::ScopedRecursiveMutex guard(vm_.mutex());     // _WED's buffer is copied before anything else can evaluate
    for (uint32_t d = 0; d < count_ && n == 0; ++d) {
      if (devices_[d].node != device || devices_[d].notify_id != value) continue;
      acpi::Node* wed = vm_.find("_WED", device);
      if (!wed || vm_.parent(wed) != device) return;
      acpi::Value argument;
      argument.kind = acpi::ValueKind::Integer;
      argument.integer = value;
      acpi::Value result;
      uint8_t copy[128];
      if (!vm_.evaluate(wed, &argument, 1, &result) || result.kind != acpi::ValueKind::Buffer || !result.bytes) return;
      const uint32_t size = result.length < sizeof copy ? result.length : sizeof copy;
      for (uint32_t i = 0; i < size; ++i) copy[i] = result.bytes[i];
      n = dell_wmi_decode(ConstByteView(copy, size), keys, 8);
      break;
    }
  }
  for (uint32_t i = 0; i < n; ++i) {                      // (the keys are acted on without the interpreter held: they post notices and change the brightness)
    if (!brightness_ && (keys[i] == key::kBrightnessUp || keys[i] == key::kBrightnessDown)) continue;
    sink_(context_, keys[i]);
  }
}

}  // namespace hardware
