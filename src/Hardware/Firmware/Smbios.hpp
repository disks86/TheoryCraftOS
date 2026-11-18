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

// SMBIOS (DMTF DSP0134): what the firmware says the machine is - its maker and model, the BIOS, the board, the chassis, the processors and
// the memory modules. Found through the UEFI configuration table (the 64-bit "_SM3_" entry point first, else the 32-bit "_SM_"), read once
// and kept as plain strings.
//
// The parser works on views and checks everything: an entry point whose checksum, length or table address is wrong is refused; a structure
// whose formatted part runs past the table or whose string set has no double-NUL ends the walk; a string number past the set is an empty
// string; the walk stops at the end-of-table structure (type 127), at the table's length, or after kMaxStructures.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace efi { struct SystemTable; }

namespace hardware {

struct SmbiosInfo {
  static constexpr uint32_t kText = 64, kMaxMemory = 16;
  uint8_t major = 0, minor = 0;                    // the SMBIOS version
  // Type 0: BIOS.
  char bios_vendor[kText] = {}, bios_version[kText] = {}, bios_date[kText] = {};
  // Type 1: system.
  char manufacturer[kText] = {}, product[kText] = {}, version[kText] = {}, serial[kText] = {}, sku[kText] = {}, family[kText] = {};
  uint8_t uuid[16] = {};
  // Type 2: baseboard.
  char board_manufacturer[kText] = {}, board_product[kText] = {}, board_version[kText] = {};
  // Type 3: chassis.
  char chassis_manufacturer[kText] = {};
  uint8_t chassis_type = 0;                        // bits 6:0 (9 laptop, 10 notebook, 14 sub-notebook, 31 convertible, 32 detachable, 3 desktop ...)
  // Type 4: the first processor.
  char processor[kText] = {};
  uint16_t processor_max_mhz = 0;
  uint32_t processor_count = 0;
  // Type 17: memory devices (populated slots).
  struct Memory {
    char locator[24] = {}, manufacturer[24] = {}, part[32] = {};
    uint64_t size_mib = 0;
    uint16_t speed_mts = 0;                         // configured speed (or the rated one when that is unknown)
    uint8_t type = 0;                               // 0x1A DDR4, 0x22 DDR5, 0x1E LPDDR4, 0x23 LPDDR5 ...
  };
  Memory memory[kMaxMemory];
  uint32_t memory_count = 0;
  uint64_t memory_total_mib = 0;
  uint32_t structures = 0;                          // how many were read
  bool truncated = false;                           // the walk stopped early (a malformed structure, or kMaxStructures)

  bool is_portable() const;                         // the chassis says laptop, notebook, convertible ...
};

// Parses an entry point (64- or 32-bit) and gives the table's physical address and length. False if it is neither or does not check out.
[[nodiscard]] bool parse_smbios_entry(ConstByteView entry, uint64_t* table_address, uint32_t* table_length, uint8_t* major, uint8_t* minor);
// Parses the structure table into `out` (which keeps the version it was given). False if not even one structure could be read.
[[nodiscard]] bool parse_smbios_table(ConstByteView table, SmbiosInfo* out);
// The readable name of a memory type (Type 17) or chassis type (Type 3).
const char* smbios_memory_type_name(uint8_t type);
const char* smbios_chassis_type_name(uint8_t type);

// The machine's SMBIOS, read from the firmware's configuration table once (at start-up, while the firmware's memory is still mapped as it
// left it) and kept. Readers may call system_smbios() from any thread after that.
[[nodiscard]] bool read_system_smbios(efi::SystemTable* st);
const SmbiosInfo* system_smbios();   // null if read_system_smbios() did not succeed

}  // namespace hardware
