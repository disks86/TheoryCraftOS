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
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// The GPIO lines of an I2C bus of the card, from the GPIO_I2C_Info data table of the video BIOS: a port of amdgpu_atombios_lookup_i2c_gpio and
// amdgpu_atombios_get_bus_rec_for_i2c_gpio (Copyright Advanced Micro Devices, Inc.; MIT license). Each entry names, for the clock and the data line, the registers (dword
// indexes of the card's register space) and bits that mask the pad to software, enable the output (pull the line low), read the line back (Y) and set the output level (A).
// Entries are 27 bytes; the table runs as far as its header says. Reads are checked; a table that runs past the image ends the search.
class AtomI2c {
 public:
  struct Bus {
    bool valid = false;               // the entry was found and has a clock mask register
    bool hw_capable = false;          // the id says the card's hardware I2C engine can do it (bit 7); the lines can still be driven by software
    bool mm_i2c = false;              // the multimedia engine's line (id 0xa0)
    uint8_t id = 0;
    uint32_t mask_clk_reg = 0, mask_data_reg = 0, en_clk_reg = 0, en_data_reg = 0, y_clk_reg = 0, y_data_reg = 0, a_clk_reg = 0, a_data_reg = 0;
    uint32_t mask_clk_mask = 0, mask_data_mask = 0, en_clk_mask = 0, en_data_mask = 0, y_clk_mask = 0, y_data_mask = 0, a_clk_mask = 0, a_data_mask = 0;
  };
  // The entry with this id (ATOM_I2C_ID_CONFIG_ACCESS.ucAccess), or an invalid Bus if there is none.
  static Bus lookup(ConstByteView bios, uint8_t id);
};

}  // namespace hardware
