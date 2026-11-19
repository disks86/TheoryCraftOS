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

// PLL programming for the devinit scripts (INIT_PLL, INIT_PLL2, INIT_PLL_INDIRECT, INIT_IO_RESTRICT_PLL*): nouveau's
//   - nvkm/subdev/bios/pll.c (nvbios_pll_parse): the video BIOS's PLL limits table (BIT 'C', or the BMP 5.24+ pointer), versions 0x10,
//     0x11, 0x20, 0x21, 0x30, 0x40 and 0x50, and the nv04/nv40/nv50/g84 type <-> register maps for the tables that do not name registers;
//   - nvkm/subdev/clk/pllnv04.c (nv04_pll_calc: single and two-stage M/N/P search) and pllgt215.c (gt215_pll_calc: M/N/P with the
//     fractional N of the GT215+ VPLLs), as pure functions;
//   - the devinit pll_set of each family: nv50.c (NV50, G84, G98), gt215.c (GT215, MCP89), gf100.c (Fermi to Pascal: GF100, GM107, GM200
//     use it), gv100.c (Volta), tu102.c (Turing), ga100.c (GA10x: nouveau's ga102 chipsets take ga100_devinit);
//   - NvPllInitHooks: an INvInitHooks whose set_pll does the above, so that NvDevinit::post's caller can hand it to the interpreter.
//
// The image is untrusted: every byte goes through NvBios::rd08/16/32, a read outside the image fails the parse (Truncated; nouveau reads 0),
// and the calculators guard every division, shift and product that a hostile table could make overflow or divide by zero (64-bit
// arithmetic, at most 2^16 for P; nouveau's int arithmetic gives the same results for every table a real board has). Registers are only
// written through INvRegisters, and only inside BAR0.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInitScript.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"

namespace hardware {
namespace nv {

// enum nvbios_pll_type: the types of the 0x40 table, used on every chip where a PLL is named by what it is rather than by its register.
// A set_pll type above kPllMax is a register address.
constexpr uint32_t kPllCore = 0x01, kPllShader = 0x02, kPllUnk03 = 0x03, kPllMemory = 0x04, kPllVdec = 0x05, kPllUnk40 = 0x40,
                   kPllUnk41 = 0x41, kPllUnk42 = 0x42, kPllVpll0 = 0x80, kPllVpll1 = 0x81, kPllVpll2 = 0x82, kPllVpll3 = 0x83,
                   kPllMax = 0xff;

// The image's version bytes (nvkm_bios.version): BIT 'i' +3 / +2 (major / chip), or the BMP's +13 / +12.
struct NvBiosVersion {
  uint8_t major = 0, chip = 0;
};
[[nodiscard]] NvBiosVersion bios_version(const NvBios& bios);

// struct nvbios_pll.
struct NvPllVco {
  uint32_t min_freq = 0, max_freq = 0;              // kHz
  uint32_t min_inputfreq = 0, max_inputfreq = 0;    // kHz
  uint8_t min_m = 0, max_m = 0, min_n = 0, max_n = 0;
};

struct NvPllLimits {
  uint32_t type = 0;          // kPll*
  uint32_t reg = 0;           // the PLL's register (0 with a 0x50 table, which has none)
  uint32_t refclk = 0;        // kHz (the table's, or the crystal)
  uint8_t min_p = 0, max_p = 0, bias_p = 0;
  uint8_t max_p_usable = 0;   // pre-NV50 VPLLs: log2 P of 7 is no different from 6 (see nouveau's comment)
  NvPllVco vco1, vco2;
  // Where it came from.
  uint8_t table_version = 0;  // 0: no limits table (the BMP defaults)
  uint32_t entry = 0;         // the image offset the limits were read from
};

enum class NvPllError : uint8_t {
  None,
  NotFound,         // the table has no entry for this type / register (nouveau's -ENOENT)
  UnknownVersion,   // a table version nvbios_pll_parse does not know (-EINVAL)
  Truncated,        // the table or an entry runs out of the image
};
const char* pll_error_name(NvPllError error);

// The PLL limits table: its offset and header (pll_limits_table). False if there is none (version 0: nouveau then uses the BMP defaults).
struct NvPllTable {
  uint32_t offset = 0;
  uint8_t version = 0, header = 0, count = 0, length = 0;
};
[[nodiscard]] bool find_pll_table(const NvBios& bios, NvPllTable* out, bool* truncated);

// nvbios_pll_parse. `type`: a kPll* value or (above kPllMax) a register. `crystal_khz`: the device's crystal (crystal_khz_from_strap),
// used when the table gives no reference clock (a reference clock of 0 makes the calculators fail). Not done: NV4x C51 (bios chip
// 0x51)'s VPLL reference clock override (it needs the VGA CRTC register 0x27), irrelevant to the chips this driver brings up.
[[nodiscard]] NvPllError parse_pll_limits(const NvBios& bios, const Chip& chip, uint32_t crystal_khz, uint32_t type, NvPllLimits* out);

// --- The calculators ---------------------------------------------------------------------------------------------------------------------

struct NvPllCoefs {
  int n1 = 0, m1 = 0, n2 = 0, m2 = 0;   // nv04 (single stage: n2 = m2 = 1)
  int p = 0;                            // log2 P for nv04, P itself for gt215
  int n = 0, m = 0, fn = 0;             // gt215 (fn: the 16-bit fractional N, 0 when not asked for)
};

// nv04_pll_calc: the coefficients for `khz` and the clock they give (kHz), 0 if none fit. `two_stage`: the caller has an N2/M2 to set
// (nouveau passes N2); the search is two-stage when it does and the table has a second VCO. `bios` is the image's version (the pre-NV50
// M and N2/M2 rules depend on it).
[[nodiscard]] uint32_t nv04_pll_calc(const NvPllLimits& limits, NvBiosVersion bios, uint32_t khz, bool two_stage, NvPllCoefs* out);

// gt215_pll_calc: N, M and P (and with `fractional`, the fractional N of the first M that fits, as nouveau's callers all ask for: then
// the result is `khz` itself). False if nothing fits (nouveau's -EINVAL) or the limits would divide by zero; *achieved_khz the clock.
[[nodiscard]] bool gt215_pll_calc(const NvPllLimits& limits, uint32_t khz, bool fractional, NvPllCoefs* out, uint32_t* achieved_khz);

// --- Programming (nvkm_devinit_pll_set) --------------------------------------------------------------------------------------------------

enum class NvPllFamily : uint8_t {
  None,     // pre-NV50 (nv04_devinit_pll_set and its setPLL_single/double are not done), GA100, Ada and later (GSP), Tegra
  Nv50,     // nv50_devinit_pll_set: NV50, G84, G98 (nv04_pll_calc)
  Gt215,    // gt215_devinit_pll_set: GT215/216/218, MCP89 (VPLLs only)
  Gf100,    // gf100_devinit_pll_set: GF100 .. GP10x (GK, GM107 and GM200 use it; VPLLs only)
  Gv100,    // gv100_devinit_pll_set: GV100 (VPLLs at 0x00ef00 + head * 0x40)
  Tu102,    // tu102_devinit_pll_set: TU10x (as GV100, then 0x00ef0c = 0x900 and 0x00ef00 = 0x02000014: nouveau's "XXX")
  Ga100,    // ga100_devinit_pll_set: GA102..GA107 (0x00ef00 = 0x02080004, N << 16 | fN at 0x00ef18, P << 16 | M at 0x00ef04, 0x00e9c0 + head * 4 = 1)
};
[[nodiscard]] NvPllFamily pll_family(const Chip& chip);
const char* pll_family_name(NvPllFamily family);

enum class NvPllSetStatus : uint8_t {
  Ok,
  NoFamily,         // this chip's PLLs are not programmed here
  BadLimits,        // parse_pll_limits failed (NvPllSetReport::error says how)
  NoCoefficients,   // the calculator found nothing
  Unimplemented,    // the family does not program this PLL type (nouveau: "unimplemented", -EINVAL)
  BadRegister,      // the table names a register outside BAR0 (or not 32-bit aligned): nothing written
};
const char* pll_set_status_name(NvPllSetStatus status);

struct NvPllSetReport {
  NvPllSetStatus status = NvPllSetStatus::NoFamily;
  NvPllError error = NvPllError::None;
  NvPllFamily family = NvPllFamily::None;
  NvPllLimits limits;
  NvPllCoefs coefs;
  uint32_t achieved_khz = 0;
  uint32_t writes = 0;
  bool ok() const { return status == NvPllSetStatus::Ok; }
};

struct NvPllSetInputs {
  const NvBios* bios = nullptr;
  Chip chip;
  INvRegisters* registers = nullptr;
  uint32_t crystal_khz = 0;          // 0: read the strap (0x101000) once
  INvInitTrace* trace = nullptr;     // each register write (NvInitAccess::Mmio), may be nullptr
};

// nvkm_devinit_pll_set for the chip's family: parse the limits, compute the coefficients, write them.
[[nodiscard]] NvPllSetReport nv_pll_set(const NvPllSetInputs& inputs, uint32_t type, uint32_t khz);

// The INvInitHooks for NvDevinit::post / NvInitScript: set_pll programs the PLL as above; I2C, AUX, GPIO resets and meminit go to `inner`
// (nullptr: none, the defaults of INvInitHooks). All references must outlive the object.
class NvPllInitHooks final : public INvInitHooks {
 public:
  NvPllInitHooks(const NvBios& bios, const Chip& chip, INvRegisters& registers, INvInitHooks* inner = nullptr, uint32_t crystal_khz = 0,
                 INvInitTrace* trace = nullptr);

  [[nodiscard]] bool set_pll(uint32_t type, uint32_t khz) override;
  II2cBus* i2c_bus(uint8_t index) override { return inner_ ? inner_->i2c_bus(index) : nullptr; }
  IDpcdAux* aux_channel(uint8_t i2c_index) override { return inner_ ? inner_->aux_channel(i2c_index) : nullptr; }
  [[nodiscard]] bool gpio_reset(uint8_t function) override { return inner_ && inner_->gpio_reset(function); }
  [[nodiscard]] bool meminit() override { return inner_ && inner_->meminit(); }

  const NvPllSetReport& last() const { return last_; }
  uint32_t programmed() const { return programmed_; }
  uint32_t failed() const { return failed_; }

 private:
  const NvBios& bios_;
  Chip chip_;
  INvRegisters& registers_;
  INvInitHooks* inner_;
  uint32_t crystal_khz_;
  INvInitTrace* trace_;
  NvPllSetReport last_;
  uint32_t programmed_ = 0, failed_ = 0;
};

}  // namespace nv
}  // namespace hardware
