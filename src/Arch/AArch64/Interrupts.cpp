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

// Exception and interrupt dispatch for AArch64: the vector tables are in Vectors.S, the handlers
// are registered per vector (a GIC interrupt id, or kVectorYield for the SVC that gives up the core).
#include "Arch/AArch64/Gic.hpp"
#include "Kernel/Interrupts.hpp"
#include "console.hpp"

extern "C" {
extern char tcos_vectors_el1[];
extern char tcos_vectors_el2[];
}

namespace kernel {

namespace {
struct HandlerSlot {
  InterruptHandler handler;
  void* context;
};
HandlerSlot g_handlers[kVectorMax];
uint64_t g_firmware_vbar = 0;

uint64_t current_el() {
  uint64_t v;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
  return v >> 2;
}

uint64_t read_vbar() {
  uint64_t v;
  if (current_el() == 2) __asm__ volatile("mrs %0, vbar_el2" : "=r"(v));
  else __asm__ volatile("mrs %0, vbar_el1" : "=r"(v));
  return v;
}

void write_vbar(uint64_t v) {
  if (current_el() == 2) __asm__ volatile("msr vbar_el2, %0; isb" ::"r"(v));
  else __asm__ volatile("msr vbar_el1, %0; isb" ::"r"(v));
}

// The vector table's address is an object address, not code we call; one cast, here.
uint64_t table_address(const char* table) { return reinterpret_cast<uint64_t>(table); }

const char* exception_class(uint64_t esr) {
  switch ((esr >> 26) & 0x3F) {
    case 0x00: return "unknown reason";
    case 0x07: return "FP/SIMD access";
    case 0x15: return "SVC";
    case 0x20: case 0x21: return "instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: case 0x25: return "data abort";
    case 0x26: return "SP alignment fault";
    case 0x2F: return "SError";
    case 0x30: case 0x31: return "breakpoint";
    case 0x3C: return "BRK instruction";
    default: return "exception";
  }
}
}  // namespace

void set_interrupt_handler(Vector vector, InterruptHandler handler, void* context) {
  if (vector < kVectorMax) g_handlers[vector] = {handler, context};
}

void interrupts_init() {}

void interrupts_load(bool boot_core) {
  if (boot_core) g_firmware_vbar = read_vbar();
  write_vbar(table_address(current_el() == 2 ? tcos_vectors_el2 : tcos_vectors_el1));
}

void interrupts_restore_firmware() {
  if (g_firmware_vbar) write_vbar(g_firmware_vbar);
}

void panic(const char* what, const Frame* frame) {
  irq_disable();
  console::break_lock();
  console::puts("\n*** KERNEL PANIC: ");
  console::puts(what);
  console::puts("\n");
  if (frame) {
    console::puts("vector ");
    console::put_dec(static_cast<int64_t>(frame->vector));
    console::puts(" esr ");
    console::put_hex(frame->esr);
    console::puts(" (");
    console::puts(exception_class(frame->esr));
    console::puts(") far ");
    console::put_hex(frame->far);
    console::puts("\nelr ");
    console::put_hex(frame->elr);
    console::puts(" sp ");
    console::put_hex(frame->sp);
    console::puts(" spsr ");
    console::put_hex(frame->spsr);
    for (uint32_t i = 0; i < 31; ++i) {
      console::puts(i % 4 == 0 ? "\n" : " ");
      console::puts("x");
      console::put_dec(i);
      console::puts("=");
      console::put_hex(frame->x[i]);
    }
    console::puts("\n");
  }
  for (;;) __asm__ volatile("wfi");
}

}  // namespace kernel

extern "C" kernel::Frame* tcos_exception_dispatch(kernel::Frame* frame) {
  using namespace kernel;
  constexpr uint64_t kSync = 0, kIrq = 1;
  const uint64_t kind = frame->vector & 3;
  if (kind == kIrq) {
    const uint32_t intid = gic::acknowledge();
    if (intid >= 1020) return frame;  // spurious: nothing to end
    const HandlerSlot slot = g_handlers[intid];
    if (!slot.handler) {
      gic::disable_interrupt(intid);  // a firmware interrupt we do not serve (its timer, say): silence it
      gic::end_of_interrupt(intid);
      return frame;
    }
    Frame* next = slot.handler(frame, slot.context);
    gic::end_of_interrupt(intid);
    return next;
  }
  if (kind == kSync && ((frame->esr >> 26) & 0x3F) == 0x15) {  // SVC from kernel code: the yield
    const HandlerSlot slot = g_handlers[kVectorYield];
    if (slot.handler) return slot.handler(frame, slot.context);
  }
  panic(kind == kSync ? exception_class(frame->esr) : "FIQ or SError", frame);
  return frame;
}
