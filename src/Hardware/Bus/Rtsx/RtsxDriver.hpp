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

// The PCI binding of the Realtek card reader (RTS525A, PCI 10EC:525A, the one in the Dell XPS 15 9575): memory decoding and bus mastering on, the function's INTx masked (the driver
// polls; an unserved interrupt line must not storm), BAR 0 mapped and the SD slot brought up (RtsxHost). The host is then the slot for an SdSlotSource.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Rtsx/RtsxHost.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class MmioRtsxRegisters final : public IRtsxRegisters {
 public:
  explicit MmioRtsxRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

class RtsxDriver {
 public:
  enum class Outcome : uint8_t { NotStarted, Started, UnknownDevice, BarUnmapped, OutOfMemory, ChipFailed };
  static constexpr uint32_t kRegisterBytes = 0x1000;
  explicit RtsxDriver(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~RtsxDriver() { stop(); }

  static bool supports(const PciDeviceInfo& device) { return device.vendor_id == 0x10EC && device.device_id == 0x525A; }
  static const char* outcome_name(Outcome outcome);
  Outcome start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper);
  Outcome start_registers(IRtsxRegisters& registers);
  void stop();
  RtsxHost* host() { return host_.get(); }
  Outcome outcome() const { return outcome_; }

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<MmioRtsxRegisters> mmio_;
  UniquePointer<RtsxHost> host_;
  Outcome outcome_ = Outcome::NotStarted;
};

}  // namespace hardware
