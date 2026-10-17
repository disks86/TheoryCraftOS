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

#pragma once
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/MmioMapper.hpp"

namespace hardware {

// Base address registers (BARs): how big a device's windows are, and getting them mapped.
struct PciBarInfo {
  bool valid = false;         // the BAR exists
  bool io = false;            // an I/O port range (not handled further here)
  bool is64 = false;          // a 64-bit memory BAR: it takes this register and the next one
  bool prefetchable = false;
  uint64_t base = 0;          // where the firmware put it (0: not assigned)
  uint64_t size = 0;
};

// Finds out about BAR `index` (0-5) of a function, size included. The size is found the way the specification says: with the device's I/O and memory decoding
// switched off for a moment, ones are written to the BAR and the bits that stay zero give the size. The BAR and the command register are put back as they were.
// Header type 1 (bridge) functions have two BARs, type 2 none.
bool pci_read_bar(IPciBus& pci, PciAddress address, uint8_t index, PciBarInfo* info);

// Maps memory BAR `index` of a function (above 4 GiB if that is where it is) and returns its registers as a window. False if the BAR does not exist, is not
// assigned, is not memory, or cannot be mapped.
bool pci_map_bar(IPciBus& pci, MmioMapper& mapper, PciAddress address, uint8_t index, MmioKind kind, MmioRegion* region);

// Makes sure the memory BARs of every function are mapped: those whose first page is not mapped yet (typically the 64-bit ones the firmware placed
// above everything it maps) are sized and mapped, uncached, as registers - a prefetchable BAR of 1 MiB or more as a frame buffer. BARs that are mapped already
// are not touched, not even sized. Returns how many BARs were mapped.
uint32_t pci_map_all_bars(IPciBus& pci, MmioMapper& mapper);

}  // namespace hardware
