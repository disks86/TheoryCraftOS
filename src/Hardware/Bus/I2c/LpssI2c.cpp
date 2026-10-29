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


#include "Hardware/Bus/I2c/LpssI2c.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint16_t kCommand = 0x04, kCapabilityPowerManagement = 0x01;
}  // namespace

bool LpssI2cController::prepare(IPciBus& pci, PciAddress address, uint64_t* bar_out) {
  const uint32_t id = pci.read32(address, 0);
  if (!is_lpss_i2c(static_cast<uint16_t>(id), static_cast<uint16_t>(id >> 16))) return false;
  // the function must be in D0 (the power management registers' power state bits) and decode memory (only the command half of the register is written: the status half has bits that clear by being written)
  const uint16_t pm = pci.find_capability(address, kCapabilityPowerManagement);
  if (pm != 0) {
    const uint32_t control = pci.read32(address, pm + 4);
    if ((control & 3) != 0) {
      pci.write32(address, pm + 4, control & ~3u & 0xFFFFu);
      delay_us(10000);                                           // (the time to come back from D3hot)
    }
  }
  pci.write32(address, kCommand, (pci.read32(address, kCommand) & 0xFFFF) | 2);
  *bar_out = pci.memory_bar(address, 0);
  return *bar_out != 0;
}

bool LpssI2cController::initialize(II2cRegisters& registers, PciAddress address, const DesignWareI2c::Config& config) {
  address_ = address;
  forward_.target = &registers;
  DesignWareI2c::lpss_release_reset(registers);
  return master_.initialize(config);
}

}  // namespace hardware
