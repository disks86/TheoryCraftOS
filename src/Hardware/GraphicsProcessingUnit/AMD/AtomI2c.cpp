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

#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): amdgpu_atombios.c (amdgpu_atombios_lookup_i2c_gpio,
// amdgpu_atombios_get_bus_rec_for_i2c_gpio) and atombios.h (ATOM_GPIO_I2C_ASSIGMENT). scripts/check_i2c_port.py checks the numbers against them.

namespace hardware {

namespace {
constexpr uint32_t kBiosMagic = 0xAA55, kRomTablePtr = 0x48, kRomMagicPtr = 4, kRomDataPtr = 0x20;
constexpr uint8_t kRomMagic[4] = {'A', 'T', 'O', 'M'};
constexpr uint32_t kDataGpioI2cInfo = 10;          // index in the master list of data tables
constexpr uint32_t kTableHeaderBytes = 4;          // ATOM_COMMON_TABLE_HEADER
constexpr uint32_t kAssignmentBytes = 27;          // sizeof(ATOM_GPIO_I2C_ASSIGMENT)
constexpr uint32_t kIdAt = 16, kShiftsAt = 17;     // sucI2cId, then the eight shifts
constexpr uint8_t kHwCapable = 0x80, kMmI2cId = 0xa0;

struct Reader {
  ConstByteView bytes;
  bool u8(uint64_t at, uint8_t* out) const { return bytes.try_get(at, out); }
  bool u16(uint64_t at, uint32_t* out) const {
    uint8_t lo = 0, hi = 0;
    if (at > UINT64_MAX - 1 || !bytes.try_get(at, &lo) || !bytes.try_get(at + 1, &hi)) return false;
    *out = static_cast<uint32_t>(lo | (hi << 8));
    return true;
  }
};

// 1 << shift as the driver computes it (an int: a shift of 32 or more is undefined there; the line is then taken to have no bit)
uint32_t bit(uint8_t shift) { return shift < 32 ? 1u << shift : 0; }
}  // namespace

AtomI2c::Bus AtomI2c::lookup(ConstByteView bios, uint8_t id) {
  Bus none;
  const Reader r{bios};
  uint32_t magic = 0, rom_table = 0, data_table = 0;
  if (!r.u16(0, &magic) || magic != kBiosMagic || !r.u16(kRomTablePtr, &rom_table)) return none;
  for (uint32_t i = 0; i < sizeof kRomMagic; ++i) {
    uint8_t c = 0;
    if (!r.u8(rom_table + kRomMagicPtr + i, &c) || c != kRomMagic[i]) return none;
  }
  uint32_t table = 0, size = 0;
  if (!r.u16(rom_table + kRomDataPtr, &data_table) || !r.u16(data_table + kDataGpioI2cInfo * 2 + 4, &table) || table == 0 || !r.u16(table, &size)) return none;
  const uint32_t entries = size >= kTableHeaderBytes ? (size - kTableHeaderBytes) / kAssignmentBytes : 0;
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t at = table + kTableHeaderBytes + i * kAssignmentBytes;
    uint8_t entry_id = 0;
    if (!r.u8(at + kIdAt, &entry_id)) return none;
    if (entry_id != id) continue;
    uint32_t reg[8] = {};
    uint8_t shift[8] = {};
    for (uint32_t k = 0; k < 8; ++k)
      if (!r.u16(at + k * 2, &reg[k]) || !r.u8(at + kShiftsAt + k, &shift[k])) return none;
    Bus bus;
    bus.mask_clk_reg = reg[0];
    bus.en_clk_reg = reg[1];
    bus.y_clk_reg = reg[2];
    bus.a_clk_reg = reg[3];
    bus.mask_data_reg = reg[4];
    bus.en_data_reg = reg[5];
    bus.y_data_reg = reg[6];
    bus.a_data_reg = reg[7];
    bus.mask_clk_mask = bit(shift[0]);
    bus.en_clk_mask = bit(shift[1]);
    bus.y_clk_mask = bit(shift[2]);
    bus.a_clk_mask = bit(shift[3]);
    bus.mask_data_mask = bit(shift[4]);
    bus.en_data_mask = bit(shift[5]);
    bus.y_data_mask = bit(shift[6]);
    bus.a_data_mask = bit(shift[7]);
    bus.hw_capable = (entry_id & kHwCapable) != 0;
    bus.mm_i2c = entry_id == kMmI2cId;
    bus.id = entry_id;
    bus.valid = bus.mask_clk_reg != 0;
    return bus;
  }
  return none;
}

}  // namespace hardware
