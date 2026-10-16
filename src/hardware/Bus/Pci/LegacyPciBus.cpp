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

#include "hardware/Bus/Pci/LegacyPciBus.hpp"

namespace hardware {

namespace {
uint32_t config_address(PciAddress a, uint16_t offset) {
  return 0x80000000u | (uint32_t{a.bus} << 16) | (uint32_t{a.device} << 11) |
         (uint32_t{a.function} << 8) | (offset & 0xFC);
}
}  // namespace

bool LegacyPciBus::initialize() {
  scan(0, 255);
  return true;
}

uint32_t LegacyPciBus::read32(PciAddress address, uint16_t offset) const {
  if (offset >= 256) return 0xFFFFFFFF;  // extended config space needs PCIe
  address_port_.write32(config_address(address, offset));
  return data_port_.read32();
}

void LegacyPciBus::write32(PciAddress address, uint16_t offset, uint32_t value) const {
  if (offset >= 256) return;
  address_port_.write32(config_address(address, offset));
  data_port_.write32(value);
}

}  // namespace hardware
