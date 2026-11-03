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
#include "Emulation/GuestCpu.hpp"
#include "Jit/CodeMemory.hpp"

// The JIT of the CPU emulator (src/Emulation): hot basic blocks of a guest program are translated through the shared pipeline of src/Jit (front end -> IR -> back end for the
// CPU the kernel runs on -> code cache) and run instead of being interpreted. Each guest has a front end of its own (X86_64Jit, RiscV64Jit); everything the front end does
// not translate, the interpreter still runs, one instruction at a time, so the translated subset can grow without the rest changing. See docs/Emulation.md.
namespace emulation {

struct GuestJitOptions {
  uint32_t hot_threshold = 8;      // how often a block is reached before it is translated
  // x86-64 only. Flags that the code after a block certainly overwrites before reading are not computed at all (the usual compare-and-branch loop never computes a flag). The
  // guest cannot tell, except that RFLAGS held in the cpu between two runs of translated code may be stale at a point where the cpu stopped only because its instruction
  // budget ran out. With this set the flags are always exact at the end of a block (slower; for tests and debugging).
  bool exact_flags = false;
  uint32_t max_block = 32;      // instructions per block at most; fewer for debugging
};

// Gives `cpu` a JIT if its guest architecture has one (x86-64 and RISC-V; a no-op for the others, or when memory runs out). The usual call after create_guest_cpu() when the
// kernel has executable code memory (jit::kernel_code_memory()). One engine per guest address space: it caches translations of that program's code, and the threads cloned from
// `cpu` share it.
void enable_jit(IGuestCpu& cpu, allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options = GuestJitOptions());

}  // namespace emulation
