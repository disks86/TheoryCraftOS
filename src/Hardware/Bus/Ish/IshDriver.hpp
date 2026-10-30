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

// The PCI binding of the Intel Integrated Sensor Hub: memory decoding and bus mastering (the ISH may DMA), BAR 0 mapped, the IPC started, the ISHTP bus brought up and the HID client
// started, which makes the sensors of the hub and adds them to a SensorHub. The Sunrise Point (Skylake, Kaby Lake: PCI 8086:9D35 for the LP parts, 8086:A135 for the H parts) register
// layout only. poll() keeps the link going (call it now and then; reading a sensor polls too).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Ish/IshHid.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/RegisterMapper.hpp"

namespace hardware {

class MmioIshRegisters final : public IIshRegisters {
 public:
  explicit MmioIshRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

class IshDriver {
 public:
  enum class Outcome : uint8_t { NotStarted, Started, UnknownDevice, BarUnmapped, OutOfMemory, FirmwareNotReady, BusFailed, NoHidClient };
  static constexpr uint32_t kRegisterBytes = 0x1000;
  explicit IshDriver(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~IshDriver() { stop(); }

  static bool supports(const PciDeviceInfo& device) { return device.vendor_id == 0x8086 && (device.device_id == 0x9D35 || device.device_id == 0xA135); }
  static const char* outcome_name(Outcome outcome);

  Outcome start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper, SensorHub* hub);
  // The part after the registers are reachable (tests bring their own).
  Outcome start_registers(IIshRegisters& registers, SensorHub* hub);
  void stop();
  void poll() { if (hid_) hid_->poll(); }
  Outcome outcome() const { return outcome_; }
  IshHid* hid() { return hid_.get(); }
  uint32_t sensor_count() const { return hid_ ? hid_->sensor_count() : 0; }

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<MmioIshRegisters> mmio_;
  UniquePointer<IshIpc> ipc_;
  UniquePointer<IshtpBus> bus_;
  UniquePointer<IshHid> hid_;
  Outcome outcome_ = Outcome::NotStarted;
};

}  // namespace hardware
