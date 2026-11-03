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
#include "Emulation/RiscV64Cpu.hpp"
#include "EmulationJit/GuestJit.hpp"
#include "Jit/CodeMemory.hpp"
#include "Utility/SharedPointer.hpp"

namespace emulation {

// The JIT for the RISC-V guest: RV64I and the multiply, in 32-bit and compressed encodings, as translated code (loads and stores go through the checked guest memory). The
// divide, high multiply, atomics, floating point, vector, bit-manipulation and system instructions end a block and are interpreted. Null if memory ran out.
SharedPointer<IRiscVJit> create_riscv64_jit(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options);

}  // namespace emulation
