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

#include "Hardware/Bus/Pci/PciBar.hpp"

namespace hardware {

namespace {
constexpr uint16_t kCommand = 0x04, kHeaderType = 0x0C, kBar0 = 0x10;
constexpr uint32_t kCommandDecode = 0x3;   // I/O space and memory space enable

// How many BARs the function's header has.
uint8_t bar_count(IPciBus& pci, PciAddress address) {
  switch ((pci.read32(address, kHeaderType) >> 16) & 0x7F) {
    case 0: return 6;
    case 1: return 2;
    default: return 0;
  }
}
}  // namespace

bool pci_read_bar(IPciBus& pci, PciAddress address, uint8_t index, PciBarInfo* info) {
  *info = PciBarInfo();
  if (index >= bar_count(pci, address)) return false;
  const uint16_t reg = static_cast<uint16_t>(kBar0 + 4 * index);
  const uint32_t low = pci.read32(address, reg);
  if (low & 1) {   // I/O BAR
    info->io = true;
    info->base = low & ~3u;
    info->valid = true;
    return true;
  }
  const bool is64 = ((low >> 1) & 3) == 2 && index + 1 < bar_count(pci, address);
  const uint32_t high = is64 ? pci.read32(address, reg + 4) : 0;
  info->is64 = is64;
  info->prefetchable = (low >> 3) & 1;
  info->base = (low & ~0xFull) | (static_cast<uint64_t>(high) << 32);

  // size it with decoding off (the command register's status half is write-1-to-clear: write zeros there)
  const uint32_t command = pci.read32(address, kCommand);
  pci.write32(address, kCommand, command & 0xFFFF & ~kCommandDecode);
  pci.write32(address, reg, 0xFFFFFFFFu);
  const uint32_t mask_low = pci.read32(address, reg);
  uint32_t mask_high = 0xFFFFFFFFu;
  if (is64) {
    pci.write32(address, reg + 4, 0xFFFFFFFFu);
    mask_high = pci.read32(address, reg + 4);
    pci.write32(address, reg + 4, high);
  }
  pci.write32(address, reg, low);
  pci.write32(address, kCommand, command & 0xFFFF);

  uint64_t mask = mask_low & ~0xFull;
  if (is64) mask |= static_cast<uint64_t>(mask_high) << 32;
  else mask |= 0xFFFFFFFF00000000ull;   // a 32-bit BAR decodes 32 address bits: the size is within them
  if ((mask_low & ~0xFu) == 0 && !(is64 && mask_high)) return true;   // not implemented (reads back zero): valid stays false
  info->size = ~mask + 1;
  info->valid = info->size != 0;
  return true;
}

bool pci_map_bar(IPciBus& pci, MmioMapper& mapper, PciAddress address, uint8_t index, MmioKind kind, MmioRegion* region) {
  PciBarInfo bar;
  if (!pci_read_bar(pci, address, index, &bar) || !bar.valid || bar.io || bar.base == 0) return false;
  return mapper.map(bar.base, bar.size, kind, region);
}

uint32_t pci_map_all_bars(IPciBus& pci, MmioMapper& mapper) {
  uint32_t mapped = 0;
  for (uint32_t d = 0; d < pci.device_count(); ++d) {
    PciDeviceInfo device;
    if (!pci.pci_device(d, &device)) continue;
    const uint8_t count = bar_count(pci, device.address);
    for (uint8_t index = 0; index < count; ++index) {
      const uint32_t low = pci.read32(device.address, static_cast<uint16_t>(kBar0 + 4 * index));
      if (low & 1) continue;                        // I/O
      const bool is64 = ((low >> 1) & 3) == 2 && index + 1 < count;
      const uint64_t base = pci.memory_bar(device.address, index);
      if (is64) ++index;                            // the next register is this BAR's upper half
      if (base == 0 || mapper.is_mapped(base, 1)) continue;
      PciBarInfo bar;
      if (!pci_read_bar(pci, device.address, static_cast<uint8_t>(is64 ? index - 1 : index), &bar) || !bar.valid || bar.size == 0) continue;
      MmioRegion region;
      const bool frame_buffer = bar.prefetchable && bar.size >= (1u << 20);
      if (mapper.map(bar.base, bar.size, frame_buffer ? MmioKind::FrameBuffer : MmioKind::Registers, &region)) ++mapped;
    }
  }
  return mapped;
}

}  // namespace hardware
