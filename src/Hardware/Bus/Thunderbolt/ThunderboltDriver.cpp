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

#include "Hardware/Bus/Thunderbolt/ThunderboltDriver.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04;
}

ThunderboltDriver::Outcome ThunderboltDriver::start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper) {
  stop();
  model_ = tb_find_nhi(device.vendor_id, device.device_id);
  if (!model_) return outcome_ = Outcome::UnknownDevice;
  // memory space and bus mastering; the status bits above the command register clear when written, so only the low half is kept
  const uint32_t command = pci.read32(device.address, kPciCommand) & 0xFFFF;
  pci.write32(device.address, kPciCommand, command | (1u << 1) | (1u << 2));
  const uint64_t base = pci.memory_bar(device.address, 0);
  MmioRegion region;
  if (!base || !mapper.map_registers(base, kRegisterBytes, &region)) return outcome_ = Outcome::BarUnmapped;
  mmio_ = UniquePointer<MmioTbRegisters>::make(allocator_, region);
  if (!mmio_) return outcome_ = Outcome::OutOfMemory;
  return start_device(*mmio_);
}

ThunderboltDriver::Outcome ThunderboltDriver::start_device(ITbRegisters& registers) {
  icm_ = UniquePointer<ThunderboltIcm>::make(allocator_, registers, allocator_);
  if (!icm_) return outcome_ = Outcome::OutOfMemory;
  const ThunderboltIcm::Outcome result = icm_->start();
  // (a controller in safe mode still answers, with no devices: reported as a failure so nobody waits for devices)
  return outcome_ = result == ThunderboltIcm::Outcome::Started ? Outcome::Started : Outcome::ControllerFailed;
}

void ThunderboltDriver::stop() {
  icm_ = UniquePointer<ThunderboltIcm>();
  mmio_ = UniquePointer<MmioTbRegisters>();
  outcome_ = Outcome::NotStarted;
}

}  // namespace hardware
