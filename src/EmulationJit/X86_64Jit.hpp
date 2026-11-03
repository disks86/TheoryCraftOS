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
#include "Emulation/X86_64Cpu.hpp"
#include "EmulationJit/GuestJit.hpp"
#include "Jit/CodeMemory.hpp"
#include "Utility/SharedPointer.hpp"

// The JIT for the x86-64 guest. It plugs the integer subset of the guest instruction set into the shared pipeline of src/Jit: a front end turns the guest code at a
// hot address into one IR block, the back end of the CPU the kernel runs on turns that into machine code, and the cpu (X86_64Cpu::run) enters the code through a
// code cache before it interprets an instruction. See docs/Emulation.md, "Translated code".
namespace emulation {

// The x86-64 engine alone (enable_jit() in GuestJit.hpp is the usual way in). Null if memory ran out.
SharedPointer<IX86Jit> create_x86_jit(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options);

}  // namespace emulation
