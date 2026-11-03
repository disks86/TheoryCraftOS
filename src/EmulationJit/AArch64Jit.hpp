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
#include "Allocators/IAllocator.hpp"
#include "Emulation/AArch64Cpu.hpp"
#include "EmulationJit/GuestJit.hpp"
#include "Jit/CodeMemory.hpp"
#include "Utility/SharedPointer.hpp"

namespace emulation {

// The JIT for the AArch64 guest: the integer core (add/subtract with flags, logical, move wide, bitfield, conditional select, multiply, variable shifts), branches, and the
// general-register loads and stores including pairs, as translated code. The flags are computed only when something reads them. Division, high multiplies, exclusives and atomics,
// floating point, SIMD, SVE and system instructions end a block and are interpreted. Null if memory ran out.
SharedPointer<IAArch64Jit> create_aarch64_jit(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options);

}  // namespace emulation
