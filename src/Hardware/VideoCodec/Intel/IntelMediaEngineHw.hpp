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

// The registers of the media engines of Intel GPUs. Gen9.5 through Xe-LP/Xe-HPG (Arc Alchemist) keep them in the first 128 KiB of the device's
// register BAR, "GTTMMADR". Xe-LPG (Meteor Lake and Arrow Lake graphics) and Xe2 (Lunar Lake, Arc Battlemage) put the media engines on a standalone media
// tile whose registers sit in the same BAR at an offset of 0x380000, with the same layout as before (the i915 and Xe drivers add the offset to every
// media register); the probe reads that tile the same way. This is all the access there is so far: reading which video boxes exist and waking each one up and back to sleep.
// Submitting work to a box (the ring, the contexts, the GTT, the codec commands) is not done yet; see IntelMediaVideoEngine.
class IntelMediaEngineHw {
 public:
  // The registers the probe touches all lie within this many bytes of the start of the engines' register block (the BAR for most generations, the
  // media tile for the standalone-media ones).
  static constexpr uint64_t kRegisterWindow = 0x20000;
  // Where the media tile's registers start in the BAR (Xe-LPG and Xe2).
  static constexpr uint64_t kMediaTileBase = 0x380000;
  static constexpr uint32_t kWakeAttempts = 20000;  // reads of the acknowledge register, about a microsecond each

  // Does this class know the register layout of the generation? (Xe3 and the compute-only Xe-HPC are not probed.)
  static bool supported(IntelGpuArchitecture architecture);
  // Where the engines' register block starts in the BAR: 0, or kMediaTileBase on a standalone media tile.
  static uint64_t register_base(IntelGpuArchitecture architecture);
  // How much of the BAR the probe needs mapped for this generation (register_base + kRegisterWindow); 0 for a generation it does not probe.
  static uint64_t register_window(IntelGpuArchitecture architecture);

  // `registers` is the BAR from its start, at least register_window(architecture) bytes. Reads the fuses and wakes every box that exists, one at a time, putting each back to sleep before the next. Leaves nothing awake. Writes
  // only the forcewake registers.
  static IntelMediaEngineStatus probe(const MmioRegion& registers, IntelGpuArchitecture architecture);
};

}  // namespace hardware
