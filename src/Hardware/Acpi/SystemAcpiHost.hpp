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
#include "Hardware/Acpi/AcpiHost.hpp"
#include "Hardware/Acpi/EmbeddedController.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/MmioMapper.hpp"

namespace hardware {

// The real machine behind AML's operation regions: physical memory (mapped through `mmio` where the firmware did not), I/O ports, PCI configuration space
// (through `pci`) and the embedded controller. Either of `pci` and `mmio` may be nullptr; the regions that need it then fail.
class SystemAcpiHost final : public IAcpiHost {
 public:
  SystemAcpiHost(const IPciBus* pci, MmioMapper* mmio) : pci_(pci), mmio_(mmio), ports_(), ec_(ports_) {}

  void configure_ec(uint16_t data_port, uint16_t command_port) override { ec_.configure(data_port, command_port); }
  bool ec_status(uint8_t* value) override { return ec_.status(value); }
  bool ec_query(uint8_t* event) override { return ec_.query(event); }
  bool region_read(const acpi::RegionAccess& access, uint64_t* value) override;
  bool region_write(const acpi::RegionAccess& access, uint64_t value) override;
  void stall_us(uint32_t us) override;
  void sleep_ms(uint32_t ms) override;
  uint64_t timer_100ns() override;

 private:
  struct Ports final : IEcPorts {
    uint8_t in8(uint16_t port) override;
    void out8(uint16_t port, uint8_t value) override;
    void delay_us(uint32_t us) override;
  };
  bool memory(uint64_t address, uint32_t bits, uint64_t* value, bool write);

  const IPciBus* pci_;
  MmioMapper* mmio_;
  Ports ports_;
  EmbeddedController ec_;
};

}  // namespace hardware
