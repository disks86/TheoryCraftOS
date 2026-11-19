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

// Follows nouveau (MIT license): nvkm/subdev/bios/pll.c, nvkm/subdev/clk/pllnv04.c and pllgt215.c, nvkm/subdev/devinit/nv50.c, gt215.c,
// gf100.c and gv100.c (the pll_set functions), base.c (bios version bytes).
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvPll.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvClk.hpp"

namespace hardware {
namespace nv {

namespace {

constexpr uint32_t kBar0Bytes = 16u << 20;
constexpr int kMaxLog2P = 16;           // a hostile table's P stays a sane shift
constexpr int64_t kIntMax = 0x7fffffff;

int64_t abs64(int64_t v) { return v < 0 ? -v : v; }
int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

struct PllMapping {
  uint8_t type;
  uint32_t reg;
};

constexpr PllMapping kNv04Map[] = {{kPllCore, 0x680500}, {kPllMemory, 0x680504}, {kPllVpll0, 0x680508}, {kPllVpll1, 0x680520}, {0, 0}};
constexpr PllMapping kNv40Map[] = {{kPllCore, 0x004000}, {kPllMemory, 0x004020}, {kPllVpll0, 0x680508}, {kPllVpll1, 0x680520}, {0, 0}};
constexpr PllMapping kNv50Map[] = {{kPllCore, 0x004028},  {kPllShader, 0x004020}, {kPllUnk03, 0x004000},
                                   {kPllMemory, 0x004008}, {kPllUnk40, 0x00e810},  {kPllUnk41, 0x00e818},
                                   {kPllUnk42, 0x00e824},  {kPllVpll0, 0x614100},  {kPllVpll1, 0x614900}, {0, 0}};
constexpr PllMapping kG84Map[] = {{kPllCore, 0x004028}, {kPllShader, 0x004020}, {kPllMemory, 0x004008}, {kPllVdec, 0x004030},
                                  {kPllUnk41, 0x00e818}, {kPllVpll0, 0x614100}, {kPllVpll1, 0x614900}, {0, 0}};

// pll_map
const PllMapping* pll_map(const Chip& chip) {
  switch (chip.card_type) {
    case 0x04: case 0x10: case 0x11: case 0x20: case 0x30: return kNv04Map;
    case 0x40: return kNv40Map;
    case 0x50:
      if (chip.chipset == 0x50) return kNv50Map;
      if (chip.chipset < 0xa3 || chip.chipset == 0xaa || chip.chipset == 0xac) return kG84Map;
      return nullptr;
    default: return nullptr;
  }
}

// The checked reads, as a small reader that remembers the first failure (nouveau reads 0 past the image; this stops).
struct Rd {
  const NvBios& bios;
  bool bad = false;
  uint8_t r8(uint64_t at) { uint8_t v = 0; if (!bios.rd08(at, &v)) bad = true; return v; }
  uint16_t r16(uint64_t at) { uint16_t v = 0; if (!bios.rd16(at, &v)) bad = true; return v; }
  uint32_t r32(uint64_t at) { uint32_t v = 0; if (!bios.rd32(at, &v)) bad = true; return v; }
};

// pll_limits_table: the table's offset (0: none) and header; `rd.bad` on a read outside the image.
uint32_t pll_limits_table(Rd& rd, NvPllTable* t) {
  const NvBios& bios = rd.bios;
  *t = NvPllTable{};
  BitEntry c;
  uint32_t data = 0;
  if (bios.bit_entry('C', &c)) {
    if (c.version == 1 && c.length >= 10) data = rd.r16(uint64_t{c.offset} + 8);
    if (c.version == 2 && c.length >= 4) data = rd.r32(c.offset);
    if (rd.bad) return 0;
    if (data) {
      t->offset = data;
      t->version = rd.r8(data + 0ull);
      t->header = rd.r8(data + 1ull);
      t->length = rd.r8(data + 2ull);
      t->count = rd.r8(data + 3ull);
      return rd.bad ? 0 : data;
    }
  }
  if (bios.bmp_offset() && bios.bmp_version() >= 0x0524) {
    data = rd.r16(uint64_t{bios.bmp_offset()} + 142);
    if (rd.bad) return 0;
    if (data) {
      t->offset = data;
      t->version = rd.r8(data);
      t->header = 1;
      t->count = 1;
      t->length = 0x18;
      return rd.bad ? 0 : data;
    }
  }
  return 0;
}

// pll_map_reg / pll_map_type in one: by register (`by_reg`) or by type. Returns the entry offset (0: none); fills the other of type/reg.
uint64_t pll_map(Rd& rd, const Chip& chip, const NvPllTable& t, bool by_reg, uint32_t* type, uint32_t* reg) {
  uint64_t data = t.offset;
  if (data && t.version >= 0x30) {
    data += t.header;
    for (uint32_t i = 0; i < t.count; ++i, data += t.length) {
      if (by_reg) {
        const uint32_t r = rd.r32(data + 3);
        if (rd.bad) return 0;
        if (r == *reg) {
          *type = rd.r8(data);
          return rd.bad ? 0 : data;
        }
      } else {
        const uint8_t ty = rd.r8(data);
        if (rd.bad) return 0;
        if (ty == *type) {
          *reg = t.version < 0x50 ? rd.r32(data + 3) : 0;
          return rd.bad ? 0 : data;
        }
      }
    }
    return 0;
  }
  for (const PllMapping* map = pll_map(chip); map && map->reg; ++map) {
    const bool match = by_reg ? map->reg == *reg : map->type == *type;
    if (!match) continue;
    if (by_reg) *type = map->type;
    else *reg = map->reg;
    if (t.version >= 0x20) {
      const uint64_t first = data += t.header;
      for (uint32_t i = 0; i < t.count; ++i, data += t.length) {
        const uint32_t r = rd.r32(data);
        if (rd.bad) return 0;
        if (r == map->reg) return data;
      }
      return first;
    }
    return data + 1;
  }
  return 0;
}

}  // namespace

NvBiosVersion bios_version(const NvBios& bios) {
  NvBiosVersion v;
  BitEntry i;
  if (bios.bit_entry('i', &i) && i.length >= 4) {
    uint8_t major = 0, chip = 0;
    if (bios.rd08(uint64_t{i.offset} + 3, &major) && bios.rd08(uint64_t{i.offset} + 2, &chip)) {
      v.major = major;
      v.chip = chip;
    }
  } else if (bios.bmp_version()) {
    uint8_t major = 0, chip = 0;
    if (bios.rd08(uint64_t{bios.bmp_offset()} + 13, &major) && bios.rd08(uint64_t{bios.bmp_offset()} + 12, &chip)) {
      v.major = major;
      v.chip = chip;
    }
  }
  return v;
}

const char* pll_error_name(NvPllError error) {
  switch (error) {
    case NvPllError::None: return "none";
    case NvPllError::NotFound: return "no limits table entry";
    case NvPllError::UnknownVersion: return "unknown PLL limits table version";
    case NvPllError::Truncated: return "the PLL limits table runs out of the image";
  }
  return "?";
}

bool find_pll_table(const NvBios& bios, NvPllTable* out, bool* truncated) {
  Rd rd{bios};
  const uint32_t data = pll_limits_table(rd, out);
  if (truncated) *truncated = rd.bad;
  return data != 0;
}

NvPllError parse_pll_limits(const NvBios& bios, const Chip& chip, uint32_t crystal_khz, uint32_t type, NvPllLimits* out) {
  Rd rd{bios};
  NvPllTable t;
  pll_limits_table(rd, &t);
  if (rd.bad) return NvPllError::Truncated;
  const uint8_t ver = t.version;
  uint32_t reg = type;
  uint64_t data = 0;
  if (type > kPllMax) {
    data = pll_map(rd, chip, t, true, &type, &reg);
  } else {
    data = pll_map(rd, chip, t, false, &type, &reg);
  }
  if (rd.bad) return NvPllError::Truncated;
  if (ver && !data) return NvPllError::NotFound;

  const NvBiosVersion version = bios_version(bios);
  NvPllLimits info;
  info.type = type;
  info.reg = reg;
  info.table_version = ver;
  info.entry = static_cast<uint32_t>(data);
  switch (ver) {
    case 0x00:
      break;
    case 0x10:
    case 0x11:
      info.vco1.min_freq = rd.r32(data + 0);
      info.vco1.max_freq = rd.r32(data + 4);
      info.vco2.min_freq = rd.r32(data + 8);
      info.vco2.max_freq = rd.r32(data + 12);
      info.vco1.min_inputfreq = rd.r32(data + 16);
      info.vco2.min_inputfreq = rd.r32(data + 20);
      info.vco1.max_inputfreq = static_cast<uint32_t>(kIntMax);
      info.vco2.max_inputfreq = static_cast<uint32_t>(kIntMax);
      info.max_p = 0x7;
      info.max_p_usable = 0x6;
      // (the values of nv30/31/36)
      info.vco1.min_n = version.chip == 0x36 ? 0x5 : 0x1;
      info.vco1.max_n = 0xff;
      info.vco1.min_m = 0x1;
      info.vco1.max_m = 0xd;
      info.vco2.min_n = 0x4;
      info.vco2.max_n = (version.chip == 0x30 || version.chip == 0x35) ? 0x1f : 0x28;
      info.vco2.min_m = 0x1;
      info.vco2.max_m = 0x4;
      break;
    case 0x20:
    case 0x21:
      info.vco1.min_freq = rd.r16(data + 4) * 1000u;
      info.vco1.max_freq = rd.r16(data + 6) * 1000u;
      info.vco2.min_freq = rd.r16(data + 8) * 1000u;
      info.vco2.max_freq = rd.r16(data + 10) * 1000u;
      info.vco1.min_inputfreq = rd.r16(data + 12) * 1000u;
      info.vco2.min_inputfreq = rd.r16(data + 14) * 1000u;
      info.vco1.max_inputfreq = rd.r16(data + 16) * 1000u;
      info.vco2.max_inputfreq = rd.r16(data + 18) * 1000u;
      info.vco1.min_n = rd.r8(data + 20);
      info.vco1.max_n = rd.r8(data + 21);
      info.vco1.min_m = rd.r8(data + 22);
      info.vco1.max_m = rd.r8(data + 23);
      info.vco2.min_n = rd.r8(data + 24);
      info.vco2.max_n = rd.r8(data + 25);
      info.vco2.min_m = rd.r8(data + 26);
      info.vco2.max_m = rd.r8(data + 27);
      info.max_p = rd.r8(data + 29);
      info.max_p_usable = info.max_p;
      if (version.chip < 0x60) info.max_p_usable = 0x6;
      info.bias_p = rd.r8(data + 30);
      if (t.length > 0x22) info.refclk = rd.r32(data + 31);
      break;
    case 0x30: {
      const uint64_t p = rd.r16(data + 1);
      if (rd.bad) return NvPllError::Truncated;
      info.vco1.min_freq = rd.r16(p + 0) * 1000u;
      info.vco1.max_freq = rd.r16(p + 2) * 1000u;
      info.vco2.min_freq = rd.r16(p + 4) * 1000u;
      info.vco2.max_freq = rd.r16(p + 6) * 1000u;
      info.vco1.min_inputfreq = rd.r16(p + 8) * 1000u;
      info.vco2.min_inputfreq = rd.r16(p + 10) * 1000u;
      info.vco1.max_inputfreq = rd.r16(p + 12) * 1000u;
      info.vco2.max_inputfreq = rd.r16(p + 14) * 1000u;
      info.vco1.min_n = rd.r8(p + 16);
      info.vco1.max_n = rd.r8(p + 17);
      info.vco1.min_m = rd.r8(p + 18);
      info.vco1.max_m = rd.r8(p + 19);
      info.vco2.min_n = rd.r8(p + 20);
      info.vco2.max_n = rd.r8(p + 21);
      info.vco2.min_m = rd.r8(p + 22);
      info.vco2.max_m = rd.r8(p + 23);
      info.max_p_usable = info.max_p = rd.r8(p + 25);
      info.bias_p = rd.r8(p + 27);
      info.refclk = rd.r32(p + 28);
      info.entry = static_cast<uint32_t>(p);
      break;
    }
    case 0x40: {
      info.refclk = rd.r16(data + 9) * 1000u;
      const uint64_t p = rd.r16(data + 1);
      if (rd.bad) return NvPllError::Truncated;
      info.vco1.min_freq = rd.r16(p + 0) * 1000u;
      info.vco1.max_freq = rd.r16(p + 2) * 1000u;
      info.vco1.min_inputfreq = rd.r16(p + 4) * 1000u;
      info.vco1.max_inputfreq = rd.r16(p + 6) * 1000u;
      info.vco1.min_m = rd.r8(p + 8);
      info.vco1.max_m = rd.r8(p + 9);
      info.vco1.min_n = rd.r8(p + 10);
      info.vco1.max_n = rd.r8(p + 11);
      info.min_p = rd.r8(p + 12);
      info.max_p = rd.r8(p + 13);
      info.entry = static_cast<uint32_t>(p);
      break;
    }
    case 0x50:
      info.refclk = rd.r16(data + 1) * 1000u;
      info.vco1.min_freq = rd.r16(data + 5) * 1000u;
      info.vco1.max_freq = rd.r16(data + 7) * 1000u;
      info.vco1.min_inputfreq = rd.r16(data + 9) * 1000u;
      info.vco1.max_inputfreq = rd.r16(data + 11) * 1000u;
      info.vco1.min_m = rd.r8(data + 13);
      info.vco1.max_m = rd.r8(data + 14);
      info.vco1.min_n = rd.r8(data + 15);
      info.vco1.max_n = rd.r8(data + 16);
      info.min_p = rd.r8(data + 17);
      info.max_p = rd.r8(data + 18);
      break;
    default:
      return NvPllError::UnknownVersion;
  }
  if (rd.bad) return NvPllError::Truncated;

  if (!info.refclk) info.refclk = crystal_khz;   // (C51's VPLL override: not done, see the header)

  // No limits table, or an empty one (seen on NV18): the BMP's VCO range and fixed M/N/P limits.
  if (!info.vco1.max_freq) {
    if (bios.bmp_offset() && bios.bmp_version() >= 0x0506) {
      info.vco1.max_freq = rd.r32(uint64_t{bios.bmp_offset()} + 67);
      info.vco1.min_freq = rd.r32(uint64_t{bios.bmp_offset()} + 71);
      if (rd.bad) return NvPllError::Truncated;
    } else {
      info.vco1.max_freq = 256000;
      info.vco1.min_freq = 128000;
    }
    info.vco1.min_inputfreq = 0;
    info.vco1.max_inputfreq = static_cast<uint32_t>(kIntMax);
    info.vco1.min_n = 0x1;
    info.vco1.max_n = 0xff;
    info.vco1.min_m = 0x1;
    if (crystal_khz == 13500) {
      if (version.chip < 0x11) info.vco1.min_m = 0x7;   // (nv05 does this, nv11 does not)
      info.vco1.max_m = 0xd;
    } else {
      if (version.chip < 0x11) info.vco1.min_m = 0x8;
      info.vco1.max_m = 0xe;
    }
    info.max_p = (version.chip < 0x17 || version.chip == 0x1a || version.chip == 0x20) ? 4 : 5;
    info.max_p_usable = info.max_p;
  }
  *out = info;
  return NvPllError::None;
}

// --- nv04_pll_calc -----------------------------------------------------------------------------------------------------------------------

namespace {

// getMNP_single
int64_t mnp_single(const NvPllLimits& info, NvBiosVersion bios, int64_t clk, NvPllCoefs* out) {
  int64_t minvco = info.vco1.min_freq, maxvco = info.vco1.max_freq;
  int64_t minM = info.vco1.min_m, maxM = info.vco1.max_m;
  const int64_t minN = info.vco1.min_n, maxN = info.vco1.max_n;
  const int64_t minU = info.vco1.min_inputfreq, maxU = info.vco1.max_inputfreq;
  const int minP = info.min_p;
  const int maxP = info.max_p_usable > kMaxLog2P ? kMaxLog2P : info.max_p_usable;
  const int64_t crystal = info.refclk;
  int64_t bestdelta = INT64_MAX, bestclk = 0;
  if (crystal <= 0) return 0;

  // (verified by nouveau for nv20, nv18, nv28 and nv34; possibly tied to the 27 MHz crystal)
  if (bios.major < 0x60) {
    const int cv = bios.chip;
    if (cv < 0x17 || cv == 0x1a || cv == 0x20) {
      if (clk > 250000) maxM = 6;
      if (clk > 340000) maxM = 2;
    } else if (cv < 0x40) {
      if (clk > 150000) maxM = 6;
      if (clk > 200000) maxM = 4;
      if (clk > 340000) maxM = 2;
    }
  }

  int64_t P = int64_t{1} << maxP;
  if (clk * P < minvco) {
    minvco = clk * maxP;
    maxvco = minvco * 2;
  }
  if (clk + clk / 200 > maxvco) maxvco = clk + clk / 200;   // +0.5%

  if (minM < 1) minM = 1;   // (a zero M would divide by zero)
  for (int thisP = minP; thisP <= maxP; ++thisP) {
    P = int64_t{1} << thisP;
    const int64_t clkP = clk * P;
    if (clkP < minvco) continue;
    if (clkP > maxvco) return bestclk;
    for (int64_t M = minM; M <= maxM; ++M) {
      if (crystal / M < minU) return bestclk;
      if (crystal / M > maxU) continue;
      const int64_t N = (clkP * M + crystal / 2) / crystal;   // (+ crystal/2 rounds better)
      if (N < minN) continue;
      if (N > maxN) break;
      const int64_t calcclk = ((N * crystal + P / 2) / P + M / 2) / M;
      const int64_t delta = abs64(calcclk - clk);
      if (delta < bestdelta) {
        bestdelta = delta;
        bestclk = calcclk;
        out->n1 = static_cast<int>(N);
        out->m1 = static_cast<int>(M);
        out->p = thisP;
        if (delta == 0) return bestclk;
      }
    }
  }
  return bestclk;
}

// getMNP_double
int64_t mnp_double(const NvPllLimits& info, NvBiosVersion bios, int64_t clk, NvPllCoefs* out) {
  const int64_t minvco1 = info.vco1.min_freq, maxvco1 = info.vco1.max_freq;
  const int64_t minvco2 = info.vco2.min_freq;
  int64_t maxvco2 = info.vco2.max_freq;
  const int64_t minU1 = info.vco1.min_inputfreq, minU2 = info.vco2.min_inputfreq;
  const int64_t maxU1 = info.vco1.max_inputfreq, maxU2 = info.vco2.max_inputfreq;
  const int64_t minM1 = max64(info.vco1.min_m, 1), maxM1 = info.vco1.max_m;
  const int64_t minN1 = info.vco1.min_n, maxN1 = info.vco1.max_n;
  const int64_t minM2 = max64(info.vco2.min_m, 1), maxM2 = info.vco2.max_m;
  const int64_t minN2 = info.vco2.min_n, maxN2 = info.vco2.max_n;
  const int maxlog2P = info.max_p_usable > kMaxLog2P ? kMaxLog2P : info.max_p_usable;
  const int64_t crystal = info.refclk;
  const bool fixedgain2 = info.vco2.min_m == info.vco2.max_m && info.vco2.min_n == info.vco2.max_n;
  int64_t bestdelta = INT64_MAX, bestclk = 0;
  if (crystal <= 0) return 0;

  const int64_t vco2 = (maxvco2 - maxvco2 / 200) / 2;
  int log2P = 0;
  while (clk && log2P < maxlog2P && clk <= (vco2 >> log2P)) ++log2P;
  const int64_t clkP = clk << log2P;

  if (maxvco2 < clk + clk / 200) maxvco2 = clk + clk / 200;   // +0.5%

  for (int64_t M1 = minM1; M1 <= maxM1; ++M1) {
    if (crystal / M1 < minU1) return bestclk;
    if (crystal / M1 > maxU1) continue;
    for (int64_t N1 = minN1; N1 <= maxN1; ++N1) {
      const int64_t calcclk1 = crystal * N1 / M1;
      if (calcclk1 < minvco1) continue;
      if (calcclk1 > maxvco1) break;
      if (calcclk1 <= 0) continue;   // (would divide by zero below)
      for (int64_t M2 = minM2; M2 <= maxM2; ++M2) {
        if (calcclk1 / M2 < minU2) break;
        if (calcclk1 / M2 > maxU2) continue;
        const int64_t N2 = (clkP * M2 + calcclk1 / 2) / calcclk1;
        if (N2 < minN2) continue;
        if (N2 > maxN2) break;
        int64_t calcclk2 = calcclk1;
        if (!fixedgain2) {
          if (bios.chip < 0x60 && (N2 / M2 < 4 || N2 / M2 > 10)) continue;
          calcclk2 = calcclk1 * N2 / M2;
          if (calcclk2 < minvco2) break;
          if (calcclk2 > maxvco2) continue;
        }
        const int64_t calcclkout = calcclk2 >> log2P;
        const int64_t delta = abs64(calcclkout - clk);
        if (delta < bestdelta) {
          bestdelta = delta;
          bestclk = calcclkout;
          out->n1 = static_cast<int>(N1);
          out->m1 = static_cast<int>(M1);
          out->n2 = static_cast<int>(N2);
          out->m2 = static_cast<int>(M2);
          out->p = log2P;
          if (delta == 0) return bestclk;
        }
      }
    }
  }
  return bestclk;
}

}  // namespace

uint32_t nv04_pll_calc(const NvPllLimits& limits, NvBiosVersion bios, uint32_t khz, bool two_stage, NvPllCoefs* out) {
  NvPllCoefs c;
  int64_t ret = 0;
  if (!limits.vco2.max_freq || !two_stage) {
    ret = mnp_single(limits, bios, khz, &c);
    if (two_stage) c.n2 = c.m2 = 1;
  } else {
    ret = mnp_double(limits, bios, khz, &c);
  }
  if (ret <= 0 || ret > UINT32_MAX) return 0;
  *out = c;
  return static_cast<uint32_t>(ret);
}

bool gt215_pll_calc(const NvPllLimits& info, uint32_t freq, bool fractional, NvPllCoefs* out, uint32_t* achieved_khz) {
  if (!freq || !info.refclk || !info.vco1.max_inputfreq || !info.vco1.min_inputfreq) return false;
  const int64_t refclk = info.refclk;
  int64_t P = info.vco1.max_freq / freq;
  if (P > info.max_p) P = info.max_p;
  if (P < info.min_p) P = info.min_p;
  if (P <= 0) return false;   // (nouveau would divide by zero)

  int64_t lM = (refclk + info.vco1.max_inputfreq) / info.vco1.max_inputfreq;
  lM = max64(lM, info.vco1.min_m);
  int64_t hM = (refclk + info.vco1.min_inputfreq) / info.vco1.min_inputfreq;
  hM = min64(hM, info.vco1.max_m);
  lM = min64(lM, hM);

  uint64_t best_err = UINT64_MAX;
  NvPllCoefs c;
  c.p = static_cast<int>(P);
  for (int64_t M = lM; M <= hM; ++M) {
    if (M <= 0) continue;
    // (wraps at 32 bits, as nouveau's u32)
    const uint32_t tmp = static_cast<uint32_t>(uint64_t{freq} * static_cast<uint64_t>(P) * static_cast<uint64_t>(M));
    int64_t N = tmp / refclk;
    int64_t fN = tmp % refclk;
    if (!fractional) {
      if (fN >= refclk / 2) ++N;
    } else {
      if (fN < refclk / 2) --N;
      fN = int64_t{tmp} - N * refclk;
    }
    if (N < info.vco1.min_n) continue;
    if (N > info.vco1.max_n) break;
    const uint64_t err = static_cast<uint64_t>(abs64(int64_t{freq} - (refclk * N / M / P)));
    if (err < best_err) {
      best_err = err;
      c.n = static_cast<int>(N);
      c.m = static_cast<int>(M);
    }
    if (fractional) {
      int64_t f = ((fN << 13) + refclk / 2) / refclk;
      c.fn = static_cast<int>((f - 4096) & 0xffff);
      *out = c;
      *achieved_khz = freq;
      return true;
    }
  }
  if (best_err == UINT64_MAX) return false;   // "unable to find matching pll values"
  *out = c;
  const int64_t achieved = refclk * c.n / c.m / P;
  *achieved_khz = achieved > 0 && achieved <= UINT32_MAX ? static_cast<uint32_t>(achieved) : 0;
  return true;
}

// --- Programming -------------------------------------------------------------------------------------------------------------------------

NvPllFamily pll_family(const Chip& chip) {
  if (chip.tegra || chip.family == Family::Unknown) return NvPllFamily::None;
  if (chip.card_type == 0x50) {
    switch (chip.chipset) {
      case 0xa3: case 0xa5: case 0xa8: case 0xaf: return NvPllFamily::Gt215;   // gt215 and mcp89
      default: return NvPllFamily::Nv50;                                       // nv50, g84, g98
    }
  }
  if (chip.chipset >= 0xc0 && chip.chipset < 0x140) return NvPllFamily::Gf100;   // gf100, gm107, gm200 (Kepler to Pascal)
  if (chip.chipset == 0x140) return NvPllFamily::Gv100;
  if (chip.family == Family::Turing) return NvPllFamily::Tu102;
  if (chip.family == Family::Ampere && chip.chipset >= 0x172) return NvPllFamily::Ga100;   // (GA100 has no display)
  return NvPllFamily::None;
}

const char* pll_family_name(NvPllFamily family) {
  switch (family) {
    case NvPllFamily::None: return "none";
    case NvPllFamily::Nv50: return "nv50";
    case NvPllFamily::Gt215: return "gt215";
    case NvPllFamily::Gf100: return "gf100";
    case NvPllFamily::Gv100: return "gv100";
    case NvPllFamily::Tu102: return "tu102";
    case NvPllFamily::Ga100: return "ga100";
  }
  return "?";
}

const char* pll_set_status_name(NvPllSetStatus status) {
  switch (status) {
    case NvPllSetStatus::Ok: return "ok";
    case NvPllSetStatus::NoFamily: return "PLLs of this chip are not programmed here";
    case NvPllSetStatus::BadLimits: return "no usable PLL limits";
    case NvPllSetStatus::NoCoefficients: return "no PLL coefficients fit";
    case NvPllSetStatus::Unimplemented: return "this PLL type is not programmed on this chip";
    case NvPllSetStatus::BadRegister: return "the PLL register is outside BAR0";
  }
  return "?";
}

namespace {

struct Writer {
  INvRegisters& r;
  INvInitTrace* trace;
  uint32_t* count;
  void wr(uint32_t reg, uint32_t value) {
    r.write32(reg, value);
    ++*count;
    if (trace) {
      NvInitWrite w;
      w.kind = NvInitAccess::Mmio;
      w.address = reg;
      w.value = value;
      trace->on_write(w);
    }
  }
  void mask(uint32_t reg, uint32_t clear, uint32_t set) { wr(reg, (r.read32(reg) & ~clear) | set); }
};

// The register block [reg, reg + span] must be inside BAR0 and 32-bit aligned.
bool reg_ok(uint32_t reg, uint32_t span) { return (reg & 3) == 0 && uint64_t{reg} + span + 4 <= kBar0Bytes; }

bool is_vpll01(uint32_t type) { return type == kPllVpll0 || type == kPllVpll1; }
bool is_vpll(uint32_t type) { return type >= kPllVpll0 && type <= kPllVpll3; }

}  // namespace

NvPllSetReport nv_pll_set(const NvPllSetInputs& in, uint32_t type, uint32_t khz) {
  NvPllSetReport rep;
  rep.family = pll_family(in.chip);
  if (!in.bios || !in.registers || rep.family == NvPllFamily::None) return rep;
  INvRegisters& r = *in.registers;
  const uint32_t crystal = in.crystal_khz ? in.crystal_khz : crystal_khz_from_strap(r.read32(kStrapRegister), in.chip);

  rep.error = parse_pll_limits(*in.bios, in.chip, crystal, type, &rep.limits);
  if (rep.error != NvPllError::None) {
    rep.status = NvPllSetStatus::BadLimits;
    return rep;
  }
  const NvPllLimits& info = rep.limits;
  Writer w{r, in.trace, &rep.writes};

  if (rep.family == NvPllFamily::Nv50) {
    rep.achieved_khz = nv04_pll_calc(info, bios_version(*in.bios), khz, true, &rep.coefs);
    if (!rep.achieved_khz) {
      rep.status = NvPllSetStatus::NoCoefficients;
      return rep;
    }
    const NvPllCoefs& c = rep.coefs;
    const uint32_t n1 = static_cast<uint32_t>(c.n1), m1 = static_cast<uint32_t>(c.m1), n2 = static_cast<uint32_t>(c.n2),
                   m2 = static_cast<uint32_t>(c.m2), p = static_cast<uint32_t>(c.p);
    if (!reg_ok(info.reg, 8)) {
      rep.status = NvPllSetStatus::BadRegister;
      return rep;
    }
    if (is_vpll01(info.type)) {
      w.wr(info.reg + 0, 0x10000611);
      w.mask(info.reg + 4, 0x00ff00ff, (m1 << 16) | n1);
      w.mask(info.reg + 8, 0x7fff00ff, (p << 28) | (m2 << 16) | n2);
    } else if (info.type == kPllMemory) {
      w.mask(info.reg + 0, 0x01ff0000, (p << 22) | (uint32_t{info.bias_p} << 19) | (p << 16));
      w.wr(info.reg + 4, (n1 << 8) | m1);
    } else {
      w.mask(info.reg + 0, 0x00070000, p << 16);
      w.wr(info.reg + 4, (n1 << 8) | m1);
    }
    rep.status = NvPllSetStatus::Ok;
    return rep;
  }

  if (!gt215_pll_calc(info, khz, true, &rep.coefs, &rep.achieved_khz)) {
    rep.status = NvPllSetStatus::NoCoefficients;
    return rep;
  }
  const NvPllCoefs& c = rep.coefs;
  const uint32_t n = static_cast<uint32_t>(c.n), m = static_cast<uint32_t>(c.m), p = static_cast<uint32_t>(c.p),
                 fn = static_cast<uint32_t>(c.fn) & 0xffff;
  switch (rep.family) {
    case NvPllFamily::Gt215:
      if (!is_vpll01(info.type)) break;
      if (!reg_ok(info.reg, 8)) {
        rep.status = NvPllSetStatus::BadRegister;
        return rep;
      }
      w.wr(info.reg + 0, 0x50000610);
      w.mask(info.reg + 4, 0x003fffff, (p << 16) | (m << 8) | n);
      w.wr(info.reg + 8, fn);
      rep.status = NvPllSetStatus::Ok;
      return rep;
    case NvPllFamily::Gf100:
      if (!is_vpll(info.type)) break;
      if (!reg_ok(info.reg, 0x10)) {
        rep.status = NvPllSetStatus::BadRegister;
        return rep;
      }
      w.mask(info.reg + 0x0c, 0x00000000, 0x00000100);
      w.wr(info.reg + 0x04, (p << 16) | (n << 8) | m);
      w.wr(info.reg + 0x10, fn << 16);
      rep.status = NvPllSetStatus::Ok;
      return rep;
    case NvPllFamily::Gv100: {
      if (!is_vpll(info.type)) break;
      // (nouveau takes the head from the type it was called with, which is wrong when that is a register; the table's type is used)
      const uint32_t head = info.type - kPllVpll0;
      w.wr(0x00ef10 + head * 0x40, fn << 16);
      w.wr(0x00ef04 + head * 0x40, (p << 16) | (n << 8) | m);
      rep.status = NvPllSetStatus::Ok;
      return rep;
    }
    case NvPllFamily::Tu102: {
      if (!is_vpll(info.type)) break;
      const uint32_t head = info.type - kPllVpll0;
      w.wr(0x00ef10 + head * 0x40, fn << 16);
      w.wr(0x00ef04 + head * 0x40, (p << 16) | (n << 8) | m);
      w.wr(0x00ef0c + head * 0x40, 0x00000900);     // "XXX" in nouveau
      w.wr(0x00ef00 + head * 0x40, 0x02000014);
      rep.status = NvPllSetStatus::Ok;
      return rep;
    }
    case NvPllFamily::Ga100: {
      if (!is_vpll(info.type)) break;
      const uint32_t head = info.type - kPllVpll0;
      w.wr(0x00ef00 + head * 0x40, 0x02080004);
      w.wr(0x00ef18 + head * 0x40, (n << 16) | fn);
      w.wr(0x00ef04 + head * 0x40, (p << 16) | m);
      w.wr(0x00e9c0 + head * 0x04, 0x00000001);
      rep.status = NvPllSetStatus::Ok;
      return rep;
    }
    default:
      break;
  }
  rep.status = NvPllSetStatus::Unimplemented;   // "%08x/%dKhz unimplemented"
  return rep;
}

NvPllInitHooks::NvPllInitHooks(const NvBios& bios, const Chip& chip, INvRegisters& registers, INvInitHooks* inner, uint32_t crystal_khz,
                               INvInitTrace* trace)
    : bios_(bios), chip_(chip), registers_(registers), inner_(inner), crystal_khz_(crystal_khz), trace_(trace) {}

bool NvPllInitHooks::set_pll(uint32_t type, uint32_t khz) {
  if (!crystal_khz_) crystal_khz_ = crystal_khz_from_strap(registers_.read32(kStrapRegister), chip_);
  NvPllSetInputs in;
  in.bios = &bios_;
  in.chip = chip_;
  in.registers = &registers_;
  in.crystal_khz = crystal_khz_;
  in.trace = trace_;
  last_ = nv_pll_set(in, type, khz);
  if (last_.ok()) ++programmed_;
  else ++failed_;
  return last_.ok();
}

}  // namespace nv
}  // namespace hardware
