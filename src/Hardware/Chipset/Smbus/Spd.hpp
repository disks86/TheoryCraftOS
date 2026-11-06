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

// Serial presence detect: the EEPROM (DDR3), EE1004 EEPROM (DDR4) or SPD5118 hub (DDR5) on every memory module, read over the chipset's SMBus at
// 0x50..0x57. The decoder covers the fields JEDEC fixes for all three (JESD21-C annexes K, L and the DDR5 SPD contents): memory type, module form,
// capacity, rank and device organisation, the fastest clock and the module maker.
#pragma once
#include <stdint.h>
#include "Hardware/Chipset/Smbus/ISmbusController.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

enum class MemoryType : uint8_t { Unknown, Ddr3, Ddr4, Ddr5, Lpddr5 };
enum class MemoryModuleForm : uint8_t { Unknown, Rdimm, Udimm, SoDimm, Lrdimm, Other };

struct SpdModule {
  MemoryType type = MemoryType::Unknown;
  MemoryModuleForm form = MemoryModuleForm::Unknown;
  uint64_t capacity_bytes = 0;   // 0 when the organisation bytes were not available
  uint32_t speed_mts = 0;        // data rate at the fastest supported clock (MT/s), 0 when unknown
  uint8_t ranks = 0;             // logical ranks (3DS stacks count each die)
  uint8_t device_width = 0;      // SDRAM I/O width: 4, 8, 16 or 32
  bool ecc = false;
  uint16_t manufacturer = 0;     // JEP106: continuation count (with its parity bit) << 8 | code; 0 when not read
};

// An SPD image as far as it was read. Bytes past `size`, and bytes the reader skipped, are zero.
struct SpdImage {
  static constexpr uint16_t kCapacity = 1024;
  uint8_t bytes[kCapacity] = {};
  uint16_t size = 0;
  ConstByteView view() const { return ConstByteView(bytes, size); }
};

// Decodes an SPD image (byte 0 first). False if byte 2 (the memory type) is missing or not DDR3/DDR4/DDR5/LPDDR5. Fields whose bytes are missing
// or out of range stay 0/Unknown; nothing outside `spd` is read.
bool spd_decode(ConstByteView spd, SpdModule* out);

// Reads the bytes spd_decode needs from the module at SMBus address 0x50 + `slot` (0..7). DDR4 EE1004 pages and DDR5 hub pages are switched as
// needed and left as they were found. NoDevice when no module answers.
SmbusStatus spd_read(ISmbusController& smbus, uint8_t slot, SpdImage* image);

const char* memory_type_name(MemoryType type);
const char* memory_module_form_name(MemoryModuleForm form);
// The best-known JEP106 makers of memory modules and DRAM; nullptr for the rest.
const char* jedec_manufacturer_name(uint16_t code);
// "DDR5 SO-DIMM, 16 GiB, 5600 MT/s, 1 rank of x16, SK hynix".
void spd_describe(const SpdModule& module, FixedString<128>& out);

}  // namespace hardware
