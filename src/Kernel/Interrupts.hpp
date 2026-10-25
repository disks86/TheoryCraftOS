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

#if defined(__x86_64__)
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

using Vector = uint8_t;
constexpr uint8_t kSelectorCode = 0x08, kSelectorData = 0x10;
constexpr uint8_t kVectorTimer = 0x40;     // local APIC timer: preemption tick
constexpr uint8_t kVectorYield = 0x41;     // software interrupt: give up the core
constexpr uint8_t kVectorWakeup = 0x42;    // inter-processor interrupt: look for work
constexpr uint8_t kVectorVirtioGpu = 0x50; // MSI-X: the virtio-gpu's command completions
constexpr uint8_t kVectorSpurious = 0xFF;
constexpr uint64_t kRflagsInterrupts = 1u << 9;
#elif defined(__aarch64__)
// The register state of an interrupted context, as Arch/AArch64/Vectors.S saves it on the
// interrupted stack (lowest address first). A handler returns the frame to resume: the same one,
// or another context's - which is how threads are switched. Only the general registers are
// saved: the image is built with -mgeneral-regs-only, so no kernel code touches the SIMD state.
// The assembly relies on these offsets (x at 0, sp at 248, elr at 256, spsr at 264, vector at 272).
struct alignas(16) Frame {
  uint64_t x[31];    // x0..x30
  uint64_t sp;       // the interrupted stack pointer (informational: resuming uses the frame's own address)
  uint64_t elr;      // where to resume
  uint64_t spsr;     // PSTATE to resume with (includes the interrupt mask bits)
  uint64_t vector;   // which of the 16 vector table entries took the exception
  uint64_t esr;      // exception syndrome
  uint64_t far;      // faulting address
  uint64_t reserved_;
};
static_assert(sizeof(Frame) == 304);

// Vectors are GIC interrupt ids (SGI 0-15, PPI 16-31, SPI 32-1019) plus pseudo vectors for
// exceptions that are not interrupts.
using Vector = uint16_t;
constexpr Vector kVectorWakeup = 1;      // SGI: look for work (inter-processor interrupt)
constexpr Vector kVectorYield = 1020;    // pseudo vector: the SVC that gives up the core
constexpr Vector kVectorMax = 1024;
constexpr uint64_t kSpsrInterruptsEnabled = 0;  // DAIF clear
#endif

using InterruptHandler = Frame* (*)(Frame* frame, void* context);

// Builds the descriptor tables (once, on the boot core, interrupts off).
void interrupts_init();
// Loads them on the calling core. Remembers what was loaded before (the
// firmware's tables) on the boot core so interrupts_restore_firmware() can undo it.
void interrupts_load(bool boot_core);
// Boot core only: puts the firmware's descriptor tables back.
void interrupts_restore_firmware();
void set_interrupt_handler(Vector vector, InterruptHandler handler, void* context);

// Interrupt flag helpers; irq_save() returns the previous state for irq_restore().
#if defined(__x86_64__)
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
#elif defined(__aarch64__)
// DAIF bit 7 is the IRQ mask (I): set = IRQs masked. The saved value is DAIF itself.
constexpr uint64_t kDaifIrqMasked = 1u << 7;
inline uint64_t irq_save() {
  uint64_t daif;
  __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(daif)::"memory");
  return daif;
}
inline void irq_restore(uint64_t daif) {
  if (!(daif & kDaifIrqMasked)) __asm__ volatile("msr daifclr, #2" ::: "memory");
}
inline void irq_enable() { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
inline void irq_disable() { __asm__ volatile("msr daifset, #2" ::: "memory"); }
inline bool irq_enabled() {
  uint64_t daif;
  __asm__ volatile("mrs %0, daif" : "=r"(daif));
  return !(daif & kDaifIrqMasked);
}
#else
#error "No interrupt flag helpers for this architecture"
#endif

// The hint a spin-wait loop gives the core (x86 PAUSE, ARM YIELD).
inline void cpu_relax() {
#if defined(__x86_64__)
  __asm__ volatile("pause");
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#endif
}

// Prints the state and stops the machine.
[[noreturn]] void panic(const char* what, const Frame* frame);

}  // namespace kernel
