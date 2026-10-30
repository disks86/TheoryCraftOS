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

// MSI-X (the capability with id 0x11) of a PCI function: a table of vectors, each a message (address and data) written to the interrupt controller when that source asks. The table lives in
// one of the function's BARs and is reached through the register mapper. Following PCI Local Bus Specification 3.0 section 6.8.2: message control (bit 15 enable, bit 14 function mask, bits
// 10:0 the table size less one), the table's BAR and offset, and per entry the message address (low, high), the data and the vector control (bit 0 masks the entry). Entries are written
// masked, with the whole function masked while MSI-X is switched on; the caller says which vector (on x86 the interrupt vector number) each entry delivers, and to which core.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"

namespace hardware {

class PciMsix {
 public:
  static constexpr uint8_t kCapabilityId = 0x11;
  static constexpr uint32_t kMaxEntries = 2048;

  PciMsix(IPciBus& bus, IRegisterMapper& mapper) : bus_(bus), mapper_(mapper) {}
  ~PciMsix() { disable(); }
  PciMsix(const PciMsix&) = delete;
  PciMsix& operator=(const PciMsix&) = delete;

  // How many entries the function's table has (0 without the capability).
  uint32_t table_size(PciAddress function);
  // Switches MSI-X on with entry i delivering `vectors[i]` (i < `count`) to the core with local APIC id `apic_id` (physical destination, edge triggered, fixed delivery). Entries beyond
  // `count` stay masked. False, with nothing changed, if there is no capability, the table is smaller than `count`, the table's BAR is not assigned or cannot be mapped, or the
  // core's id does not fit.
  bool enable(PciAddress function, const uint8_t* vectors, uint32_t count, uint32_t apic_id);
  // MSI-X off (every entry masked first). Safe when not enabled.
  void disable();
  bool enabled() const { return capability_ != 0; }

 private:
  IPciBus& bus_;
  IRegisterMapper& mapper_;
  PciAddress function_{};
  uint16_t capability_ = 0;
  MmioRegion table_;
  uint32_t entries_ = 0;
};

}  // namespace hardware
