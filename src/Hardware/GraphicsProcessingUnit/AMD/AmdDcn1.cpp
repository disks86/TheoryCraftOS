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

// The register numbers and field masks come from AMD's headers (dcn_1_0_offset.h, dcn_1_0_sh_mask.h), see AmdDcn1Registers.inc. The sequence follows the Linux
// amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): dcn10_hubp.c, hubp1_program_surface_flip_and_addr() and hubp1_is_flip_pending().
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDcn1.hpp"

namespace hardware {
namespace amd_dcn1 {

namespace {
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDcn1Registers.inc"

// The register blocks of the Vega10 table, which Raven uses (vega10_ip_offset.h: DCN_BASE), by segment index.
constexpr uint32_t kDcnBase[5] = {0x00000012, 0x000000C0, 0x000034C0, 0, 0};

// ARGB8888 in DCSURF_SURFACE_CONFIG (hubp1_program_pixel_format), and the linear swizzle mode (DC_SW_LINEAR).
constexpr uint32_t kFormatArgb8888 = 8;
constexpr uint32_t kSwLinear = 0;

struct PipeRegisters {
  uint32_t primary_low;
  uint32_t primary_high;
  uint32_t inuse_low;
  uint32_t inuse_high;
  uint32_t pitch;
  uint32_t surface_control;
  uint32_t flip_control;
  uint32_t apt_low_msb;
  uint32_t apt_low_lsb;
  uint32_t apt_high_msb;
  uint32_t apt_high_lsb;
  uint32_t hubp_cntl;
  uint32_t surface_config;
  uint32_t tiling_config;
};

// Each register with the base of its block, one row per pipe.
#define R(name) (kDcnBase[name##_BASE_IDX] + name)
const PipeRegisters kPipeRegisters[kPipes] = {
    {R(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS), R(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), R(mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE), R(mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH), R(mmHUBPREQ0_DCSURF_SURFACE_PITCH), R(mmHUBPREQ0_DCSURF_SURFACE_CONTROL), R(mmHUBPREQ0_DCSURF_FLIP_CONTROL), R(mmHUBPREQ0_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_MSB), R(mmHUBPREQ0_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_LSB), R(mmHUBPREQ0_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_MSB), R(mmHUBPREQ0_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_LSB), R(mmHUBP0_DCHUBP_CNTL), R(mmHUBP0_DCSURF_SURFACE_CONFIG), R(mmHUBP0_DCSURF_TILING_CONFIG)},
    {R(mmHUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS), R(mmHUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), R(mmHUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE), R(mmHUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH), R(mmHUBPREQ1_DCSURF_SURFACE_PITCH), R(mmHUBPREQ1_DCSURF_SURFACE_CONTROL), R(mmHUBPREQ1_DCSURF_FLIP_CONTROL), R(mmHUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_MSB), R(mmHUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_LSB), R(mmHUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_MSB), R(mmHUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_LSB), R(mmHUBP1_DCHUBP_CNTL), R(mmHUBP1_DCSURF_SURFACE_CONFIG), R(mmHUBP1_DCSURF_TILING_CONFIG)},
    {R(mmHUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS), R(mmHUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), R(mmHUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE), R(mmHUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH), R(mmHUBPREQ2_DCSURF_SURFACE_PITCH), R(mmHUBPREQ2_DCSURF_SURFACE_CONTROL), R(mmHUBPREQ2_DCSURF_FLIP_CONTROL), R(mmHUBPREQ2_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_MSB), R(mmHUBPREQ2_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_LSB), R(mmHUBPREQ2_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_MSB), R(mmHUBPREQ2_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_LSB), R(mmHUBP2_DCHUBP_CNTL), R(mmHUBP2_DCSURF_SURFACE_CONFIG), R(mmHUBP2_DCSURF_TILING_CONFIG)},
    {R(mmHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS), R(mmHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), R(mmHUBPREQ3_DCSURF_SURFACE_EARLIEST_INUSE), R(mmHUBPREQ3_DCSURF_SURFACE_EARLIEST_INUSE_HIGH), R(mmHUBPREQ3_DCSURF_SURFACE_PITCH), R(mmHUBPREQ3_DCSURF_SURFACE_CONTROL), R(mmHUBPREQ3_DCSURF_FLIP_CONTROL), R(mmHUBPREQ3_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_MSB), R(mmHUBPREQ3_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_LSB), R(mmHUBPREQ3_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_MSB), R(mmHUBPREQ3_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_LSB), R(mmHUBP3_DCHUBP_CNTL), R(mmHUBP3_DCSURF_SURFACE_CONFIG), R(mmHUBP3_DCSURF_TILING_CONFIG)},
};
#undef R

constexpr uint32_t kHubpFormatShift = 0;  // SURFACE_PIXEL_FORMAT is the lowest field of DCSURF_SURFACE_CONFIG
}  // namespace

bool Plane::probe(uint64_t surface, uint32_t pitch_pixels) {
  active_ = false;
  if (surface == 0 || pitch_pixels == 0 || pitch_pixels > HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) return false;
  for (uint32_t p = 0; p < kPipes; ++p) {
    const PipeRegisters& g = kPipeRegisters[p];
    const uint32_t cntl = r_.read(g.hubp_cntl);
    if (cntl == 0xFFFFFFFFu || (cntl & HUBP0_DCHUBP_CNTL__HUBP_DISABLE_MASK)) continue;   // an engine that is off reads all ones
    if (((r_.read(g.surface_config) & HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) >> kHubpFormatShift) != kFormatArgb8888) continue;
    if ((r_.read(g.surface_config) & (HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE_MASK | HUBP0_DCSURF_SURFACE_CONFIG__H_MIRROR_EN_MASK)) != 0) continue;
    if ((r_.read(g.tiling_config) & HUBP0_DCSURF_TILING_CONFIG__SW_MODE_MASK) != kSwLinear) continue;
    if (r_.read(g.surface_control) & HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK) continue;
    if ((r_.read(g.pitch) & HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) + 1 != pitch_pixels) continue;
    pipe_ = p;
    if (this->surface() != surface) continue;
    if (r_.read(g.flip_control) & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) continue;
    // The system aperture, as dc programs it: a 36-bit page number (bits 35:32 in the MSB register, the rest in the LSB register).
    const uint64_t low = (uint64_t{r_.read(g.apt_low_msb) & HUBPREQ0_DCN_VM_SYSTEM_APERTURE_LOW_ADDR_MSB__MC_VM_SYSTEM_APERTURE_LOW_ADDR_MSB_MASK} << 32) | r_.read(g.apt_low_lsb);
    const uint64_t high = (uint64_t{r_.read(g.apt_high_msb) & HUBPREQ0_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR_MSB__MC_VM_SYSTEM_APERTURE_HIGH_ADDR_MSB_MASK} << 32) | r_.read(g.apt_high_lsb);
    aperture_low_ = low << 12;
    aperture_high_ = (high << 12) | 0xFFF;
    if (aperture_low_ > aperture_high_) continue;
    original_ = surface;
    active_ = true;
    return true;
  }
  return false;
}

bool Plane::in_aperture(uint64_t address, uint64_t bytes) const {
  return active_ && bytes != 0 && address >= aperture_low_ && address <= aperture_high_ && bytes - 1 <= aperture_high_ - address;
}

uint64_t Plane::surface() const {
  const PipeRegisters& g = kPipeRegisters[pipe_];
  const uint64_t high = r_.read(g.primary_high) & HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK;
  return (high << 32) | r_.read(g.primary_low);
}

bool Plane::underflow() const { return (r_.read(kPipeRegisters[pipe_].hubp_cntl) & HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS_MASK) != 0; }

// The engine reports the address it is fetching now (earliest in use) and whether a flip is still waiting for the next frame.
bool Plane::wait_in_use(uint64_t address) {
  const PipeRegisters& g = kPipeRegisters[pipe_];
  for (uint32_t i = 0; i < kLatchAttempts; ++i) {
    const uint64_t high = r_.read(g.inuse_high) & HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
    const uint64_t in_use = (high << 32) | r_.read(g.inuse_low);
    if (in_use == address && !(r_.read(g.flip_control) & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK)) return true;
  }
  return false;
}

bool Plane::set_surface(uint64_t address, uint64_t bytes) {
  if (!active_ || (address & (kSurfaceAlignment - 1)) || !in_aperture(address, bytes)) return false;
  const PipeRegisters& g = kPipeRegisters[pipe_];
  if (r_.read(g.flip_control) & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return false;
  const uint64_t before = surface();
  const bool underflowed = underflow();
  r_.write(g.primary_high, static_cast<uint32_t>(address >> 32));   // high first: the write of the low half latches the address
  r_.write(g.primary_low, static_cast<uint32_t>(address));
  if (wait_in_use(address) && (underflowed || !underflow())) return true;
  r_.write(g.primary_high, static_cast<uint32_t>(before >> 32));
  r_.write(g.primary_low, static_cast<uint32_t>(before));
  wait_in_use(before);
  return false;
}

bool Plane::restore() {
  if (!active_) return false;
  const PipeRegisters& g = kPipeRegisters[pipe_];
  if (surface() == original_) return true;
  r_.write(g.primary_high, static_cast<uint32_t>(original_ >> 32));
  r_.write(g.primary_low, static_cast<uint32_t>(original_));
  return wait_in_use(original_);
}

}  // namespace amd_dcn1
}  // namespace hardware
