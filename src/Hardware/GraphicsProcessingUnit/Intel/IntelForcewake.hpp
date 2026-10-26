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
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"

namespace hardware {
namespace intel_gt {

// Forcewake: the GPU powers parts of itself down on its own; a part's registers are only reliable while a request for power is acknowledged. Gen9 has a
// domain per part: the GT (global registers such as the PAT), the render engine, and the media engines (the video rings at 0x12000 and 0x1c000+).
// A request is a masked write of bit 0 of the domain's register; the acknowledge register shows when it is granted. Registers from the i915 driver
// (gt/intel_gt_regs.h, intel_uncore.c).
// MediaVdbox0 is the well of the first video box on Gen11 and later (Gen9 has one well for all of them).
enum class ForcewakeDomain : uint8_t { Gt, Render, Media, MediaVdbox0 };

class Forcewake {
 public:
  static constexpr uint32_t kAckAttempts = 50000;  // reads of the acknowledge register, about a microsecond each (the driver waits up to 50 ms)

  // Asks for power and waits for the acknowledge. On failure the request is withdrawn again. Calls are counted per well (IRegisterBank::forcewake_state): while
  // someone else holds the well, acquire returns at once and release does not let go, so independent drivers of one GPU can share its wells.
  static bool acquire(IRegisterBank& registers, ForcewakeDomain domain, uint32_t attempts = kAckAttempts);
  static void release(IRegisterBank& registers, ForcewakeDomain domain);

  static uint32_t request_register(ForcewakeDomain domain);
  static uint32_t ack_register(ForcewakeDomain domain);
};

}  // namespace intel_gt
}  // namespace hardware
