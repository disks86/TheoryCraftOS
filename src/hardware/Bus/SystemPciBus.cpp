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

#include "hardware/Bus/SystemPciBus.hpp"
#include <new>

namespace hardware {

SystemPciBus::SystemPciBus(efi::SystemTable* st, allocators::IAllocator& allocator) : acpi_(st) {
  if (acpi_.initialize()) {
    auto* ecam = new (storage_) EcamPcieBus(acpi_, allocator);
    if (ecam->initialize()) {
      active_ = ecam;
      pcie_ = ecam;
      return;
    }
    ecam->~EcamPcieBus();  // no MCFG table: fall back below
  }
  auto* legacy = new (storage_) LegacyPciBus(allocator);
  legacy->initialize();
  active_ = legacy;
}

SystemPciBus::~SystemPciBus() {
  if (pcie_) static_cast<EcamPcieBus*>(pcie_)->~EcamPcieBus();
  else static_cast<LegacyPciBus*>(active_)->~LegacyPciBus();
}

}  // namespace hardware
