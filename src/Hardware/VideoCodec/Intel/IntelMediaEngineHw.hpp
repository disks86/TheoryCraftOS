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

#pragma once
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {

// What the first look at the media (video) engines of an Intel GPU found.
struct IntelMediaEngineStatus {
  bool probed = false;         // the registers were read at all (false: unsupported generation, or no usable register window)
  uint32_t vdbox_present = 0;  // bit n: video decode/encode box (VCS n) is on this part (not fused off)
  uint32_t vdbox_awake = 0;    // bit n: the box acknowledged a power request (it can be driven)
  uint32_t vdbox_count() const { return popcount(vdbox_awake); }

  static constexpr uint32_t popcount(uint32_t v) { uint32_t n = 0; for (; v; v &= v - 1) ++n; return n; }
};

// The registers of the media engines of Intel GPUs, Gen9.5 through Xe-LP/Xe-HPG (which keep them in the first 128 KiB of the device's
// register BAR, "GTTMMADR"). This is all the access there is so far: reading which video boxes exist and waking each one up and back to sleep.
// Submitting work to a box (the ring, the contexts, the GTT, the codec commands) is not done yet; see IntelMediaVideoEngine.
class IntelMediaEngineHw {
 public:
  // The registers the probe touches all lie below this offset; a smaller window is refused.
  static constexpr uint64_t kRegisterWindow = 0x20000;
  static constexpr uint32_t kWakeAttempts = 20000;  // reads of the acknowledge register, about a microsecond each

  // Does this class know the register layout of the generation? (Meteor Lake and later put the media engines on a separate tile with its own
  // register base; those are not probed.)
  static bool supported(IntelGpuArchitecture architecture);

  // Reads the fuses and wakes every box that exists, one at a time, putting each back to sleep before the next. Leaves nothing awake. Writes
  // only the forcewake registers.
  static IntelMediaEngineStatus probe(const MmioRegion& registers, IntelGpuArchitecture architecture);
};

}  // namespace hardware
