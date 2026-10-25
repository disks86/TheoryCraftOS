/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

#include "Arch/AArch64/Spcr.hpp"
#include "Utility/ByteStream.hpp"

namespace arch_aarch64 {

namespace {
constexpr uint32_t kGasOffset = 40;          // SPCR: Base Address (Generic Address Structure)
constexpr uint8_t kSpaceSystemMemory = 0;
constexpr uint8_t kTypePl011 = 3;            // Interface Type: ARM PL011
constexpr uint8_t kTypeSbsa = 14;            // ARM SBSA generic UART (PL011 register layout)
constexpr uint8_t kTypeSbsa32 = 13;
}  // namespace

uint64_t spcr_uart_base(const hardware::AcpiTables& acpi) {
  const ConstByteView table = acpi.find("SPCR");
  if (table.size() < kGasOffset + 12) return 0;
  const uint8_t type = table[36];  // after the 36-byte header: Interface Type
  if (type != kTypePl011 && type != kTypeSbsa && type != kTypeSbsa32) return 0;
  ByteReader r(table.subview(kGasOffset, 12));
  const uint8_t space = r.u8();
  r.skip(3);  // bit width, bit offset, access size
  const uint64_t address = r.le64();
  if (!r.ok()) return 0;
  return space == kSpaceSystemMemory ? address : 0;
}

}  // namespace arch_aarch64
