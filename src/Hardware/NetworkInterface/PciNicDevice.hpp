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
#include "Hardware/Bus/Pci/PciMsi.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "uefi.hpp"

namespace hardware {

// A network controller on the PCI bus. Its registers are the first memory BAR (Intel parts have them in BAR 0; Realtek's BAR 0 is I/O ports and the memory
// window comes after it). The BAR is sized and mapped uncached through the register mapper when there is one; without one (paging not set up) the firmware's
// identity mapping is used, as the older drivers do.
class PciNicDevice final : public INicDevice {
 public:
  PciNicDevice(IPciBus& pci, const PciDeviceInfo& device, efi::SystemTable* st, IRegisterMapper* mapper)
      : pci_(pci), device_(device), st_(st), mapper_(mapper), msi_(pci) {}
  ~PciNicDevice() { detach_interrupt(); }

  bool claim() override;
  void release() override;
  INicRegisters& registers() override { return registers_; }
  uint16_t vendor_id() const override { return device_.vendor_id; }
  uint16_t device_id() const override { return device_.device_id; }
  uint32_t config_read32(uint16_t offset) override { return pci_.read32(device_.address, offset); }
  void config_write32(uint16_t offset, uint32_t value) override { pci_.write32(device_.address, offset, value); }
  // MSI to a vector of the network range on the core the scheduler picks for I/O interrupts (x86 only).
  bool attach_interrupt(InterruptHandler handler, void* context) override;
  void detach_interrupt() override;
  // For the interrupt entry: where the interrupt goes now.
  void interrupt_target(InterruptHandler* handler, void** context) const;

 private:
  IPciBus& pci_;
  PciDeviceInfo device_;
  efi::SystemTable* st_;
  IRegisterMapper* mapper_;
  MmioNicRegisters registers_;
  bool detached_ = false;
  PciMsi msi_;
  uint8_t vector_ = 0;
  int32_t io_device_ = -1;
  InterruptHandler handler_ = nullptr;   // read by the interrupt handler (atomic)
  void* handler_context_ = nullptr;
};

}  // namespace hardware
