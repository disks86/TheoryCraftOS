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

#include "Hardware/NetworkInterface/PciNicDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/Bus/Pci/PciBar.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04;
constexpr uint32_t kCommandMemory = 1u << 1, kCommandBusMaster = 1u << 2;
constexpr uint8_t kBarCount = 6;
}  // namespace

bool PciNicDevice::claim() {
  // The firmware's driver goes first: sizing a BAR switches decoding off for a moment, which must not happen under a driver that is using the device.
  if (st_) detach_firmware_drivers(st_, device_.address);
  detached_ = true;

  MmioRegion region;
  for (uint8_t bar = 0; bar < kBarCount && !region.valid(); ++bar) {
    PciBarInfo info;
    if (!pci_read_bar(pci_, device_.address, bar, &info)) break;
    if (info.valid && !info.io && info.base != 0) {
      if (mapper_) {
        if (!mapper_->map_registers(info.base, info.size, &region)) return false;
      } else {
        region = MmioRegion(info.base, info.size);
      }
    }
    if (info.is64) ++bar;  // the upper half of this BAR is the next register
  }
  if (!region.valid()) return false;
  registers_ = MmioNicRegisters(region);

  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;  // (the status half is write-1-to-clear)
  pci_.write32(device_.address, kPciCommand, command | kCommandMemory | kCommandBusMaster);
  return true;
}

void PciNicDevice::release() {
  registers_ = MmioNicRegisters();
  if (!detached_) return;
  detached_ = false;
  if (st_) reattach_firmware_drivers(st_, device_.address);
}

}  // namespace hardware
