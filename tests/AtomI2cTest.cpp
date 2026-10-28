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

// AtomI2c against hand-assembled video BIOS images: the GPIO_I2C_Info table (data table 10) with 27-byte entries at literal offsets, the registers and shifts of each entry, the id
// bits, the entries that are not usable, and every truncation and bit flip under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
// ROM header at 0x100, master data table at 0x200 (GPIO_I2C_Info, entry 10, at 0x218 -> 0x300), the table at 0x300.
struct Img {
  std::vector<uint8_t> b = std::vector<uint8_t>(0x600, 0);
  uint32_t count = 0;
  void u8(uint32_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v); }
  void u16(uint32_t at, uint32_t v) { u8(at, v); u8(at + 1, v >> 8); }
  Img() {
    u8(0, 0x55); u8(1, 0xAA);
    u16(0x48, 0x100);
    memcpy(&b[0x104], "ATOM", 4);
    u16(0x120, 0x200);
    u16(0x200 + 4 + 10 * 2, 0x300);
    u16(0x300, 4);
  }
  // clk: mask, en, y, a registers; data: mask, en, y, a; id; shifts clk mask, en, y, a, data mask, en, y, a
  void entry(uint32_t id, const uint32_t regs[8], const uint8_t shifts[8]) {
    const uint32_t at = 0x304 + count * 27;
    for (int i = 0; i < 8; ++i) u16(at + i * 2, regs[i]);
    u8(at + 16, id);
    for (int i = 0; i < 8; ++i) u8(at + 17 + i, shifts[i]);
    u8(at + 25, 0xEE); u8(at + 26, 0xEE);
    ++count;
    u16(0x300, 4 + count * 27);
  }
  ConstByteView view() const { return ConstByteView(b.data(), b.size()); }
};

Img board() {
  Img i;
  const uint32_t r1[8] = {0x1A00, 0x1A01, 0x1A02, 0x1A03, 0x1A04, 0x1A05, 0x1A06, 0x1A07};
  const uint8_t s1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  i.entry(0x91, r1, s1);                      // id 0x91: hardware capable bit set
  const uint32_t r2[8] = {0x1B00, 0x1B01, 0x1B02, 0x1B03, 0x1B04, 0x1B05, 0x1B06, 0x1B07};
  const uint8_t s2[8] = {9, 10, 11, 12, 13, 14, 15, 16};
  i.entry(0x12, r2, s2);                      // a software line
  const uint32_t r3[8] = {0x1C00, 0, 0, 0, 0x1C04, 0, 0, 0};
  const uint8_t s3[8] = {0, 0, 0, 0, 31, 0, 0, 0};
  i.entry(0xa0, r3, s3);                      // the multimedia line
  const uint32_t r4[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  const uint8_t s4[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  i.entry(0x33, r4, s4);                      // no clock mask register: found but not valid
  const uint32_t r5[8] = {0x1D00, 0x1D01, 0x1D02, 0x1D03, 0x1D04, 0x1D05, 0x1D06, 0x1D07};
  const uint8_t s5[8] = {32, 33, 255, 0, 0, 0, 0, 0};
  i.entry(0x44, r5, s5);                      // shifts the driver would make undefined: no bit
  return i;
}
}  // namespace

static void test_entries() {
  Img i = board();
  AtomI2c::Bus b = AtomI2c::lookup(i.view(), 0x91);
  CHECK(b.valid && b.hw_capable && !b.mm_i2c && b.id == 0x91);
  CHECK(b.mask_clk_reg == 0x1A00 && b.en_clk_reg == 0x1A01 && b.y_clk_reg == 0x1A02 && b.a_clk_reg == 0x1A03);
  CHECK(b.mask_data_reg == 0x1A04 && b.en_data_reg == 0x1A05 && b.y_data_reg == 0x1A06 && b.a_data_reg == 0x1A07);
  CHECK(b.mask_clk_mask == 1u << 1 && b.en_clk_mask == 1u << 2 && b.y_clk_mask == 1u << 3 && b.a_clk_mask == 1u << 4);
  CHECK(b.mask_data_mask == 1u << 5 && b.en_data_mask == 1u << 6 && b.y_data_mask == 1u << 7 && b.a_data_mask == 1u << 8);
  b = AtomI2c::lookup(i.view(), 0x12);
  CHECK(b.valid && !b.hw_capable && !b.mm_i2c && b.id == 0x12 && b.mask_clk_reg == 0x1B00 && b.a_data_reg == 0x1B07);
  CHECK(b.mask_clk_mask == 1u << 9 && b.a_data_mask == 1u << 16);
  b = AtomI2c::lookup(i.view(), 0xa0);
  CHECK(b.valid && b.hw_capable && b.mm_i2c && b.mask_data_mask == 1u << 31 && b.mask_clk_mask == 1);
  b = AtomI2c::lookup(i.view(), 0x33);
  CHECK(!b.valid && b.id == 0x33 && b.en_clk_reg == 1);
  b = AtomI2c::lookup(i.view(), 0x44);
  CHECK(b.valid && b.mask_clk_mask == 0 && b.en_clk_mask == 0 && b.y_clk_mask == 0 && b.a_clk_mask == 1);
  // not in the table
  b = AtomI2c::lookup(i.view(), 0x55);
  CHECK(!b.valid && b.id == 0 && b.mask_clk_reg == 0);
  // the first of two entries with one id wins
  Img twice = board();
  const uint32_t rr[8] = {0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007};
  const uint8_t ss[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  twice.entry(0x91, rr, ss);
  CHECK(AtomI2c::lookup(twice.view(), 0x91).mask_clk_reg == 0x1A00);
  // an entry beyond the table's size is not looked at
  Img cut = board();
  cut.u16(0x300, 4 + 3 * 27);
  CHECK(!AtomI2c::lookup(cut.view(), 0x44).valid && AtomI2c::lookup(cut.view(), 0xa0).valid);
  cut.u16(0x300, 3);   // (smaller than its header)
  CHECK(!AtomI2c::lookup(cut.view(), 0x91).valid);
  cut.u16(0x300, 4 + 27 - 1);
  CHECK(!AtomI2c::lookup(cut.view(), 0x91).valid);
}

static void test_missing() {
  Img none = board();
  none.u16(0x200 + 4 + 10 * 2, 0);   // no GPIO_I2C_Info table
  CHECK(!AtomI2c::lookup(none.view(), 0x91).valid);
  Img bad = board();
  bad.b[0] = 0;
  CHECK(!AtomI2c::lookup(bad.view(), 0x91).valid);
  bad = board();
  bad.b[0x104] = 'X';
  CHECK(!AtomI2c::lookup(bad.view(), 0x91).valid);
  CHECK(!AtomI2c::lookup(ConstByteView(), 0x91).valid);
}

static void test_damage() {
  const Img good = board();
  for (size_t len = 0; len <= good.b.size(); ++len) {
    const AtomI2c::Bus b = AtomI2c::lookup(ConstByteView(good.b.data(), len), 0x44);
    // (the fifth entry starts at 0x370 and its last field is the shift at +24: whole from 905 bytes on, and not before)
    if (b.valid != (len >= 0x370 + 25)) { printf("  truncated to %zu\n", len); CHECK(false); break; }
  }
  Img flipped = good;
  for (size_t at = 0; at < flipped.b.size(); at += 1) {
    for (int bit = 0; bit < 8; ++bit) {
      flipped.b[at] ^= static_cast<uint8_t>(1 << bit);
      AtomI2c::lookup(flipped.view(), 0x91);
      AtomI2c::lookup(flipped.view(), 0x44);
      flipped.b[at] ^= static_cast<uint8_t>(1 << bit);
    }
  }
}

int main() {
  test_entries();
  test_missing();
  test_damage();
  printf(g_failed ? "AtomI2cTest: %d FAILED\n" : "AtomI2cTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
