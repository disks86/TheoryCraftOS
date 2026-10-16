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
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Pci/LegacyPciBus.hpp"
#include "Hardware/Bus/Pcie/EcamPcieBus.hpp"

namespace hardware {

constexpr unsigned long BusMaxOf(unsigned long a, unsigned long b) { return a > b ? a : b; }

// Picks the most capable PCI access method the platform offers: PCI Express
// through ECAM when ACPI provides an MCFG table, else conventional PCI through
// I/O ports. The bus is initialized (scanned) on return.
class SystemPciBus {
 public:
  // The device list is allocated from `allocator`, which must outlive this object.
  SystemPciBus(efi::SystemTable* st, allocators::IAllocator& allocator);
  ~SystemPciBus();
  SystemPciBus(const SystemPciBus&) = delete;
  SystemPciBus& operator=(const SystemPciBus&) = delete;

  IPciBus& pci() { return *active_; }
  // Non-null only when the PCI Express (ECAM) bus is in use.
  IPcieBus* pcie() { return pcie_; }

 private:
  AcpiTables acpi_;
  alignas(16) unsigned char storage_[BusMaxOf(sizeof(LegacyPciBus), sizeof(EcamPcieBus))];
  IPciBus* active_ = nullptr;
  IPcieBus* pcie_ = nullptr;
};

}  // namespace hardware
