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

// NVIDIA PLL programming (Nvkm/NvPll): the BIOS PLL limits table in each version nvbios_pll_parse knows (0x10/0x11 and 0x20/0x21 through
// the chip's type <-> register map, 0x30, 0x40, 0x50, none at all), the nv04 and gt215 calculators against coefficients worked out by
// hand from nouveau's algorithms (exact, nearest, boundary and unreachable frequencies), the register sequences of the nv50, gt215, gf100
// and gv100 pll_set on a register model, NvPllInitHooks under the init-script interpreter, and the hostile cases: the image cut at every
// length and random bit flips of the tables (no crash under ASan/UBSan, no write outside BAR0).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInitScript.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvPll.hpp"

using namespace hardware;
using namespace hardware::nv;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "NvBiosImages.hpp"

struct FakeRegs final : INvRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint32_t read32(uint32_t offset) override {
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint32_t offset, uint32_t value) override {
    regs[offset] = value;
    writes.push_back({offset, value});
  }
};

struct InnerHooks final : INvInitHooks {
  std::vector<uint8_t> gpio;
  bool gpio_reset(uint8_t function) override { gpio.push_back(function); return true; }
};

struct Trace final : INvInitTrace {
  std::vector<NvInitWrite> all;
  void on_write(const NvInitWrite& w) override { all.push_back(w); }
};

static const Chip kKepler = chip_of(0x0e4000a2);   // GK104
static const Chip kG84 = chip_of(0x084000a2);
static const Chip kNv50 = chip_of(0x050000a2);
static const Chip kGt215 = chip_of(0x0a3000a2);
static const Chip kGv100 = chip_of(0x140000a1);

constexpr size_t kBit = 0x100;
constexpr uint32_t kTable = 0x1000;

// The Kepler image with a BIT 'C' entry (version 2: a 32-bit pointer at +0) to a table at kTable.
static Bytes image_with_c(uint8_t c_version = 2) {
  Bytes v = kepler_image(false);
  if (v.size() < 0x4000) v.resize(0x4000, 0);
  const uint16_t c = 0x0f00;
  add_bit_entry(v, kBit, 'C', c_version, c_version == 1 ? 10 : 4, c);
  if (c_version == 1) put16(v, c + 8, kTable);
  else put32(v, c, kTable);
  return v;
}

// --- Table versions ----------------------------------------------------------------------------------------------------------------------

// A 0x40 table: header 5, entries of 0x10 bytes (type +0, limits pointer +1, register +3, reference clock MHz +9).
struct Limits40 {
  uint8_t type;
  uint32_t reg;
  uint16_t ref_mhz, vco_min, vco_max, in_min, in_max;
  uint8_t min_m, max_m, min_n, max_n, min_p, max_p;
};
static void table40(Bytes& v, const Limits40* e, uint8_t count, uint8_t version = 0x40) {
  put8(v, kTable, version); put8(v, kTable + 1, 5); put8(v, kTable + 2, 0x10); put8(v, kTable + 3, count);
  for (uint8_t i = 0; i < count; ++i) {
    const size_t at = kTable + 5 + i * 0x10, lim = 0x1400 + i * 0x20;
    put8(v, at, e[i].type); put16(v, at + 1, lim); put32(v, at + 3, e[i].reg); put16(v, at + 9, e[i].ref_mhz);
    put16(v, lim + 0, e[i].vco_min); put16(v, lim + 2, e[i].vco_max); put16(v, lim + 4, e[i].in_min); put16(v, lim + 6, e[i].in_max);
    put8(v, lim + 8, e[i].min_m); put8(v, lim + 9, e[i].max_m); put8(v, lim + 10, e[i].min_n); put8(v, lim + 11, e[i].max_n);
    put8(v, lim + 12, e[i].min_p); put8(v, lim + 13, e[i].max_p);
  }
}
// The VPLL every gt215-style example below uses: 27 MHz, VCO 400-800 MHz, input 13-27 MHz, M 1-4, N 8-255, P 1-6.
static const Limits40 kVpll0 = {0x80, 0x00612050, 27, 400, 800, 13, 27, 1, 4, 8, 255, 1, 6};
static const Limits40 kVpll1 = {0x81, 0x00612850, 27, 400, 800, 13, 27, 1, 4, 8, 255, 1, 6};
static const Limits40 kCore40 = {0x01, 0x00137000, 27, 400, 800, 13, 27, 1, 4, 8, 255, 1, 6};

static void test_version_40() {
  Bytes v = image_with_c();
  const Limits40 e[3] = {kCore40, kVpll0, kVpll1};
  table40(v, e, 3);
  NvBios bios(view(v));
  NvPllTable t;
  bool truncated = true;
  CHECK(find_pll_table(bios, &t, &truncated) && !truncated && t.offset == kTable && t.version == 0x40 && t.count == 3 && t.length == 0x10);
  NvPllLimits l;
  CHECK(parse_pll_limits(bios, kKepler, 25000, kPllVpll1, &l) == NvPllError::None);
  CHECK(l.type == kPllVpll1 && l.reg == 0x00612850 && l.refclk == 27000 && l.table_version == 0x40);
  CHECK(l.vco1.min_freq == 400000 && l.vco1.max_freq == 800000 && l.vco1.min_inputfreq == 13000 && l.vco1.max_inputfreq == 27000);
  CHECK(l.vco1.min_m == 1 && l.vco1.max_m == 4 && l.vco1.min_n == 8 && l.vco1.max_n == 255 && l.min_p == 1 && l.max_p == 6);
  CHECK(l.vco2.max_freq == 0);
  // by register
  CHECK(parse_pll_limits(bios, kKepler, 25000, 0x00612050, &l) == NvPllError::None && l.type == kPllVpll0 && l.reg == 0x00612050);
  // absent type / register: -ENOENT
  CHECK(parse_pll_limits(bios, kKepler, 25000, kPllMemory, &l) == NvPllError::NotFound);
  CHECK(parse_pll_limits(bios, kKepler, 25000, 0x00123456, &l) == NvPllError::NotFound);
  // a reference clock of 0 in the table: the crystal
  put16(v, kTable + 5 + 9, 0);
  NvBios b2(view(v));
  CHECK(parse_pll_limits(b2, kKepler, 25000, kPllCore, &l) == NvPllError::None && l.refclk == 25000);
  // an unknown version
  put8(v, kTable, 0x60);
  NvBios b3(view(v));
  CHECK(parse_pll_limits(b3, kKepler, 25000, kPllCore, &l) == NvPllError::UnknownVersion);
}

static void test_version_30_and_50() {
  Bytes v = image_with_c(1);   // (and BIT 'C' version 1: a 16-bit pointer at +8)
  // 0x30: header 4, entries of 7 (type, limits pointer, register).
  put8(v, kTable, 0x30); put8(v, kTable + 1, 4); put8(v, kTable + 2, 7); put8(v, kTable + 3, 1);
  put8(v, kTable + 4, kPllMemory); put16(v, kTable + 5, 0x1400); put32(v, kTable + 7, 0x00004008);
  const size_t l = 0x1400;
  put16(v, l + 0, 500); put16(v, l + 2, 1000); put16(v, l + 4, 400); put16(v, l + 6, 1000);
  put16(v, l + 8, 2); put16(v, l + 10, 20); put16(v, l + 12, 50); put16(v, l + 14, 400);
  put8(v, l + 16, 1); put8(v, l + 17, 255); put8(v, l + 18, 1); put8(v, l + 19, 13);
  put8(v, l + 20, 1); put8(v, l + 21, 31); put8(v, l + 22, 1); put8(v, l + 23, 4);
  put8(v, l + 25, 6); put8(v, l + 27, 1); put32(v, l + 28, 27000);
  NvBios bios(view(v));
  NvPllLimits p;
  CHECK(parse_pll_limits(bios, kG84, 25000, kPllMemory, &p) == NvPllError::None);
  CHECK(p.reg == 0x4008 && p.refclk == 27000 && p.table_version == 0x30 && p.entry == l);
  CHECK(p.vco1.min_freq == 500000 && p.vco1.max_freq == 1000000 && p.vco2.min_freq == 400000 && p.vco2.max_freq == 1000000);
  CHECK(p.vco1.min_inputfreq == 2000 && p.vco2.min_inputfreq == 20000 && p.vco1.max_inputfreq == 50000 && p.vco2.max_inputfreq == 400000);
  CHECK(p.vco1.max_m == 13 && p.vco2.max_n == 31 && p.vco2.max_m == 4 && p.max_p == 6 && p.max_p_usable == 6 && p.bias_p == 1);
  CHECK(parse_pll_limits(bios, kG84, 25000, 0x4008, &p) == NvPllError::None && p.type == kPllMemory);

  // 0x50: no register; reference clock at +1, the limits in the entry.
  Bytes w = image_with_c();
  put8(w, kTable, 0x50); put8(w, kTable + 1, 4); put8(w, kTable + 2, 0x13); put8(w, kTable + 3, 2);
  for (int i = 0; i < 2; ++i) {
    const size_t e = kTable + 4 + i * 0x13;
    put8(w, e, 0x80 + i); put16(w, e + 1, 27); put16(w, e + 3, 100);
    put16(w, e + 5, 400); put16(w, e + 7, 800); put16(w, e + 9, 13); put16(w, e + 11, 27);
    put8(w, e + 13, 1); put8(w, e + 14, 4); put8(w, e + 15, 8); put8(w, e + 16, 255); put8(w, e + 17, 1); put8(w, e + 18, 6);
  }
  NvBios b5(view(w));
  CHECK(parse_pll_limits(b5, kGv100, 25000, kPllVpll1, &p) == NvPllError::None);
  CHECK(p.reg == 0 && p.refclk == 27000 && p.vco1.max_freq == 800000 && p.vco1.max_m == 4 && p.max_p == 6 && p.entry == kTable + 4 + 0x13);
}

static void test_versions_through_the_map() {
  // 0x21 (G84's map: Core = 0x4028): header 4, entries of 0x23 bytes starting with their register; the reference clock at +31.
  Bytes v = image_with_c();
  put8(v, kTable, 0x21); put8(v, kTable + 1, 4); put8(v, kTable + 2, 0x23); put8(v, kTable + 3, 2);
  const uint32_t regs[2] = {0x4020, 0x4028};
  for (int i = 0; i < 2; ++i) {
    const size_t e = kTable + 4 + i * 0x23;
    put32(v, e, regs[i]);
    put16(v, e + 4, 100 + i); put16(v, e + 6, 400); put16(v, e + 8, 400); put16(v, e + 10, 1000);
    put16(v, e + 12, 5); put16(v, e + 14, 20); put16(v, e + 16, 30); put16(v, e + 18, 400);
    put8(v, e + 20, 1); put8(v, e + 21, 255); put8(v, e + 22, 1); put8(v, e + 23, 4);
    put8(v, e + 24, 1); put8(v, e + 25, 255); put8(v, e + 26, 1); put8(v, e + 27, 4);
    put8(v, e + 29, 7); put8(v, e + 30, 2); put32(v, e + 31, 27000);
  }
  NvBios bios(view(v));
  NvPllLimits p;
  CHECK(parse_pll_limits(bios, kG84, 25000, kPllCore, &p) == NvPllError::None);
  CHECK(p.reg == 0x4028 && p.vco1.min_freq == 101000 && p.entry == kTable + 4 + 0x23 && p.refclk == 27000);
  CHECK(p.max_p == 7 && p.max_p_usable == 6 /* (bios chip 0x04 < 0x60) */ && p.bias_p == 2);
  // the register form
  CHECK(parse_pll_limits(bios, kG84, 25000, 0x4020, &p) == NvPllError::None && p.type == kPllShader && p.vco1.min_freq == 100000);
  // in the map but not in the table: nouveau takes the first entry
  CHECK(parse_pll_limits(bios, kG84, 25000, kPllVdec, &p) == NvPllError::None && p.reg == 0x4030 && p.entry == kTable + 4);
  // not in G84's map
  CHECK(parse_pll_limits(bios, kG84, 25000, kPllUnk40, &p) == NvPllError::NotFound);
  // NV50's map has it
  CHECK(parse_pll_limits(bios, kNv50, 25000, kPllUnk40, &p) == NvPllError::None && p.reg == 0x00e810);
  // a GT215 has no map: a < 0x30 table cannot be used
  CHECK(parse_pll_limits(bios, kGt215, 25000, kPllCore, &p) == NvPllError::NotFound);

  // 0x10: the u32 limits right after the table's first byte; fixed M/N limits.
  Bytes w = image_with_c();
  put8(w, kTable, 0x10);
  for (int i = 0; i < 6; ++i) put32(w, kTable + 1 + i * 4, 100000u * (i + 1));
  NvBios b10(view(w));
  CHECK(parse_pll_limits(b10, kG84, 25000, kPllCore, &p) == NvPllError::None);
  CHECK(p.vco1.min_freq == 100000 && p.vco1.max_freq == 200000 && p.vco2.min_freq == 300000 && p.vco2.max_freq == 400000);
  CHECK(p.vco1.min_inputfreq == 500000 && p.vco2.min_inputfreq == 600000 && p.vco1.max_inputfreq == 0x7fffffffu);
  CHECK(p.max_p == 7 && p.max_p_usable == 6 && p.vco1.min_n == 1 && p.vco1.max_m == 0xd && p.vco2.min_n == 4 && p.vco2.max_n == 0x28);
  CHECK(p.refclk == 25000);   // (no reference clock in the table: the crystal)

  // No table at all: the BMP-less defaults (VCO 128-256 MHz, M 8-14 with a 27 MHz crystal and BIOS chip < 0x11, P up to 4).
  Bytes n = kepler_image(false);
  NvBios none(view(n));
  NvPllTable absent;
  CHECK(!find_pll_table(none, &absent, nullptr));
  CHECK(parse_pll_limits(none, kG84, 27000, kPllCore, &p) == NvPllError::None);
  CHECK(p.table_version == 0 && p.reg == 0x4028 && p.refclk == 27000 && p.vco1.min_freq == 128000 && p.vco1.max_freq == 256000);
  CHECK(p.vco1.min_m == 8 && p.vco1.max_m == 0xe && p.max_p == 4 && p.max_p_usable == 4);
  CHECK(parse_pll_limits(none, kG84, 13500, kPllCore, &p) == NvPllError::None && p.vco1.min_m == 7 && p.vco1.max_m == 0xd);
}

// --- The calculators ---------------------------------------------------------------------------------------------------------------------

static NvPllLimits gt215_limits() {
  NvPllLimits l;
  l.refclk = 27000;
  l.vco1.min_freq = 400000; l.vco1.max_freq = 800000;
  l.vco1.min_inputfreq = 13000; l.vco1.max_inputfreq = 27000;
  l.vco1.min_m = 1; l.vco1.max_m = 4; l.vco1.min_n = 8; l.vco1.max_n = 255;
  l.min_p = 1; l.max_p = 6;
  return l;
}

static void test_gt215_calc() {
  NvPllLimits l = gt215_limits();
  NvPllCoefs c;
  uint32_t got = 0;
  // 148.5 MHz: P = 800000 / 148500 = 5; M from (27000+27000)/27000 = 2 to min((27000+13000)/13000 = 3, 4). M = 2: 148500*5*2 = 1485000
  // = 55 * 27000 exactly; the fractional form takes N - 1 = 54 and fN = 27000 -> ((27000 << 13) + 13500) / 27000 = 8192, - 4096 = 0x1000.
  CHECK(gt215_pll_calc(l, 148500, true, &c, &got) && c.p == 5 && c.m == 2 && c.n == 54 && c.fn == 0x1000 && got == 148500);
  // The integer form: N = 55 (fN 0 < 13500, no rounding up) is exact at M = 2; M = 3 gives 83 (error 900): 55/2/5 stays.
  CHECK(gt215_pll_calc(l, 148500, false, &c, &got) && c.p == 5 && c.m == 2 && c.n == 55 && c.fn == 0 && got == 148500);
  // 160 MHz, P exactly 800000/160000 = 5. M = 2: 1600000 = 59 * 27000 + 7000; 7000 < 13500 -> N = 58, fN = 34000;
  // ((34000 << 13) + 13500) / 27000 = 10316, - 4096 = 6220.
  CHECK(gt215_pll_calc(l, 160000, true, &c, &got) && c.p == 5 && c.m == 2 && c.n == 58 && c.fn == 6220);
  // integer: M = 2: N = 59 (7000 < 13500), 27000*59/2/5 = 159300 (error 700); M = 3: 2400000 = 88 * 27000 + 24000 -> N = 89,
  // 27000*89/3/5 = 160200 (error 200): M = 3 wins.
  CHECK(gt215_pll_calc(l, 160000, false, &c, &got) && c.m == 3 && c.n == 89 && got == 160200);
  // Above the VCO: P = 0 is raised to min_p = 1. 1 GHz: M = 2: 2000000 = 74 * 27000 + 2000 -> N = 73 (in range): fine for gt215.
  CHECK(gt215_pll_calc(l, 1000000, true, &c, &got) && c.p == 1 && c.n == 73 && c.m == 2);
  // ... but with N at most 40 it is unreachable (N = 73 > 40 breaks the loop at the first M).
  NvPllLimits small = l;
  small.vco1.max_n = 40;
  CHECK(!gt215_pll_calc(small, 1000000, true, &c, &got));
  // 1 MHz: P capped to 6; M = 2: 12000 -> N = 0 - 1 below min_n; M = 3: 18000 -> N = 0: unreachable.
  CHECK(!gt215_pll_calc(l, 1000, true, &c, &got));
  // Divisions by zero refused: no frequency, no reference clock, no input range, P of 0.
  CHECK(!gt215_pll_calc(l, 0, true, &c, &got));
  NvPllLimits z = l; z.refclk = 0; CHECK(!gt215_pll_calc(z, 148500, true, &c, &got));
  z = l; z.vco1.max_inputfreq = 0; CHECK(!gt215_pll_calc(z, 148500, true, &c, &got));
  z = l; z.vco1.min_inputfreq = 0; CHECK(!gt215_pll_calc(z, 148500, true, &c, &got));
  z = l; z.min_p = 0; z.max_p = 0; CHECK(!gt215_pll_calc(z, 148500, true, &c, &got));
  z = l; z.vco1.max_m = 0; CHECK(!gt215_pll_calc(z, 148500, true, &c, &got));
}

static NvPllLimits nv04_limits() {
  NvPllLimits l;
  l.refclk = 27000;
  l.vco1.min_freq = 500000; l.vco1.max_freq = 1000000;
  l.vco1.min_inputfreq = 2000; l.vco1.max_inputfreq = 50000;
  l.vco1.min_n = 1; l.vco1.max_n = 255; l.vco1.min_m = 1; l.vco1.max_m = 13;
  l.max_p = l.max_p_usable = 6;
  return l;
}

static void test_nv04_calc() {
  const NvBiosVersion modern{0x80, 0x04}, nv3x{0x05, 0x31};
  NvPllLimits l = nv04_limits();
  NvPllCoefs c;
  // Single stage, 135 MHz: log2 P 0 and 1 leave the VCO below 500 MHz; P = 4: 540000, M = 1: N = (540000 + 13500) / 27000 = 20,
  // ((20 * 27000 + 2) / 4 + 0) / 1 = 135000: exact.
  CHECK(nv04_pll_calc(l, modern, 135000, true, &c) == 135000 && c.n1 == 20 && c.m1 == 1 && c.p == 2 && c.n2 == 1 && c.m2 == 1);
  CHECK(nv04_pll_calc(l, modern, 135000, false, &c) == 135000 && c.n1 == 20 && c.p == 2);
  // 100 MHz: P = 8 (800000), M = 1: N = 30 -> 810000 / 8 = 101250 (delta 1250); M = 2: N = 59 -> (1593000 + 4)/8 = 199125, +1 / 2
  // = 99563 (delta 437); M = 3: N = 89 -> (2403000 + 4)/8 = 300375, +1 / 3 = 100125 (125); M = 4: N = 119 -> 401625 + 2 / 4 = 100406
  // (406); M = 5: N = 148 -> (3996000 + 4)/8 = 499500, + 2 / 5 = 99900 (100); M = 6: N = 178 -> 600750 + 3 / 6 = 100125 (125);
  // M = 7: N = 207 -> 698625 + 3 / 7 = 99804 (196); M = 8: N = 237 -> 799875 + 4 / 8 = 99984 (16); M = 9: N = 267 > 255: break.
  // P = 16: 1600000 > 1000000: stop. Best: N 237, M 8, P 3 = 99984.
  CHECK(nv04_pll_calc(l, modern, 100000, true, &c) == 99984 && c.n1 == 237 && c.m1 == 8 && c.p == 3);
  // NV3x BIOS (major < 0x60, chip 0x31 < 0x40): above 200 MHz M is limited to 4. 270 MHz: P = 2 (540000), M = 1: N = 20 -> 270000.
  CHECK(nv04_pll_calc(l, nv3x, 270000, true, &c) == 270000 && c.n1 == 20 && c.m1 == 1 && c.p == 1);
  // Boundary: 1000 MHz is the VCO maximum: P = 1, M = 1: N = 37 -> 999000 (delta 1000); M = 2: N = 74 -> 999000; ... M = 13: N = 481 >
  // 255 at M = 7 (N = 259): break. Best: the first, 37/1/0 = 999000.
  CHECK(nv04_pll_calc(l, modern, 1000000, true, &c) == 999000 && c.n1 == 37 && c.m1 == 1 && c.p == 0);
  // Unreachable: N limited to 10 and every P's N for M = 1 already above it.
  NvPllLimits small = l;
  small.vco1.max_n = 10;
  CHECK(nv04_pll_calc(small, modern, 5000000, true, &c) == 0);
  // The input frequency below its minimum for every M: nothing.
  NvPllLimits slow = l;
  slow.vco1.min_inputfreq = 30000;
  CHECK(nv04_pll_calc(slow, modern, 135000, true, &c) == 0);
  // No reference clock, M of 0: refused rather than divided by.
  NvPllLimits z = l; z.refclk = 0; CHECK(nv04_pll_calc(z, modern, 135000, true, &c) == 0);
  z = l; z.vco1.min_m = 0; CHECK(nv04_pll_calc(z, modern, 135000, true, &c) == 135000 && c.m1 == 1);
  // A hostile P limit does not overflow the shifts.
  z = l; z.max_p_usable = 255; z.min_p = 0; (void)nv04_pll_calc(z, modern, 135000, true, &c);

  // Two stages, 108 MHz: vco2 = (1000000 - 5000) / 2 = 497500; log2 P rises while 108000 <= 497500 >> P: 0, 1, 2 -> 3 (62187 < 108000):
  // clkP = 864000. M1 = 1 (27000 within 5000..30000), N1 = 4 is the first VCO1 >= 100000 (108000); M2 = 1: N2 = (864000 + 54000) /
  // 108000 = 8 (ratio 8 within 4..10 for BIOS chip < 0x60), VCO2 = 864000, >> 3 = 108000: exact.
  NvPllLimits d = l;
  d.vco1.min_freq = 100000; d.vco1.max_freq = 400000;
  d.vco1.min_inputfreq = 5000; d.vco1.max_inputfreq = 30000;
  d.vco1.min_m = 1; d.vco1.max_m = 4;
  d.vco2.min_freq = 400000; d.vco2.max_freq = 1000000;
  d.vco2.min_inputfreq = 20000; d.vco2.max_inputfreq = 400000;
  d.vco2.min_n = 1; d.vco2.max_n = 255; d.vco2.min_m = 1; d.vco2.max_m = 4;
  CHECK(nv04_pll_calc(d, modern, 108000, true, &c) == 108000 && c.n1 == 4 && c.m1 == 1 && c.n2 == 8 && c.m2 == 1 && c.p == 3);
  // Without an N2 (the caller's single-stage PLL) the second VCO is ignored.
  CHECK(nv04_pll_calc(d, modern, 108000, false, &c) != 0 && c.n2 == 0);
  // Fixed gain (min = max for M2 and N2): VCO2 = VCO1. 54 MHz: log2 P -> 4 (497500 >> 4 = 31093 < 54000), clkP = 864000;
  // M1 = 1, N1 = 4..: M2 = N2 = 1 only: N2 = (864000 + 54000)/108000 = 8 > 1: break; ... N1 = 32: 864000, N2 = (864000 + 432000) /
  // 864000 = 1: out = 864000 >> 4 = 54000. (N1 > 14 gives a VCO1 above 400000: break before that.) So nothing fits.
  NvPllLimits fixed = d;
  fixed.vco2.min_m = fixed.vco2.max_m = 1; fixed.vco2.min_n = fixed.vco2.max_n = 1;
  CHECK(nv04_pll_calc(fixed, modern, 54000, true, &c) == 0);
  // With the first VCO and the second stage's input up to 1 GHz: from N1 = 22 (594000: N2 = (864000 + 297000) / 594000 = 1) the output
  // rises towards the target, and N1 = 32 (864000) hits it: 54000 exactly.
  fixed.vco1.max_freq = 1000000;
  fixed.vco2.max_inputfreq = 1000000;
  CHECK(nv04_pll_calc(fixed, modern, 54000, true, &c) == 54000 && c.n1 == 32 && c.m1 == 1 && c.n2 == 1 && c.p == 4);
}

// --- Programming -------------------------------------------------------------------------------------------------------------------------

static void test_programming() {
  // GF100 family (a Kepler): the VPLL with 148.5 MHz, coefficients as in test_gt215_calc.
  {
    Bytes v = image_with_c();
    const Limits40 e[3] = {kCore40, kVpll0, kVpll1};
    table40(v, e, 3);
    NvBios bios(view(v));
    FakeRegs regs;
    regs.regs[0x0061205c] = 0x00000001;
    NvPllSetInputs in;
    in.bios = &bios;
    in.chip = kKepler;
    in.registers = &regs;
    in.crystal_khz = 27000;
    const NvPllSetReport rep = nv_pll_set(in, kPllVpll0, 148500);
    CHECK(rep.ok() && rep.family == NvPllFamily::Gf100 && rep.writes == 3);
    CHECK(regs.writes.size() == 3);
    CHECK(regs.writes[0].first == 0x0061205c && regs.writes[0].second == 0x00000101);   // mask(+0x0c, 0, 0x100)
    CHECK(regs.writes[1].first == 0x00612054 && regs.writes[1].second == 0x00053602);   // P 5, N 54, M 2
    CHECK(regs.writes[2].first == 0x00612060 && regs.writes[2].second == 0x10000000);   // fN 0x1000 << 16
    // the core PLL is not programmed by gf100_devinit_pll_set
    FakeRegs r2;
    in.registers = &r2;
    CHECK(nv_pll_set(in, kPllCore, 148500).status == NvPllSetStatus::Unimplemented && r2.writes.empty());
    // a type the table does not have
    const NvPllSetReport miss = nv_pll_set(in, kPllMemory, 148500);
    CHECK(miss.status == NvPllSetStatus::BadLimits && miss.error == NvPllError::NotFound && r2.writes.empty());
    // nothing fits
    CHECK(nv_pll_set(in, kPllVpll1, 1000).status == NvPllSetStatus::NoCoefficients && r2.writes.empty());
    // the crystal from the strap when none is given (NV_PEXTDEV_BOOT_0 bit 22: 27 MHz)
    in.crystal_khz = 0;
    r2.regs[0x101000] = 0x00400000;
    put16(v, kTable + 5 + 0x10 + 9, 0);   // VPLL0 without its own reference clock
    NvBios b2(view(v));
    in.bios = &b2;
    const NvPllSetReport strap = nv_pll_set(in, kPllVpll0, 148500);
    CHECK(strap.ok() && strap.limits.refclk == 27000);
    // a hostile register: nothing written
    put32(v, kTable + 5 + 0x10 + 3, 0x00fffff8);
    NvBios b3(view(v));
    in.bios = &b3;
    FakeRegs r3;
    in.registers = &r3;
    in.crystal_khz = 27000;
    CHECK(nv_pll_set(in, kPllVpll0, 148500).status == NvPllSetStatus::BadRegister && r3.writes.empty());
  }
  // GT215 (VPLL0/1 only): wr(+0, 0x50000610), mask(+4, 0x003fffff, P << 16 | M << 8 | N), wr(+8, fN).
  {
    Bytes v = image_with_c();
    const Limits40 e[2] = {kCore40, kVpll0};
    table40(v, e, 2);
    NvBios bios(view(v));
    FakeRegs regs;
    regs.regs[0x00612054] = 0xffc00000;
    Trace trace;
    NvPllSetInputs in;
    in.bios = &bios;
    in.chip = kGt215;
    in.registers = &regs;
    in.crystal_khz = 27000;
    in.trace = &trace;
    const NvPllSetReport rep = nv_pll_set(in, kPllVpll0, 148500);
    CHECK(rep.ok() && rep.family == NvPllFamily::Gt215);
    CHECK(regs.writes.size() == 3 && regs.writes[0].first == 0x00612050 && regs.writes[0].second == 0x50000610);
    CHECK(regs.writes[1].first == 0x00612054 && regs.writes[1].second == (0xffc00000u | 0x00050236));
    CHECK(regs.writes[2].first == 0x00612058 && regs.writes[2].second == 0x1000);
    CHECK(trace.all.size() == 3 && trace.all[1].kind == NvInitAccess::Mmio && trace.all[1].address == 0x00612054);
    FakeRegs r2;
    in.registers = &r2;
    CHECK(nv_pll_set(in, kPllCore, 148500).status == NvPllSetStatus::Unimplemented && r2.writes.empty());
  }
  // GV100: a 0x50 table; the VPLLs at 0x00ef00 + head * 0x40.
  {
    Bytes v = image_with_c();
    put8(v, kTable, 0x50); put8(v, kTable + 1, 4); put8(v, kTable + 2, 0x13); put8(v, kTable + 3, 2);
    for (int i = 0; i < 2; ++i) {
      const size_t e = kTable + 4 + i * 0x13;
      put8(v, e, 0x80 + i); put16(v, e + 1, 27);
      put16(v, e + 5, 400); put16(v, e + 7, 800); put16(v, e + 9, 13); put16(v, e + 11, 27);
      put8(v, e + 13, 1); put8(v, e + 14, 4); put8(v, e + 15, 8); put8(v, e + 16, 255); put8(v, e + 17, 1); put8(v, e + 18, 6);
    }
    NvBios bios(view(v));
    FakeRegs regs;
    NvPllSetInputs in;
    in.bios = &bios;
    in.chip = kGv100;
    in.registers = &regs;
    in.crystal_khz = 27000;
    CHECK(nv_pll_set(in, kPllVpll1, 148500).ok());
    CHECK(regs.writes.size() == 2 && regs.writes[0].first == 0x00ef50 && regs.writes[0].second == 0x10000000);
    CHECK(regs.writes[1].first == 0x00ef44 && regs.writes[1].second == 0x00053602);
  }
  // NV50 family (G84): a 0x30 table with a single-stage core PLL, a two-stage VPLL and the memory PLL.
  {
    Bytes v = image_with_c();
    put8(v, kTable, 0x30); put8(v, kTable + 1, 4); put8(v, kTable + 2, 7); put8(v, kTable + 3, 3);
    const uint8_t types[3] = {kPllCore, kPllVpll0, kPllMemory};
    const uint32_t regs_of[3] = {0x4028, 0x614100, 0x4008};
    for (int i = 0; i < 3; ++i) {
      const size_t e = kTable + 4 + i * 7, l = 0x1400 + i * 0x20;
      put8(v, e, types[i]); put16(v, e + 1, l); put32(v, e + 3, regs_of[i]);
      if (types[i] == kPllVpll0) {
        put16(v, l + 0, 100); put16(v, l + 2, 400); put16(v, l + 4, 400); put16(v, l + 6, 1000);
        put16(v, l + 8, 5); put16(v, l + 10, 20); put16(v, l + 12, 30); put16(v, l + 14, 400);
        put8(v, l + 16, 1); put8(v, l + 17, 255); put8(v, l + 18, 1); put8(v, l + 19, 4);
        put8(v, l + 20, 1); put8(v, l + 21, 255); put8(v, l + 22, 1); put8(v, l + 23, 4);
      } else {
        put16(v, l + 0, 500); put16(v, l + 2, 1000);
        put16(v, l + 8, 2); put16(v, l + 12, 50);
        put8(v, l + 16, 1); put8(v, l + 17, 255); put8(v, l + 18, 1); put8(v, l + 19, 13);
      }
      put8(v, l + 25, 6); put8(v, l + 27, 1); put32(v, l + 28, 27000);
    }
    NvBios bios(view(v));
    FakeRegs regs;
    regs.regs[0x4028] = 0xffffffff;
    NvPllSetInputs in;
    in.bios = &bios;
    in.chip = kG84;
    in.registers = &regs;
    in.crystal_khz = 27000;
    // core: mask(+0, 0x00070000, P << 16), wr(+4, N1 << 8 | M1): 135 MHz -> 20/1/2
    CHECK(nv_pll_set(in, 0x4028, 135000).ok());
    CHECK(regs.writes.size() == 2 && regs.writes[0].first == 0x4028 && regs.writes[0].second == 0xfffaffff);
    CHECK(regs.writes[1].first == 0x402c && regs.writes[1].second == 0x1401);
    // memory: mask(+0, 0x01ff0000, P << 22 | bias << 19 | P << 16)
    regs.writes.clear();
    CHECK(nv_pll_set(in, kPllMemory, 135000).ok());
    CHECK(regs.writes.size() == 2 && regs.writes[0].first == 0x4008 && regs.writes[0].second == 0x008a0000);
    CHECK(regs.writes[1].first == 0x400c && regs.writes[1].second == 0x1401);
    // VPLL0, two stages: 108 MHz -> N1 4, M1 1, N2 8, M2 1, log2 P 3
    regs.writes.clear();
    const NvPllSetReport rep = nv_pll_set(in, kPllVpll0, 108000);
    CHECK(rep.ok() && rep.achieved_khz == 108000);
    CHECK(regs.writes.size() == 3 && regs.writes[0].first == 0x614100 && regs.writes[0].second == 0x10000611);
    CHECK(regs.writes[1].first == 0x614104 && regs.writes[1].second == 0x00010004);
    CHECK(regs.writes[2].first == 0x614108 && regs.writes[2].second == 0x30010008);
  }
  // Chips whose PLLs are not programmed here: nothing written.
  {
    Bytes v = image_with_c();
    const Limits40 e[1] = {kVpll0};
    table40(v, e, 1);
    NvBios bios(view(v));
    const Chip none[] = {chip_of(0x046000a1), chip_of(0x164000a1, 0x16400000), chip_of(0x0ea000a1) /* GK20A */};
    for (const Chip& c : none) {
      FakeRegs regs;
      NvPllSetInputs in;
      in.bios = &bios;
      in.chip = c;
      in.registers = &regs;
      CHECK(pll_family(c) == NvPllFamily::None);
      CHECK(nv_pll_set(in, kPllVpll0, 148500).status == NvPllSetStatus::NoFamily && regs.writes.empty());
    }
    CHECK(pll_family(kNv50) == NvPllFamily::Nv50 && pll_family(chip_of(0x098000a1)) == NvPllFamily::Nv50);
    CHECK(pll_family(chip_of(0x0af000a1)) == NvPllFamily::Gt215 && pll_family(chip_of(0x124000a1)) == NvPllFamily::Gf100);
    CHECK(pll_family(chip_of(0x134000a1)) == NvPllFamily::Gf100 && pll_family(kGv100) == NvPllFamily::Gv100);
  }
}

// --- The hooks under the interpreter ----------------------------------------------------------------------------------------------------

static void test_hooks() {
  Bytes v = image_with_c();
  const Limits40 e[2] = {kCore40, kVpll0};
  table40(v, e, 2);
  // A script: INIT_PLL2 (0x4b) VPLL0's register at 148.5 MHz, INIT_PLL (0x79) the core PLL at 100 MHz (in 10 kHz), INIT_GPIO (0x8e),
  // INIT_DONE.
  const size_t s = 0x2000;
  put8(v, s, 0x4b); put32(v, s + 1, 0x00612050); put32(v, s + 5, 148500);
  put8(v, s + 9, 0x79); put32(v, s + 10, 0x00137000); put16(v, s + 14, 10000);
  put8(v, s + 16, 0x8e);
  put8(v, s + 17, 0x71);
  NvBios bios(view(v));
  FakeRegs regs;
  regs.regs[0x101000] = 0x00400040;   // 25 MHz crystal (unused: the table has 27 MHz)
  InnerHooks inner;
  NvPllInitHooks hooks(bios, kKepler, regs, &inner);
  NvInitOptions options;
  NvInitScript script(bios, kKepler, regs, nullptr, &hooks, nullptr, options);
  CHECK(script.run(s));
  CHECK(script.result().ok() && script.result().plls_unsupported == 1);   // (the core PLL: gf100 programs VPLLs only)
  CHECK(hooks.programmed() == 1 && hooks.failed() == 1 && hooks.last().status == NvPllSetStatus::Unimplemented);
  CHECK(regs.read32(0x00612054) == 0x00053602);
  CHECK(inner.gpio.size() == 1 && inner.gpio[0] == kInitGpioAll);   // delegated
  // without an inner object the other hooks do nothing
  NvPllInitHooks bare(bios, kKepler, regs);
  CHECK(!bare.gpio_reset(1) && !bare.meminit() && bare.i2c_bus(0) == nullptr && bare.aux_channel(0) == nullptr);
  // a dry run never calls the hooks
  FakeRegs dry_regs;
  NvPllInitHooks dry_hooks(bios, kKepler, dry_regs, nullptr, 27000);
  options.dry_run = true;
  NvInitScript dry(bios, kKepler, dry_regs, nullptr, &dry_hooks, nullptr, options);
  CHECK(dry.run(s) && dry_regs.writes.empty() && dry_hooks.programmed() == 0);
}

// --- Hostile images ---------------------------------------------------------------------------------------------------------------------

static Bytes hostile_base() {
  Bytes v = image_with_c();
  const Limits40 e[3] = {kCore40, kVpll0, kVpll1};
  table40(v, e, 3);
  v.resize(0x1480);   // the image ends right after the last limits block
  return v;
}

static void exercise(const NvBios& bios, const Chip& chip, uint32_t* ok_count) {
  static const uint32_t kTypes[] = {kPllCore, kPllMemory, kPllVpll0, kPllVpll1, kPllVpll2, 0x00612050, 0x4028};
  for (uint32_t type : kTypes) {
    NvPllLimits l;
    if (parse_pll_limits(bios, chip, 27000, type, &l) == NvPllError::None) {
      ++*ok_count;
      NvPllCoefs c;
      uint32_t got = 0;
      (void)gt215_pll_calc(l, 148500, true, &c, &got);
      (void)gt215_pll_calc(l, 25175, false, &c, &got);
      (void)nv04_pll_calc(l, NvBiosVersion{0x80, 0x04}, 148500, true, &c);
      (void)nv04_pll_calc(l, NvBiosVersion{0x05, 0x31}, 25175, false, &c);
    }
    FakeRegs regs;
    NvPllSetInputs in;
    in.bios = &bios;
    in.chip = chip;
    in.registers = &regs;
    in.crystal_khz = 27000;
    (void)nv_pll_set(in, type, 148500);
    for (const auto& w : regs.writes) CHECK(w.first < (16u << 20) && (w.first & 3) == 0);
  }
}

static void test_truncation_and_flips() {
  const Bytes full = hostile_base();
  for (size_t n = 0; n <= full.size(); ++n) {
    Bytes cut(full.begin(), full.begin() + n);
    NvBios bios(view(cut));
    uint32_t ok = 0;
    exercise(bios, kKepler, &ok);
    exercise(bios, kGt215, &ok);
    if (n == full.size()) CHECK(ok >= 6);
    NvPllLimits l;
    if (n < 0x1400 + 0x40 + 14 && n > 0x200) CHECK(parse_pll_limits(bios, kKepler, 27000, kPllVpll1, &l) != NvPllError::None);
  }
  // The table header claims more entries than the image has.
  {
    Bytes v = full;
    put8(v, kTable + 3, 0xff);
    NvBios bios(view(v));
    NvPllLimits l;
    CHECK(parse_pll_limits(bios, kKepler, 27000, 0x00123456, &l) == NvPllError::Truncated);
  }
  srand(0x5eed);
  for (int round = 0; round < 400; ++round) {
    Bytes v = full;
    const int flips = 1 + rand() % 8;
    for (int i = 0; i < flips; ++i) {
      // the BIT 'C' entry and pointer, the table header and entries, the limits blocks
      const size_t regions[3][2] = {{0x10c, 0x40}, {kTable, 0x40}, {0x1400, 0x80}};
      const auto& r = regions[rand() % 3];
      const size_t at = r[0] + static_cast<size_t>(rand()) % r[1];
      if (at < v.size()) v[at] ^= static_cast<uint8_t>(1u << (rand() % 8));
    }
    if (round % 7 == 0) put32(v, 0x0f00, static_cast<uint32_t>(rand()));   // a wild table pointer
    NvBios bios(view(v));
    uint32_t ok = 0;
    exercise(bios, round % 2 ? kKepler : kG84, &ok);
    exercise(bios, kGv100, &ok);
  }
}

int main() {
  test_version_40();
  test_version_30_and_50();
  test_versions_through_the_map();
  test_gt215_calc();
  test_nv04_calc();
  test_programming();
  test_hooks();
  test_truncation_and_flips();
  if (g_failed) {
    printf("NvPllTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("NvPllTest: all passed\n");
  return 0;
}
