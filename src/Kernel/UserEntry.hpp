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
#include "Kernel/Interrupts.hpp"

namespace kernel {

#if defined(__x86_64__)
// Selectors of the per-core GDT (kernel code and data come first, see Interrupts.hpp).
constexpr uint16_t kSelectorUserData = 0x18 | 3, kSelectorUserCode = 0x20 | 3, kSelectorTss = 0x28;
constexpr uint8_t kVectorSyscall = 0x80;      // what the SYSCALL entry stub reports as its "interrupt"

// What the CPU and the entry stubs need while they are still on the user page tables, so it all sits in the core's own page (see Core in the thread
// scheduler): GS-relative offsets used by Kernel/UserEntry.S and Kernel/Interrupts.S - keep them in step (they are checked by static_asserts).
struct alignas(64) UserEntry {
  uint64_t kernel_cr3 = 0;        //   0: the kernel's page tables
  uint64_t user_cr3 = 0;          //   8: the running thread's page tables (0: it is not a user thread)
  uint64_t kernel_stack = 0;      //  16: top of the running thread's kernel stack
  uint64_t scratch = 0;           //  24: the SYSCALL stub parks the user stack pointer here
  uint64_t entry_stack_top = 0;   //  32: top of this core's entry stack (TSS.rsp0)
  uint64_t tramp_exit = 0;        //  40: address of the exit stub in the trampoline
  uint64_t reserved[2] = {};
  alignas(16) uint64_t gdt[8] = {};      //  64
  alignas(16) uint32_t tss[28] = {};     // 128: 104 bytes used
};
static_assert(sizeof(UserEntry) == 256);

// The core's record and entry area, for loading the descriptor tables on that core (UserMode.cpp) and for the scheduler's bookkeeping.
UserEntry* core_user_entry(uint32_t core);
uint64_t core_page(uint32_t core);
uint64_t core_entry_stack_top(uint32_t core);

// Sets the page tables and context of the calling thread, and of the core it is running on, with interrupts off. Used when a kernel thread turns into a
// user-mode thread, and when its tables are rebuilt.
void set_current_user_context(uint64_t cr3, void* context);

// Changes the FS and/or GS base the calling user thread will return to ring 3 with (and keeps them for later switches).
void set_current_user_bases(bool set_fs, uint64_t fs, bool set_gs, uint64_t gs);

// Called on every timer tick that interrupted user mode, with the interrupted frame, which it may change (the user-mode module uses it to stop a program that
// was asked to end while it was running).
using UserTickHook = void (*)(Frame* frame);
void set_user_tick_hook(UserTickHook hook);

// Called on exceptions taken in user mode (instead of the kernel panic).
using UserFaultHandler = Frame* (*)(Frame* frame);
void set_user_fault_handler(UserFaultHandler handler);

#endif

}  // namespace kernel
