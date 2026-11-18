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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvClk.hpp"

// After nouveau nvkm/subdev/clk/gf100.c and gk104.c (MIT license), written from memory of them; every register below is uncertain to that
// degree, and none was checked on hardware.

namespace hardware {
namespace nv {

namespace {

// ---- The registers, all in one place. ----
// The PLLs: control (bit 0 enable) at +0, coefficients at +4 (M bits 7:0, N bits 15:8, P bits 21:16), Kepler's fractional N at +0x10 (bits 31:16)
constexpr uint32_t kSpPll0 = 0x00e800, kSpPll1 = 0x00e820;                   // the two "special" PLLs, fed by the crystal
constexpr uint32_t kMemPll = 0x132000, kMemPllSrc = 0x132020;               // the memory PLL and the PLL feeding it
constexpr uint32_t kDomPll[4] = {0x137000, 0x137020, 0x137040, 0x1370e0};   // the per-domain PLLs (0x1370e0: the one shared by domains >= 7)
// The dividers: a source select (bits 1:0: 0 crystal / 108 MHz, 2 the 100 MHz reference, 3 a VCO; bit 8 picks SPPLL1) and a control (bit 31
// enables the divider, bits 5:0 its value, Fermi's/Kepler's second divider in bits 13:8)
constexpr uint32_t kDomSrc = 0x137160, kDomCtl = 0x1371d0;                  // per domain (index * 4)
constexpr uint32_t kDomPllRefSrc = 0x137120, kDomPllRefCtl = 0x137140;      // what feeds the domain PLLs
constexpr uint32_t kMemSrcRefSrc = 0x137320, kMemSrcRefCtl = 0x137330;      // what feeds the memory source PLL
constexpr uint32_t kMemDivSrc = 0x137300, kMemDivCtl = 0x137310;            // Fermi: the memory clock's divider path
constexpr uint32_t kPllSelect = 0x137100;                                   // bit per domain: the domain PLL (1) or the divider (0)
constexpr uint32_t kDomFinal = 0x137250;                                    // the last divider of each domain (index * 4)
constexpr uint32_t kGf100MemSelect = 0x1373f0;                              // bit 1: the memory PLL, else the divider
constexpr uint32_t kGk104MemSelect = 0x1373f4;                              // bits 3:0: 1 the source PLL, 2 the memory PLL
constexpr uint32_t kRefClockKhz = 100000, kAltCrystalKhz = 108000;
constexpr uint32_t kMaxDepth = 6;                                           // (the deepest chain is mpll -> mpllsrc -> div -> vco -> sppll)

}  // namespace

uint32_t crystal_khz_from_strap(uint32_t strap, const Chip& chip) {
  (void)chip;   // (the NV1x/NV2x exceptions of nouveau concern chips this driver does not do)
  switch (strap & 0x00400040) {
    case 0x00000000: return 13500;
    case 0x00000040: return 14318;
    case 0x00400000: return 27000;
    default: return 25000;
  }
}

const char* clock_domain_name(ClockDomain domain) {
  switch (domain) {
    case ClockDomain::Crystal: return "Crystal";
    case ClockDomain::Core: return "Graphics";
    case ClockDomain::Memory: return "Memory";
    case ClockDomain::Rop: return "ROP";
    case ClockDomain::Hub07: return "Hub (07)";
    case ClockDomain::Hub06: return "Hub (06)";
    case ClockDomain::Hub01: return "Hub (01)";
    case ClockDomain::Pmu: return "PMU";
    case ClockDomain::Video: return "Video decoder";
  }
  return "?";
}

NvClk::Variant NvClk::variant_for(const Chip& chip) {
  switch (chip.family) {
    case Family::Fermi: return Variant::Gf100;
    case Family::Kepler: return Variant::Gk104;
    case Family::Maxwell: return chip.chipset < 0x120 ? Variant::Gk104 : Variant::None;   // GM107/GM108 run gk104_clk in nouveau
    default: return Variant::None;
  }
}

NvClk::NvClk(INvRegisters& regs, const Chip& chip) : regs_(regs), chip_(chip), variant_(variant_for(chip)) {}

uint32_t NvClk::crystal_khz() {
  if (!crystal_) crystal_ = crystal_khz_from_strap(regs_.read32(kStrapRegister), chip_);
  return crystal_;
}

uint32_t NvClk::read_vco(uint32_t dsrc, uint32_t depth) {
  const uint32_t ssrc = regs_.read32(dsrc);
  return read_pll(ssrc & 0x00000100 ? kSpPll1 : kSpPll0, depth + 1);
}

uint32_t NvClk::read_div(uint32_t doff, uint32_t dsrc, uint32_t dctl, uint32_t depth) {
  if (depth > kMaxDepth) return 0;
  const uint32_t ssrc = regs_.read32(dsrc + doff * 4);
  const uint32_t sctl = regs_.read32(dctl + doff * 4);
  switch (ssrc & 0x00000003) {
    case 0:
      return (ssrc & 0x00030000) != 0x00030000 ? crystal_khz() : kAltCrystalKhz;
    case 2:
      return kRefClockKhz;
    case 3: {
      const uint32_t vco = read_vco(dsrc + doff * 4, depth);
      if (sctl & 0x80000000) return static_cast<uint32_t>(uint64_t{vco} * 2 / ((sctl & 0x3f) + 2));
      return vco;
    }
    default:
      return 0;
  }
}

uint32_t NvClk::read_pll(uint32_t pll, uint32_t depth) {
  if (depth > kMaxDepth) return 0;
  const uint32_t ctrl = regs_.read32(pll);
  const uint32_t coef = regs_.read32(pll + 4);
  uint32_t P = (coef & 0x003f0000) >> 16;
  const uint32_t N = (coef & 0x0000ff00) >> 8;
  const uint32_t M = coef & 0x000000ff;
  uint32_t fN = 0xf000;   // (Kepler: makes the fractional term vanish, (fN + 4096) wrapping to 0 in 16 bits)
  if (!(ctrl & 1)) return 0;
  uint32_t sclk = 0;
  if (pll == kSpPll0 || pll == kSpPll1) {
    sclk = crystal_khz();
    P = 1;
  } else if (pll == kMemPll) {
    sclk = read_pll(kMemPllSrc, depth + 1);
    if (variant_ == Variant::Gk104) P = coef & 0x10000000 ? 2 : 1;
  } else if (pll == kMemPllSrc) {
    sclk = read_div(0, kMemSrcRefSrc, kMemSrcRefCtl, depth + 1);
    if (variant_ == Variant::Gk104) fN = regs_.read32(pll + 0x10) >> 16;
  } else if (pll == kDomPll[0] || pll == kDomPll[1] || pll == kDomPll[2] || pll == kDomPll[3]) {
    sclk = read_div((pll & 0xff) / 0x20, kDomPllRefSrc, kDomPllRefCtl, depth + 1);
  } else {
    return 0;
  }
  if (P == 0) P = 1;
  if (M == 0) return 0;   // (garbage: nouveau would divide by zero)
  uint64_t out = uint64_t{sclk} * N;
  if (variant_ == Variant::Gk104) out += (uint64_t{static_cast<uint16_t>(fN + 4096)} * sclk) >> 13;
  out /= uint64_t{M} * P;
  return out > 0xffffffffu ? 0 : static_cast<uint32_t>(out);
}

uint32_t NvClk::read_clk(uint32_t idx) {
  const uint32_t sctl = regs_.read32(kDomFinal + idx * 4);
  const uint32_t ssel = regs_.read32(kPllSelect);
  uint32_t sclk = 0, sdiv = 0;
  if (variant_ == Variant::Gf100) {
    if (ssel & (1u << idx)) {
      sclk = read_pll(idx < 7 ? 0x137000 + idx * 0x20 : kDomPll[3], 0);
      sdiv = ((sctl & 0x00003f00) >> 8) + 2;
    } else {
      sclk = read_div(idx, kDomSrc, kDomCtl, 0);
      sdiv = (sctl & 0x0000003f) + 2;
    }
    return sctl & 0x80000000 ? static_cast<uint32_t>(uint64_t{sclk} * 2 / sdiv) : sclk;
  }
  // gk104
  bool second = false;
  if (idx < 7) {
    if (ssel & (1u << idx)) {
      sclk = read_pll(0x137000 + idx * 0x20, 0);
      second = true;
    } else {
      sclk = read_div(idx, kDomSrc, kDomCtl, 0);
    }
  } else {
    const uint32_t ssrc = regs_.read32(kDomSrc + idx * 4);
    sclk = read_div(idx, kDomSrc, kDomCtl, 0);
    if ((ssrc & 0x3) == 0x3 && (ssrc & 0x00000100)) {
      if (ssrc & 0x40000000) sclk = read_pll(kDomPll[3], 0);
      second = true;
    }
  }
  if (sctl & 0x80000000) {
    sdiv = second ? ((sctl & 0x00003f00) >> 8) + 2 : (sctl & 0x3f) + 2;
    return static_cast<uint32_t>(uint64_t{sclk} * 2 / sdiv);
  }
  return sclk;
}

uint32_t NvClk::read_mem() {
  if (variant_ == Variant::Gf100) {
    if (regs_.read32(kGf100MemSelect) & 0x2) return read_pll(kMemPll, 0);
    return read_div(0, kMemDivSrc, kMemDivCtl, 0);
  }
  switch (regs_.read32(kGk104MemSelect) & 0xf) {
    case 1: return read_pll(kMemPllSrc, 0);
    case 2: return read_pll(kMemPll, 0);
    default: return 0;
  }
}

Optional<uint32_t> NvClk::read_khz(ClockDomain domain) {
  if (domain == ClockDomain::Crystal) return crystal_khz();
  if (variant_ == Variant::None) return {};
  uint32_t khz = 0;
  switch (domain) {
    case ClockDomain::Core: khz = read_clk(0x00); break;
    case ClockDomain::Rop: khz = read_clk(0x01); break;
    case ClockDomain::Hub07: khz = read_clk(0x02); break;
    case ClockDomain::Hub06: khz = read_clk(0x07); break;
    case ClockDomain::Hub01: khz = read_clk(0x08); break;
    case ClockDomain::Pmu: khz = read_clk(0x0c); break;
    case ClockDomain::Video: khz = read_clk(0x0e); break;
    case ClockDomain::Memory: khz = read_mem(); break;
    default: break;
  }
  if (khz == 0) return {};
  return khz;
}

}  // namespace nv
}  // namespace hardware
