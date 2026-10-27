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
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

// Message Signaled Interrupts (the MSI capability, id 0x05 - not MSI-X, see VirtioPciDevice for that) of a PCI function: one vector, delivered as a memory write of `data` to
// `address`. The caller says what the message means to its interrupt controller (on x86: 0xFEE00000 | apic_id << 12 and the vector number), this writes it into the capability and
// switches it on, following PCI Local Bus Specification 3.0 section 6.8.1: the message control register (bit 0 enable, 3:1 how many vectors the function asks for, 6:4 how many
// were given, bit 7 64-bit address, bit 8 per-vector masking), the address (and its upper half for a 64-bit function), the data word and the mask bits. Only one vector is used,
// so the number of vectors given is 0 (1). The command register's INTx disable bit is set while MSI is on, as Linux does, so the legacy pin is not also raised.
class PciMsi {
 public:
  static constexpr uint8_t kCapabilityId = 0x05;

  explicit PciMsi(IPciBus& bus) : bus_(bus) {}
  ~PciMsi() { disable(); }
  PciMsi(const PciMsi&) = delete;
  PciMsi& operator=(const PciMsi&) = delete;

  // Programs the message and enables MSI. False (nothing changed) if the function has no MSI capability, the address does not fit the function (above 4 GiB for one that is not
  // 64-bit capable, or not 4-byte aligned), or MSI would not switch on.
  bool enable(PciAddress function, uint64_t message_address, uint16_t message_data);
  // MSI off and the legacy pin back. Safe when not enabled.
  void disable();
  bool enabled() const { return capability_ != 0; }

 private:
  IPciBus& bus_;
  PciAddress function_{};
  uint16_t capability_ = 0;
};

}  // namespace hardware
