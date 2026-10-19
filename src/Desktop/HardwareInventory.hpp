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

// What the computer manager's Hardware tab shows: the devices of the machine, each in a group by kind. The desktop knows nothing about drivers;
// whoever builds the system hands it an IHardwareInventory (SystemInventory, in the real system), which looks at the machine whenever refresh() is
// called. HardwareList is a plain list of entries that inventories (and tests) fill.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/StringView.hpp"

namespace desktop {

enum class HardwareGroup : uint8_t {
  Processors,
  Memory,
  DisplayAdapters,
  Displays,
  Storage,
  Network,
  Audio,
  Midi,
  Input,
  Usb,
  Printers,
  Cameras,
  System,   // everything else on the buses
};
constexpr uint32_t kHardwareGroupCount = 13;

// "Processors", "Display adapters", "Displays", "Disk drives", ...
const char* hardware_group_name(HardwareGroup group);

struct HardwareEntry {
  HardwareGroup group = HardwareGroup::System;
  char name[64] = {};
  char detail[96] = {};   // one line about the device: its size, address, state
  uint32_t id = 0;        // the device's number among those of its kind in its subsystem (a network adapter's number, for its properties)
};

class HardwareList {
 public:
  explicit HardwareList(allocators::IAllocator& allocator) : entries_(allocator) {}
  void clear() { entries_.clear(); }
  // Adds a device (text is cut to fit). False if memory ran out.
  bool add(HardwareGroup group, StringView name, StringView detail = StringView(), uint32_t id = 0);
  uint32_t count() const { return static_cast<uint32_t>(entries_.count()); }
  const HardwareEntry& at(uint32_t index) const { return entries_[index]; }
  bool same_as(const HardwareList& other) const;

 private:
  List<HardwareEntry> entries_;
};

class IHardwareInventory {
 public:
  // Looks at the machine again; true if the list is not what it was at the last call.
  virtual bool refresh() = 0;
  virtual uint32_t count() const = 0;
  virtual bool entry(uint32_t index, HardwareEntry* out) const = 0;

 protected:
  constexpr IHardwareInventory() = default;
  ~IHardwareInventory() = default;
};

}  // namespace desktop
