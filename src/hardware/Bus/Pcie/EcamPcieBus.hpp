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
#include "hardware/Acpi/AcpiTables.hpp"
#include "hardware/Bus/Pci/PciBusBase.hpp"
#include "hardware/Bus/Pcie/IPcieBus.hpp"

namespace hardware {

// PCI Express through ECAM: config space is memory-mapped, at an address the
// ACPI MCFG table provides. Only PCI segment 0 is used.
class EcamPcieBus final : public PciBusBase<IPcieBus> {
 public:
  EcamPcieBus(const AcpiTables& acpi, allocators::IAllocator& allocator)
      : PciBusBase(allocator), acpi_(acpi) {}

  const char* name() const override { return "PCI Express bus (ECAM)"; }
  bool initialize() override;  // false if firmware has no MCFG table
  void shutdown() override { base_ = 0; }

  uint32_t read32(PciAddress address, uint16_t offset) const override;
  void write32(PciAddress address, uint16_t offset, uint32_t value) const override;
  uint16_t find_extended_capability(PciAddress address, uint16_t id) const override;

  uint64_t ecam_base() const { return base_; }

 private:
  volatile uint32_t* locate(PciAddress address, uint16_t offset) const;

  const AcpiTables& acpi_;
  uint64_t base_ = 0;  // ECAM address of bus first_bus_
  uint32_t first_bus_ = 0, last_bus_ = 0;
};

}  // namespace hardware
