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


// The primary plane of the display engine (DCE 11.2) of a Radeon RX Vega M, for one thing: pointing it at another framebuffer. This is the dGPU counterpart of
// amd_dcn1::Plane (AmdDcn1.hpp) for the older display block of the Polaris family: the firmware has lit the screen (the card's video BIOS ran at POST and the UEFI graphics
// output is on it), and everything about how the picture is made and shown (the timings, the PLLs, the encoders and the link) is left exactly as the firmware set it. Only
// the graphics surface's address is written, the way amdgpu's dce_v11_0_page_flip does it (the high half, then the low half, which triggers the update), and the engine's
// answer (GRPH_UPDATE: is the update still pending) is read back.
//
// Setting a mode of its own would need the video BIOS's command tables (the PLL, the DIG encoder and the transmitter are programmed through AtomBIOS in the driver): there is
// no interpreter for them here, so there is no mode setting on this card, only this. It does not touch a CRTC it does not recognise: probe() looks for the one that shows the
// firmware's framebuffer (same GPU address, same pitch, 32-bit ARGB8888, linear, no byte swapping) and refuses anything else. The new address has to lie inside the system
// aperture the memory controller was given (AmdGmc8), where the display reads physical video memory without translation. If the engine does not take the address, the old one
// is put back.
//
// Register numbers are generated from AMD's own headers (AmdDce11Registers.inc); the sequence is ported from the Linux amdgpu driver (dce_v11_0.c, from kernel 6.12: the legacy
// display code was later removed in favour of the display core). Not run on a real card: the host tests drive it against a simulated register file.
#pragma once
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {
namespace amd_dce11 {

constexpr uint32_t kMaxCrtcs = 6;                  // the Polaris 10 and the VEGAM; the Polaris 11 and 12 have 5
constexpr uint64_t kSurfaceAlignment = 256;        // GRPH_PRIMARY_SURFACE_ADDRESS holds bits 31:8
constexpr uint32_t kDefaultTimeoutUs = 100000;     // far more than a frame

class Plane {
 public:
  Plane(IVcnRegisters& registers, const IClock& clock) : r_(registers), clock_(clock) {}

  // Looks for the CRTC whose graphics surface is `surface` (the GPU's address of the firmware's framebuffer) with `pitch_pixels` between rows. False if none matches, or
  // the one that does is not on, linear, ARGB8888 and unswapped.
  bool probe(uint64_t surface, uint32_t pitch_pixels);
  bool active() const { return active_; }
  uint32_t crtc() const { return crtc_; }
  uint64_t original_surface() const { return original_; }
  // The system aperture as the registers hold it: byte addresses, both ends inclusive (low is the first byte, high the last of the last page).
  uint64_t aperture_low() const { return aperture_low_; }
  uint64_t aperture_high() const { return aperture_high_; }
  bool in_aperture(uint64_t address, uint64_t bytes) const;
  // The address the graphics surface fetches now (the register, not what was asked).
  uint64_t surface() const;

  // Points the surface at `address` (aligned, `bytes` of picture inside the aperture) with an immediate flip. Waits until the engine no longer has an update pending; if it
  // never takes it, the previous address is put back and false returned.
  bool set_surface(uint64_t address, uint64_t bytes, uint32_t timeout_us = kDefaultTimeoutUs);
  // Back to the firmware's framebuffer.
  bool restore(uint32_t timeout_us = kDefaultTimeoutUs);

 private:
  uint32_t offset() const;
  bool flip(uint64_t address, uint32_t timeout_us);

  IVcnRegisters& r_;
  const IClock& clock_;
  bool active_ = false;
  uint32_t crtc_ = 0;
  uint64_t original_ = 0;
  uint32_t pitch_ = 0;
  uint64_t aperture_low_ = 0, aperture_high_ = 0;
};

}  // namespace amd_dce11
}  // namespace hardware
