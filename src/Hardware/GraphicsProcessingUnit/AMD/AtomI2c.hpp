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

  // ATOM firmware images (data table format 2.1: Vega and later) have no GPIO_I2C_Info table: the I2C record's id names an entry of the gpio_pin_lut table
  // (atom_gpio_pin_lut_v2_1, 8-byte atom_gpio_pin_assignment entries, content revision 1), the first one whose hardware capable bit, engine id and lane mux agree with the id
  // (bios_parser2.c's get_gpio_i2c_info). The entry gives the clock pad's A register (an absolute register number) and its bit; the display engine's own table says which
  // DDC line that is (amd_dce12::ddc_line_of) and where the line's other registers are. found is false if the image has no such table or entry.
  struct PinV21 {
    bool found = false;
    uint32_t a_register = 0;
    uint8_t shift = 0;
    uint8_t gpio_id = 0;
  };
  static PinV21 lookup_v21(ConstByteView bios, uint8_t i2c_id);
};

}  // namespace hardware
