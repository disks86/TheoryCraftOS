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
#include "Kernel/Thread.hpp"
#include "Kernel/UserEntry.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/UserPageTables.hpp"
#include "Process/UserProgram.hpp"
#include "Utility/UniquePointer.hpp"
#include "Utility/ArrayView.hpp"

namespace kernel {

namespace usermode {

// Sets up everything a user-mode process needs, once, after kernel::start(): the trampoline (entry stubs, interrupt table), the SYSCALL registers and
// the per-core tables on every core. False if the machine or the memory does not allow it (nothing is changed then, except memory already allocated).
bool init(allocators::IAllocator& allocator);
bool ready();
// Addresses a user address space must stay away from (pages the CPU needs on the user page tables): hand them to AddressSpace::set_reserved().
ArrayView<const process::AddressRange> reserved_ranges();

// One running program: its page tables, its thread and how it ended.
struct Task {
  Task(allocators::IAllocator& allocator, process::UserProgram& program, bool nx) : program(program), tables(allocator, nx) {}
  process::UserProgram& program;
  process::UserPageTables tables;
  Thread* thread = nullptr;
  bool faulted = false;        // the CPU stopped it (page fault, invalid instruction, ...)
  uint64_t fault_vector = 0, fault_address = 0;
};

// Starts the program's first thread in ring 3. `program` must be loaded (with the reserved ranges set on its address space before loading) and must stay
// alive until the task is gone. Null if there is no memory or user mode is not ready.
UniquePointer<Task> spawn(process::UserProgram& program, allocators::IAllocator& allocator, const char* name);
// Waits for the program to end (timeout_us 0 = forever); false on timeout. The exit code is the program's own, or 128 + the vector of the fault that
// stopped it. Call once; afterwards the task may be destroyed.
bool wait(Task& task, uint32_t* exit_code, uint64_t timeout_us = 0);

}  // namespace usermode

}  // namespace kernel
