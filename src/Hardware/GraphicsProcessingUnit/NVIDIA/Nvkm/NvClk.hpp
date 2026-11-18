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

// The GPU's clocks, read only (nouveau's nvkm/subdev/clk gf100.c and gk104.c read paths, which GM107/GM108 also use): the reference crystal
// from the strap register, and the frequency each clock domain runs at, worked out from its source selection, its dividers and the PLL
// coefficients (N/M/P, and Kepler's fractional N) as the firmware left them. Nothing is reprogrammed: reclocking needs the video BIOS's
// performance tables and, from GM20x on, NVIDIA's signed PMU firmware.
//
// Fermi and Kepler/GM10x are decoded; Tesla (whose clock tree nouveau splits across nv50.c, g84.c, mcp77.c and gt215.c) and GM20x and later
// (no clock reading in nouveau without the GSP) report nothing but the crystal. A register value that would divide by zero, or a PLL that is
// off, reads as an empty Optional, never as a made-up number.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Utility/Optional.hpp"

namespace hardware {
namespace nv {

// The crystal (kHz) the strap register (NV_PEXTDEV_BOOT_0, 0x101000) selects: 13500, 14318, 27000 or 25000 (nouveau device/base.c).
uint32_t crystal_khz_from_strap(uint32_t strap, const Chip& chip);
constexpr uint32_t kStrapRegister = 0x101000;

enum class ClockDomain : uint8_t { Crystal, Core, Memory, Rop, Hub07, Hub06, Hub01, Pmu, Video };
const char* clock_domain_name(ClockDomain domain);

class NvClk {
 public:
  enum class Variant : uint8_t { None, Gf100, Gk104 };
  static Variant variant_for(const Chip& chip);

  NvClk(INvRegisters& regs, const Chip& chip);

  Variant variant() const { return variant_; }
  uint32_t crystal_khz();
  // kHz. Memory is the memory PLL's output (the command clock; GDDR5's data rate is a multiple of it).
  [[nodiscard]] Optional<uint32_t> read_khz(ClockDomain domain);

 private:
  uint32_t read_pll(uint32_t pll, uint32_t depth);
  uint32_t read_div(uint32_t doff, uint32_t dsrc, uint32_t dctl, uint32_t depth);
  uint32_t read_vco(uint32_t dsrc, uint32_t depth);
  uint32_t read_clk(uint32_t index);
  uint32_t read_mem();

  INvRegisters& regs_;
  Chip chip_;
  Variant variant_;
  uint32_t crystal_ = 0;
};

}  // namespace nv
}  // namespace hardware
