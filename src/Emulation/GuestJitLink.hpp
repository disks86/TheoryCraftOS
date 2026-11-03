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

namespace emulation {

class GuestMemory;

// What translated code and its helpers share with the cpu that runs it, kept in the cpu's register State right after the registers so that the offsets stay small (the
// back ends reach the state with short displacements). 8-byte fields only: the translated code addresses them as words of the state block.
struct GuestJitLink {
  uint64_t fault = 0;                 // a guest memory access in the block failed: the details below
  uint64_t fault_address = 0;
  uint64_t fault_error = 0;           // page fault error code bits (2 = write)
  uint64_t count = 0;                 // instructions the block retired
  uint64_t flag_op = 0;               // x86: pending flags to compute, (kind << 8) | operand size ...
  uint64_t flag_a = 0, flag_b = 0;    // ... of these operands
  GuestMemory* memory = nullptr;      // the guest's memory for this run
  void* cpu = nullptr;                // the cpu that owns this state
};

}  // namespace emulation
