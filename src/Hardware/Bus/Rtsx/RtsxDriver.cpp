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

#include "Hardware/Bus/Rtsx/RtsxDriver.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPciCommand = 0x04;
}

const char* RtsxDriver::outcome_name(Outcome o) {
  switch (o) {
    case Outcome::NotStarted: return "not started";
    case Outcome::Started: return "started";
    case Outcome::UnknownDevice: return "not a supported card reader";
    case Outcome::BarUnmapped: return "the registers could not be mapped";
    case Outcome::OutOfMemory: return "out of memory";
    case Outcome::ChipFailed: return "the chip did not come up";
  }
  return "";
}

RtsxDriver::Outcome RtsxDriver::start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper) {
  stop();
  if (!supports(device)) return outcome_ = Outcome::UnknownDevice;
  const uint32_t command = pci.read32(device.address, kPciCommand) & 0xFFFF;
  pci.write32(device.address, kPciCommand, command | (1u << 1) | (1u << 2) | (1u << 10));     // memory space, bus mastering, INTx disabled
  const uint64_t base = pci.memory_bar(device.address, 0);
  MmioRegion region;
  if (!base || !mapper.map_registers(base, kRegisterBytes, &region)) return outcome_ = Outcome::BarUnmapped;
  mmio_ = UniquePointer<MmioRtsxRegisters>::make(allocator_, region);
  if (!mmio_) return outcome_ = Outcome::OutOfMemory;
  return start_registers(*mmio_);
}

RtsxDriver::Outcome RtsxDriver::start_registers(IRtsxRegisters& registers) {
  host_ = UniquePointer<RtsxHost>::make(allocator_, registers, allocator_);
  if (!host_) return outcome_ = Outcome::OutOfMemory;
  if (!host_->initialize()) {
    host_.reset();
    return outcome_ = Outcome::ChipFailed;
  }
  return outcome_ = Outcome::Started;
}

void RtsxDriver::stop() {
  if (host_) host_->power_off();
  host_.reset();
  mmio_.reset();
  outcome_ = Outcome::NotStarted;
}

}  // namespace hardware
