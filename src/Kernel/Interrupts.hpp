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

namespace kernel {

// The register state of an interrupted context, as the interrupt entry code saves
// it on the interrupted stack (lowest address first). A handler returns the frame
// to resume: the same one, or another context's - which is how threads are switched.
struct alignas(16) Frame {
  uint8_t fpu[512];  // fxsave area: x87/SSE state
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
  uint64_t vector;  // low 8 bits
  uint64_t error;   // CPU error code, or 0
  uint64_t rip, cs, rflags, rsp, ss;
};
static_assert(sizeof(Frame) == 512 + 15 * 8 + 16 + 40);

constexpr uint8_t kSelectorCode = 0x08, kSelectorData = 0x10;
constexpr uint8_t kVectorTimer = 0x40;     // local APIC timer: preemption tick
constexpr uint8_t kVectorYield = 0x41;     // software interrupt: give up the core
constexpr uint8_t kVectorWakeup = 0x42;    // inter-processor interrupt: look for work
constexpr uint8_t kVectorSpurious = 0xFF;
constexpr uint64_t kRflagsInterrupts = 1u << 9;

using InterruptHandler = Frame* (*)(Frame* frame, void* context);

// Builds the descriptor tables (once, on the boot core, interrupts off).
void interrupts_init();
// Loads them on the calling core. Remembers what was loaded before (the
// firmware's tables) on the boot core so interrupts_restore_firmware() can undo it.
void interrupts_load(bool boot_core);
// Boot core only: puts the firmware's descriptor tables back.
void interrupts_restore_firmware();
void set_interrupt_handler(uint8_t vector, InterruptHandler handler, void* context);

// Interrupt flag helpers; irq_save() returns the previous state for irq_restore().
inline uint64_t irq_save() {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
  return flags;
}
inline void irq_restore(uint64_t flags) {
  if (flags & kRflagsInterrupts) __asm__ volatile("sti" ::: "memory");
}
inline void irq_enable() { __asm__ volatile("sti" ::: "memory"); }
inline void irq_disable() { __asm__ volatile("cli" ::: "memory"); }
inline bool irq_enabled() {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  return flags & kRflagsInterrupts;
}

// Prints the state and stops the machine.
[[noreturn]] void panic(const char* what, const Frame* frame);

}  // namespace kernel
