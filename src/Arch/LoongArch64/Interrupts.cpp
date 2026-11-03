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

// Exception dispatch for LoongArch: the entry is in Trap.S; handlers are registered per vector (an ESTAT.IS interrupt
// bit number, or kVectorYield for the BREAK that gives up the core).
#include "Arch/LoongArch64/Csr.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Kernel/Interrupts.hpp"
#include "console.hpp"

extern "C" void tcos_trap_entry();

namespace kernel {

namespace {
using namespace arch_loongarch64;

struct HandlerSlot {
  InterruptHandler handler;
  void* context;
};
HandlerSlot g_handlers[kVectorMax];
uint64_t g_firmware_eentry = 0;
uint64_t g_firmware_ecfg = 0;

// The trap entry's address is code we hand to the hardware as a number; one cast, here.
uint64_t entry_address() { return reinterpret_cast<uint64_t>(&tcos_trap_entry); }

uint32_t ecode_of(uint64_t estat) { return static_cast<uint32_t>((estat >> 16) & 0x3F); }

const char* exception_name(uint32_t ecode) {
  switch (ecode) {
    case 1: return "page invalid exception for load";
    case 2: return "page invalid exception for store";
    case 3: return "page invalid exception for fetch";
    case 4: return "page modification exception";
    case 5: return "page not readable";
    case 6: return "page not executable";
    case 7: return "page privilege illegal";
    case 8: return "address error";
    case 9: return "address not aligned";
    case 10: return "bound check exception";
    case 11: return "system call";
    case 12: return "breakpoint";
    case 13: return "instruction not exist";
    case 14: return "instruction privilege error";
    case 15: return "floating-point disabled";
    case 18: return "floating-point error";
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
    g_firmware_eentry = csr_read<kCsrEentry>();
    g_firmware_ecfg = csr_read<kCsrEcfg>();
  }
  csr_write<kCsrEentry>(entry_address());   // vector spacing 0 (ECFG.VS): every exception enters at the same address
  enable_ipi();
  csr_exchange<kCsrEcfg>(uint64_t{1} << kIntIpi, (uint64_t{1} << kIntIpi) | (uint64_t{7} << 16));
}

void interrupts_restore_firmware() {
  csr_write<kCsrEcfg>(g_firmware_ecfg);
  csr_write<kCsrEentry>(g_firmware_eentry);
}

void panic(const char* what, const Frame* frame) {
  irq_disable();
  hardware::DebugConsole::set_enabled(true);   // the firmware's text console is not safe to call from here
  console::break_lock();
  console::puts("\n*** KERNEL PANIC: ");
  console::puts(what);
  console::puts("\n");
  if (frame) {
    console::puts("estat ");
    console::put_hex(frame->estat);
    console::puts(" (");
    console::puts(ecode_of(frame->estat) == kEcodeInterrupt ? "interrupt" : exception_name(ecode_of(frame->estat)));
    console::puts(") badv ");
    console::put_hex(frame->badv);
    console::puts("\nera ");
    console::put_hex(frame->era);
    console::puts(" sp ");
    console::put_hex(frame->x[2]);
    console::puts(" prmd ");
    console::put_hex(frame->prmd);
    for (uint32_t i = 0; i < 31; ++i) {
      console::puts(i % 4 == 0 ? "\n" : " ");
      console::puts("r");
      console::put_dec(i + 1);
      console::puts("=");
      console::put_hex(frame->x[i]);
    }
    console::puts("\n");
  }
  for (;;) __asm__ volatile("idle 0");
}

}  // namespace kernel

extern "C" kernel::Frame* tcos_trap_dispatch(kernel::Frame* frame) {
  using namespace kernel;
  using namespace arch_loongarch64;
  const uint32_t ecode = ecode_of(frame->estat);
  if (ecode == kEcodeInterrupt) {
    // The pending interrupts the core has enabled; the lowest numbered goes first (the rest take the core again on return).
    const uint64_t pending = frame->estat & csr_read<kCsrEcfg>() & kIntMask;
    if (!pending) return frame;
    const uint32_t code = static_cast<uint32_t>(__builtin_ctzll(pending));
    const HandlerSlot slot = g_handlers[code];
    if (!slot.handler) {
      // An interrupt we do not serve: silence the source so it cannot storm.
      csr_exchange<kCsrEcfg>(0, uint64_t{1} << code);
      return frame;
    }
    return slot.handler(frame, slot.context);
  }
  if (ecode == kEcodeBreak) {   // BREAK from kernel code: the yield (a 4-byte instruction, see yield())
    const HandlerSlot slot = g_handlers[kVectorYield];
    if (slot.handler) {
      frame->era += 4;
      return slot.handler(frame, slot.context);
    }
  }
  panic(exception_name(ecode), frame);
  return frame;
}
