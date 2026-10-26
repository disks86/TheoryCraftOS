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

// The primary plane of the display engine of Raven-family APUs (DCN 1.0: Ryzen 2000/3000 "Raven Ridge", "Picasso", "Raven2"), for one thing: pointing it at another
// framebuffer. The firmware has lit the screen, and everything about how the picture is fetched and shown (pixel format, tiling, pitch, the request and timing
// registers the Display Mode Library computes, the scaler, the timings) is left exactly as the firmware set it. Only the plane's surface address is written, in
// the order amdgpu's hubp1_program_surface_flip_and_addr does it (high half first, the low half latches), and the display engine's answer is read back.
//
// It will not touch a plane it does not recognise: probe() looks for the plane that shows the firmware's own framebuffer (same address, same pitch, linear 32-bit
// ARGB8888, no compression) and refuses anything else. The new address has to lie inside the VM system aperture, where the display engine reads physical video
// memory without translation. If the engine does not take the address, the old one is put back.
//
// Register numbers are generated from AMD's own headers (AmdDcn1Registers.inc); the sequence is ported from the Linux amdgpu driver (dcn10_hubp.c). It has not been run on a
// real APU: the host tests drive it against a simulated register file.
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {
namespace amd_dcn1 {

constexpr uint32_t kPipes = 4;
// The surface address must be aligned to this (the register holds the byte address; the engine fetches in 256-byte units).
constexpr uint64_t kSurfaceAlignment = 256;
// Reads of the in-use address (about a microsecond each over PCI Express) before the engine is declared not to have taken a new address: far more than a frame.
constexpr uint32_t kLatchAttempts = 200000;

class Plane {
 public:
  explicit Plane(IVcnRegisters& registers) : r_(registers) {}

  // Looks for the plane that shows `surface` (the GPU's address of the firmware's framebuffer) with `pitch_pixels` between rows. False if no plane matches, or the
  // one that does is not a linear, uncompressed 32-bit ARGB8888 plane that is on.
  bool probe(uint64_t surface, uint32_t pitch_pixels);
  bool active() const { return active_; }
  uint32_t pipe() const { return pipe_; }
  uint64_t original_surface() const { return original_; }
  // The system aperture as the registers hold it: byte addresses, both ends inclusive.
  uint64_t aperture_low() const { return aperture_low_; }
  uint64_t aperture_high() const { return aperture_high_; }
  // Is [address, address + bytes) inside the aperture?
  bool in_aperture(uint64_t address, uint64_t bytes) const;
  // The address the plane is fetching now (the register, not what was asked).
  uint64_t surface() const;

  // Points the plane at `address` (aligned, `bytes` of picture inside the aperture). Waits for the display engine to use it; if it does not (or the engine reports an
  // underflow it did not before), the previous address is put back and false returned.
  bool set_surface(uint64_t address, uint64_t bytes);
  // Back to the firmware's framebuffer.
  bool restore();

 private:
  bool wait_in_use(uint64_t address);
  bool underflow() const;

  IVcnRegisters& r_;
  bool active_ = false;
  uint32_t pipe_ = 0;
  uint64_t original_ = 0;
  uint64_t aperture_low_ = 0, aperture_high_ = 0;
};

}  // namespace amd_dcn1
}  // namespace hardware
