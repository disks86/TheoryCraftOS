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


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): dce_v11_0.c (dce_v11_0_page_flip, dce_v11_0_crtc_do_set_base, crtc_offsets) and gmc_v8_0.c
// (the system aperture registers).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"

namespace hardware {
namespace amd_dce11 {

namespace {
// The register numbers and field masks this file uses, generated from dce_11_2_d.h, dce_11_2_sh_mask.h and gmc_8_1_d.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11Registers.inc"

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))
// (the field of a register value; written with the generated names spelled out)
#define FIELD(value, mask, shift) (((value) & (mask)) >> (shift))

// CRTCn_REGISTER_OFFSET (vid.h): the register blocks of the six controllers.
constexpr uint32_t kCrtcOffsets[kMaxCrtcs] = {0x1b9c - 0x1b9c, 0x1d9c - 0x1b9c, 0x1f9c - 0x1b9c, 0x419c - 0x1b9c, 0x439c - 0x1b9c, 0x459c - 0x1b9c};
// GRPH_CONTROL: the 32-bit ARGB8888 format of dce_v11_0_crtc_do_set_base (GRPH_DEPTH 2, GRPH_FORMAT 0) and linear addressing (array mode 0).
constexpr uint32_t kDepth32 = 2, kFormatArgb8888 = 0, kArrayLinear = 0;
}  // namespace

uint32_t Plane::offset() const { return kCrtcOffsets[crtc_]; }

bool Plane::probe(uint64_t surface, uint32_t pitch_pixels) {
  active_ = false;
  if (surface == 0 || (surface & (kSurfaceAlignment - 1)) != 0 || pitch_pixels == 0) return false;
  for (uint32_t i = 0; i < kMaxCrtcs; ++i) {
    const uint32_t off = kCrtcOffsets[i];
    const uint32_t crtc_control = r_.read(mmCRTC_CONTROL + off);
    const uint32_t grph_enable = r_.read(mmGRPH_ENABLE + off);
    if (!(crtc_control & CRTC_CONTROL__CRTC_MASTER_EN_MASK) || !(grph_enable & GRPH_ENABLE__GRPH_ENABLE_MASK)) continue;
    const uint32_t control = r_.read(mmGRPH_CONTROL + off);
    if (FIELD(control, GRPH_CONTROL__GRPH_DEPTH_MASK, GRPH_CONTROL__GRPH_DEPTH__SHIFT) != kDepth32 ||
        FIELD(control, GRPH_CONTROL__GRPH_FORMAT_MASK, GRPH_CONTROL__GRPH_FORMAT__SHIFT) != kFormatArgb8888 ||
        FIELD(control, GRPH_CONTROL__GRPH_ARRAY_MODE_MASK, GRPH_CONTROL__GRPH_ARRAY_MODE__SHIFT) != kArrayLinear)
      continue;
    if (r_.read(mmGRPH_SWAP_CNTL + off) != 0) continue;   // no byte swapping, no crossbar
    if (r_.read(mmGRPH_PITCH + off) != pitch_pixels) continue;
    const uint64_t address = (uint64_t{r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off)} << 32) | (r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS + off) & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
    if (address != surface) continue;
    crtc_ = i;
    original_ = surface;
    pitch_ = pitch_pixels;
    active_ = true;
    break;
  }
  if (!active_) return false;
  // The system aperture (gmc_v8_0_mc_program): pages, inclusive.
  aperture_low_ = uint64_t{r_.read(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR)} << 12;
  aperture_high_ = (uint64_t{r_.read(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR)} << 12) | 0xFFF;
  if (aperture_high_ <= aperture_low_) active_ = false;
  return active_;
}

bool Plane::in_aperture(uint64_t address, uint64_t bytes) const {
  return active_ && bytes != 0 && address >= aperture_low_ && address <= aperture_high_ && bytes - 1 <= aperture_high_ - address;
}

uint64_t Plane::surface() const {
  if (!active_) return 0;
  const uint32_t off = kCrtcOffsets[crtc_];
  return (uint64_t{r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off)} << 32) | (r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS + off) & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
}

// dce_v11_0_page_flip, asynchronous: the flip is taken at once, not at the next vertical blank.
bool Plane::flip(uint64_t address, uint32_t timeout_us) {
  const uint32_t off = offset();
  const uint32_t saved_flip_control = r_.read(mmGRPH_FLIP_CONTROL + off);
  // flip immediate for async, default is vsync
  uint32_t tmp = saved_flip_control;
  tmp = REG_SET_FIELD(tmp, GRPH_FLIP_CONTROL, GRPH_SURFACE_UPDATE_IMMEDIATE_EN, 1);
  r_.write(mmGRPH_FLIP_CONTROL + off, tmp);
  // update pitch
  r_.write(mmGRPH_PITCH + off, pitch_);
  // update the scanout addresses
  r_.write(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off, static_cast<uint32_t>(address >> 32));
  // writing to the low address triggers the update
  r_.write(mmGRPH_PRIMARY_SURFACE_ADDRESS + off, static_cast<uint32_t>(address));
  // post the write
  r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS + off);
  // the update is taken when it is no longer pending
  bool taken = false;
  const uint64_t begin = clock_.now_us();
  do {
    if (!(r_.read(mmGRPH_UPDATE + off) & GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK) && surface() == address) {
      taken = true;
      break;
    }
  } while (clock_.now_us() - begin < timeout_us);
  r_.write(mmGRPH_FLIP_CONTROL + off, saved_flip_control);   // the firmware's flip policy is kept
  return taken;
}

bool Plane::set_surface(uint64_t address, uint64_t bytes, uint32_t timeout_us) {
  if (!active_ || (address & (kSurfaceAlignment - 1)) != 0 || !in_aperture(address, bytes)) return false;
  const uint64_t previous = surface();
  if (flip(address, timeout_us)) return true;
  flip(previous, timeout_us);   // put the old one back
  return false;
}

bool Plane::restore(uint32_t timeout_us) {
  if (!active_) return false;
  return surface() == original_ || flip(original_, timeout_us);
}

}  // namespace amd_dce11
}  // namespace hardware
