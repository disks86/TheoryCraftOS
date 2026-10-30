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
#include "Hardware/Acpi/AcpiEvents.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"

namespace hardware {

// The machine's I/O ports for the ACPI event registers.
// Blocks at memory addresses are mapped through `mapper` (nullptr: none; then they read as zero and ignore writes), a page at a time, up to four pages.
class SystemAcpiPorts final : public IAcpiPorts {
 public:
  explicit SystemAcpiPorts(IRegisterMapper* mapper = nullptr) : mapper_(mapper) {}
  uint8_t mem_in8(uint64_t address) override;
  void mem_out8(uint64_t address, uint8_t value) override;
  uint16_t mem_in16(uint64_t address) override;
  void mem_out16(uint64_t address, uint16_t value) override;
  uint8_t in8(uint16_t port) override;
  void out8(uint16_t port, uint8_t value) override;
  uint16_t in16(uint16_t port) override;
  void out16(uint16_t port, uint16_t value) override;
  void delay_us(uint32_t us) override;

 private:
  const MmioRegion* window(uint64_t address, uint32_t size);
  IRegisterMapper* mapper_;
  MmioRegion windows_[4];
  uint32_t window_count_ = 0;
};

}  // namespace hardware
