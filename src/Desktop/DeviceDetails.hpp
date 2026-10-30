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

// What the generic device Properties window shows about any device of the Hardware tab: a title and a list of name/value lines. The desktop knows
// nothing about drivers: whoever builds the system hands it an IDeviceDetails (SystemInventory, in the real system), which asks the device for its
// facts by its class. Every device of a kind (every sound card, every disk) therefore gets the same window, and a new driver of the kind adds nothing
// here.
#pragma once
#include "Desktop/HardwareInventory.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/StringView.hpp"

namespace desktop {

struct DeviceProperty {
  char name[40] = {};
  char value[120] = {};
};

class DevicePropertyList {
 public:
  explicit DevicePropertyList(allocators::IAllocator& allocator) : lines_(allocator) {}
  char title[64] = {};   // the device's name
  void clear() { title[0] = 0; lines_.clear(); }
  // Adds a line (text is cut to fit). False if memory ran out.
  bool add(StringView name, StringView value);
  uint32_t count() const { return static_cast<uint32_t>(lines_.count()); }
  const DeviceProperty& at(uint32_t index) const { return lines_[index]; }

 private:
  List<DeviceProperty> lines_;
};

class IDeviceDetails {
 public:
  // Looks at the device of this kind with this number (HardwareEntry::id of its entry in the hardware tree) and fills `out`. False if there is none
  // (any more).
  virtual bool describe(HardwareGroup group, uint32_t id, DevicePropertyList* out) = 0;

 protected:
  constexpr IDeviceDetails() = default;
  ~IDeviceDetails() = default;
};

}  // namespace desktop
