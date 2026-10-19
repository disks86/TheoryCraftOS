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

#include "Desktop/HardwareInventory.hpp"

namespace desktop {

const char* hardware_group_name(HardwareGroup group) {
  switch (group) {
    case HardwareGroup::Processors: return "Processors";
    case HardwareGroup::Memory: return "Memory";
    case HardwareGroup::DisplayAdapters: return "Display adapters";
    case HardwareGroup::Displays: return "Displays";
    case HardwareGroup::Storage: return "Disk drives";
    case HardwareGroup::Network: return "Network adapters";
    case HardwareGroup::Audio: return "Sound devices";
    case HardwareGroup::Midi: return "MIDI devices";
    case HardwareGroup::Input: return "Keyboards and mice";
    case HardwareGroup::Usb: return "USB controllers and devices";
    case HardwareGroup::Printers: return "Printers";
    case HardwareGroup::Cameras: return "Cameras";
    default: return "System devices";
  }
}

namespace {
void copy_text(char* out, uint32_t capacity, StringView text) {
  uint32_t n = 0;
  for (; n < text.size() && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}
bool same_text(const char* a, const char* b) {
  uint32_t i = 0;
  for (; a[i] && a[i] == b[i]; ++i) {}
  return a[i] == b[i];
}
}  // namespace

bool HardwareList::add(HardwareGroup group, StringView name, StringView detail, uint32_t id) {
  HardwareEntry e;
  e.group = group;
  copy_text(e.name, sizeof e.name, name);
  copy_text(e.detail, sizeof e.detail, detail);
  e.id = id;
  return entries_.push_back(e);
}

bool HardwareList::same_as(const HardwareList& other) const {
  if (entries_.count() != other.entries_.count()) return false;
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    const HardwareEntry &a = entries_[i], &b = other.entries_[i];
    if (a.group != b.group || a.id != b.id || !same_text(a.name, b.name) || !same_text(a.detail, b.detail)) return false;
  }
  return true;
}

}  // namespace desktop
