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

#include "Hardware/Bus/Ish/IshDriver.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPciCommand = 0x04;
}

const char* IshDriver::outcome_name(Outcome o) {
  switch (o) {
    case Outcome::NotStarted: return "not started";
    case Outcome::Started: return "started";
    case Outcome::UnknownDevice: return "not a known sensor hub";
    case Outcome::BarUnmapped: return "the registers could not be mapped";
    case Outcome::OutOfMemory: return "out of memory";
    case Outcome::FirmwareNotReady: return "the firmware does not answer the IPC reset";
    case Outcome::BusFailed: return "the ISHTP bus did not start";
    case Outcome::NoHidClient: return "the firmware has no HID client or no devices";
  }
  return "";
}

IshDriver::Outcome IshDriver::start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper, SensorHub* hub) {
  stop();
  if (!supports(device)) return outcome_ = Outcome::UnknownDevice;
  const uint32_t command = pci.read32(device.address, kPciCommand) & 0xFFFF;
  pci.write32(device.address, kPciCommand, command | (1u << 1) | (1u << 2));          // memory space and bus mastering
  const uint64_t base = pci.memory_bar(device.address, 0);
  MmioRegion region;
  if (!base || !mapper.map_registers(base, kRegisterBytes, &region)) return outcome_ = Outcome::BarUnmapped;
  mmio_ = UniquePointer<MmioIshRegisters>::make(allocator_, region);
  if (!mmio_) return outcome_ = Outcome::OutOfMemory;
  return start_registers(*mmio_, hub);
}

IshDriver::Outcome IshDriver::start_registers(IIshRegisters& registers, SensorHub* hub) {
  ipc_ = UniquePointer<IshIpc>::make(allocator_, registers);
  if (!ipc_) return outcome_ = Outcome::OutOfMemory;
  bus_ = UniquePointer<IshtpBus>::make(allocator_, allocator_, *ipc_);
  hid_ = UniquePointer<IshHid>::make(allocator_, allocator_, *bus_);
  if (!bus_ || !hid_) return outcome_ = Outcome::OutOfMemory;
  if (!ipc_->start()) return outcome_ = Outcome::FirmwareNotReady;
  if (!bus_->start()) return outcome_ = Outcome::BusFailed;
  if (!hid_->start(hub) || hid_->device_count() == 0) return outcome_ = Outcome::NoHidClient;
  return outcome_ = Outcome::Started;
}

void IshDriver::stop() {
  hid_.reset();
  bus_.reset();
  ipc_.reset();
  mmio_.reset();
  outcome_ = Outcome::NotStarted;
}

}  // namespace hardware
