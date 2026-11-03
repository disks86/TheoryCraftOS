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

#include "Arch/LoongArch64/Uart.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Utility/ByteStream.hpp"

namespace arch_loongarch64 {

namespace {
constexpr uint32_t kGasOffset = 40;          // SPCR: Base Address (Generic Address Structure)
constexpr uint8_t kSpaceSystemMemory = 0;
constexpr uint8_t kType16550 = 0;            // Interface Type: fully 16550-compatible
constexpr uint8_t kType16550Subset = 1;      // 16550 subset (DBGP revision 1)
}  // namespace

uint64_t spcr_uart_base(const hardware::AcpiTables& acpi) {
  const ConstByteView table = acpi.find("SPCR");
  if (table.size() < kGasOffset + 12) return 0;
  const uint8_t type = table[36];  // after the 36-byte header: Interface Type
  if (type != kType16550 && type != kType16550Subset) return 0;
  ByteReader r(table.subview(kGasOffset, 12));
  const uint8_t space = r.u8();
  r.skip(3);  // bit width, bit offset, access size
  const uint64_t address = r.le64();
  if (!r.ok()) return 0;
  return space == kSpaceSystemMemory ? address : 0;
}

}  // namespace arch_loongarch64

namespace hardware {
void DebugConsole::put(char c) const {
  if (!enabled_ || !base_) return;
  auto* thr = reinterpret_cast<volatile uint8_t*>(base_);          // transmit holding register
  auto* lsr = reinterpret_cast<volatile uint8_t*>(base_ + 5);      // line status register
  constexpr uint8_t kThrEmpty = 1u << 5;
  for (uint32_t spin = 0; !(*lsr & kThrEmpty) && spin < 100000; ++spin) {}
  *thr = static_cast<uint8_t>(c);
}
}  // namespace hardware
