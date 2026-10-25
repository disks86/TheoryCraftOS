/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

// Trap dispatch for RISC-V: the entry is in Trap.S; handlers are registered per vector (a supervisor interrupt cause
// code, or kVectorYield for the EBREAK that gives up the core).
#include "Hardware/DebugConsole.hpp"
#include "Kernel/Interrupts.hpp"
#include "console.hpp"

extern "C" void tcos_trap_entry();

namespace kernel {

namespace {
struct HandlerSlot {
  InterruptHandler handler;
  void* context;
};
HandlerSlot g_handlers[kVectorMax];
uint64_t g_firmware_stvec = 0;
uint64_t g_firmware_sie = 0;

constexpr uint64_t kInterruptBit = 1ull << 63;
constexpr uint64_t kCauseBreakpoint = 3;
constexpr uint64_t kSieSoftware = 1u << 1;   // sie.SSIE

// The trap entry's address is code we hand to the hardware as a number; one cast, here.
uint64_t entry_address() { return reinterpret_cast<uint64_t>(&tcos_trap_entry); }

const char* exception_name(uint64_t code) {
  switch (code) {
    case 0: return "instruction address misaligned";
    case 1: return "instruction access fault";
    case 2: return "illegal instruction";
    case 3: return "breakpoint";
    case 4: return "load address misaligned";
    case 5: return "load access fault";
    case 6: return "store address misaligned";
    case 7: return "store access fault";
    case 8: return "environment call from U-mode";
    case 9: return "environment call from S-mode";
    case 12: return "instruction page fault";
    case 13: return "load page fault";
    case 15: return "store page fault";
    default: return "exception";
  }
}
}  // namespace

void set_interrupt_handler(Vector vector, InterruptHandler handler, void* context) {
  if (vector < kVectorMax) g_handlers[vector] = {handler, context};
}

void interrupts_init() {}

void interrupts_load(bool boot_core) {
  if (boot_core) {
    __asm__ volatile("csrr %0, stvec" : "=r"(g_firmware_stvec));
    __asm__ volatile("csrr %0, sie" : "=r"(g_firmware_sie));
  }
  __asm__ volatile("csrw stvec, %0" ::"r"(entry_address()));   // direct mode: the address is 4-byte aligned
  __asm__ volatile("csrs sie, %0" ::"r"(kSieSoftware));
}

void interrupts_restore_firmware() {
  __asm__ volatile("csrw sie, %0" ::"r"(g_firmware_sie));
  __asm__ volatile("csrw stvec, %0" ::"r"(g_firmware_stvec));
}

void panic(const char* what, const Frame* frame) {
  irq_disable();
  hardware::DebugConsole::set_enabled(true);   // the firmware's text console is not safe to call from here
  console::break_lock();
  console::puts("\n*** KERNEL PANIC: ");
  console::puts(what);
  console::puts("\n");
  if (frame) {
    console::puts("scause ");
    console::put_hex(frame->scause);
    console::puts(" (");
    console::puts(frame->scause & kInterruptBit ? "interrupt" : exception_name(frame->scause));
    console::puts(") stval ");
    console::put_hex(frame->stval);
    console::puts("\nsepc ");
    console::put_hex(frame->sepc);
    console::puts(" sp ");
    console::put_hex(frame->x[1]);
    console::puts(" sstatus ");
    console::put_hex(frame->sstatus);
    for (uint32_t i = 0; i < 31; ++i) {
      console::puts(i % 4 == 0 ? "\n" : " ");
      console::puts("x");
      console::put_dec(i + 1);
      console::puts("=");
      console::put_hex(frame->x[i]);
    }
    console::puts("\n");
  }
  for (;;) __asm__ volatile("wfi");
}

}  // namespace kernel

extern "C" kernel::Frame* tcos_trap_dispatch(kernel::Frame* frame) {
  using namespace kernel;
  const uint64_t cause = frame->scause;
  const uint64_t code = cause & ~kInterruptBit;
  if (cause & kInterruptBit) {
    const HandlerSlot slot = code < kVectorMax ? g_handlers[code] : HandlerSlot{};
    if (!slot.handler) {
      // An interrupt we do not serve: silence the source so it cannot storm.
      if (code < 64) __asm__ volatile("csrc sie, %0" ::"r"(uint64_t{1} << code));
      return frame;
    }
    return slot.handler(frame, slot.context);
  }
  if (code == kCauseBreakpoint) {   // EBREAK from kernel code: the yield (a 4-byte instruction, see yield())
    const HandlerSlot slot = g_handlers[kVectorYield];
    if (slot.handler) {
      frame->sepc += 4;
      return slot.handler(frame, slot.context);
    }
  }
  panic(exception_name(code), frame);
  return frame;
}
