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
#include "hardware/Bus/Pci/PciBusBase.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

// Conventional PCI through the legacy 0xCF8/0xCFC I/O ports. Works everywhere
// but only sees the first 256 bytes of each device's config space.
class LegacyPciBus final : public PciBusBase<IPciBus> {
 public:
  explicit LegacyPciBus(allocators::IAllocator& allocator) : PciBusBase(allocator) {}

  const char* name() const override { return "PCI bus (I/O ports)"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t read32(PciAddress address, uint16_t offset) const override;
  void write32(PciAddress address, uint16_t offset, uint32_t value) const override;

 private:
  static constexpr uint16_t kConfigAddress = 0xCF8;
  static constexpr uint16_t kConfigData = 0xCFC;

  IoPort address_port_{kConfigAddress};
  IoPort data_port_{kConfigData};
};

}  // namespace hardware
