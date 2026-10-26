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

#include "Kernel/Interrupts.hpp"
#include "console.hpp"
#include "Hardware/Guest/PvPanic.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Apic.hpp"
#include "Kernel/UserEntry.hpp"

extern "C" {
extern const uint64_t tcos_isr_table[256];
kernel::Frame* tcos_interrupt_dispatch(kernel::Frame* frame);
}

namespace kernel {

namespace {
UserFaultHandler g_user_fault_handler = nullptr;
struct alignas(8) Descriptor {  // 64-bit interrupt gate
  uint16_t offset_low;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attributes;
  uint16_t offset_middle;
  uint32_t offset_high;
  uint32_t reserved;
};
static_assert(sizeof(Descriptor) == 16);

struct __attribute__((packed)) TablePointer {
  uint16_t limit;
  uint64_t base;
};

struct HandlerSlot {
  InterruptHandler handler;
  void* context;
};

uint64_t g_gdt[3];  // null, code, data
Descriptor g_idt[256];
HandlerSlot g_handlers[256];
TablePointer g_firmware_gdt, g_firmware_idt;
uint8_t g_firmware_pic_mask[2];
uint16_t g_firmware_cs = 0, g_firmware_ds = 0;
bool g_initialized = false;

const char* exception_name(uint8_t v) {
  static const char* const names[] = {
      "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode",
      "device not available", "double fault", "coprocessor overrun", "invalid TSS", "segment not present",
      "stack fault", "general protection", "page fault", "reserved", "x87 error", "alignment check",
      "machine check", "SIMD error", "virtualization", "control protection"};
  return v < sizeof(names) / sizeof(names[0]) ? names[v] : "exception";
}

uint64_t read_cr2() {
  uint64_t v;
  __asm__ volatile("movq %%cr2, %0" : "=r"(v));
  return v;
}
}  // namespace

void set_user_fault_handler(UserFaultHandler handler) { g_user_fault_handler = handler; }

void set_interrupt_handler(uint8_t vector, InterruptHandler handler, void* context) {
  g_handlers[vector] = {handler, context};
}

void interrupts_init() {
  if (g_initialized) return;
  g_gdt[0] = 0;
  g_gdt[1] = 0x00AF9A000000FFFFull;  // 64-bit code, ring 0
  g_gdt[2] = 0x00CF92000000FFFFull;  // data, ring 0
  for (uint32_t v = 0; v < 256; ++v) {
    const uint64_t address = tcos_isr_table[v];
    g_idt[v].offset_low = static_cast<uint16_t>(address);
    g_idt[v].selector = kSelectorCode;
    g_idt[v].ist = 0;
    g_idt[v].type_attributes = 0x8E;  // present, ring 0, interrupt gate (interrupts off inside)
    g_idt[v].offset_middle = static_cast<uint16_t>(address >> 16);
    g_idt[v].offset_high = static_cast<uint32_t>(address >> 32);
    g_idt[v].reserved = 0;
  }
  g_initialized = true;
}

void interrupts_load(bool boot_core) {
  if (boot_core) {
    __asm__ volatile("sgdt %0" : "=m"(g_firmware_gdt));
    __asm__ volatile("sidt %0" : "=m"(g_firmware_idt));
    __asm__ volatile("movw %%cs, %0" : "=r"(g_firmware_cs));
    __asm__ volatile("movw %%ds, %0" : "=r"(g_firmware_ds));
    // Silence the legacy PIC: the firmware's timer must not interrupt us.
    hardware::IoPort master(0x21), slave(0xA1);
    g_firmware_pic_mask[0] = master.read8();
    g_firmware_pic_mask[1] = slave.read8();
    master.write8(0xFF);
    slave.write8(0xFF);
  }
  const TablePointer gdt = {sizeof(g_gdt) - 1, reinterpret_cast<uint64_t>(g_gdt)};
  const TablePointer idt = {sizeof(g_idt) - 1, reinterpret_cast<uint64_t>(g_idt)};
  __asm__ volatile(
      "lgdt %0\n\t"
      "lidt %1\n\t"
      "pushq %2\n\t"
      "leaq 1f(%%rip), %%rax\n\t"
      "pushq %%rax\n\t"
      "lretq\n"  // reload CS from the new table
      "1:\n\t"
      "movw %3, %%ax\n\t"
      "movw %%ax, %%ds\n\t"
      "movw %%ax, %%es\n\t"
      "movw %%ax, %%ss\n\t"
      :
      : "m"(gdt), "m"(idt), "i"(static_cast<uint64_t>(kSelectorCode)), "i"(static_cast<uint16_t>(kSelectorData))
      : "rax", "memory");
}

void interrupts_restore_firmware() {
  __asm__ volatile("cli");
  const uint64_t cs = g_firmware_cs;
  const uint16_t ds = g_firmware_ds;
  __asm__ volatile(
      "lgdt %0\n\t"
      "lidt %1\n\t"
      "pushq %2\n\t"
      "leaq 1f(%%rip), %%rax\n\t"
      "pushq %%rax\n\t"
      "lretq\n"
      "1:\n\t"
      "movw %3, %%ax\n\t"
      "movw %%ax, %%ds\n\t"
      "movw %%ax, %%es\n\t"
      "movw %%ax, %%ss\n\t"
      :
      : "m"(g_firmware_gdt), "m"(g_firmware_idt), "r"(cs), "r"(ds)
      : "rax", "memory");
  hardware::IoPort(0x21).write8(g_firmware_pic_mask[0]);
  hardware::IoPort(0xA1).write8(g_firmware_pic_mask[1]);
}

void panic(const char* what, const Frame* frame) {
  __asm__ volatile("cli");
  console::break_lock();
  console::puts("\n*** KERNEL PANIC: ");
  console::puts(what);
  console::puts("\n");
  if (frame) {
    console::puts("vector ");
    console::put_hex(frame->vector & 0xFF);
    console::puts(" error ");
    console::put_hex(frame->error);
    console::puts(" rip ");
    console::put_hex(frame->rip);
    console::puts(" rsp ");
    console::put_hex(frame->rsp);
    console::puts(" cr2 ");
    console::put_hex(read_cr2());
    console::puts("\nrax ");
    console::put_hex(frame->rax);
    console::puts(" rbx ");
    console::put_hex(frame->rbx);
    console::puts(" rcx ");
    console::put_hex(frame->rcx);
    console::puts(" rdx ");
    console::put_hex(frame->rdx);
    console::puts("\n");
  }
  hardware::PvPanic::notify_panic();  // tell the host before the machine goes quiet
  for (;;) __asm__ volatile("hlt");
}

}  // namespace kernel

extern "C" kernel::Frame* tcos_interrupt_dispatch(kernel::Frame* frame) {
  const uint8_t vector = static_cast<uint8_t>(frame->vector);
  const kernel::HandlerSlot slot = kernel::g_handlers[vector];
  if (slot.handler) return slot.handler(frame, slot.context);
  if (vector < 32 && (frame->cs & 3) == 3 && kernel::g_user_fault_handler) return kernel::g_user_fault_handler(frame);      // a user program's fault is its own, not the kernel's
  if (vector < 32) kernel::panic(kernel::exception_name(vector), frame);
  // An interrupt nobody asked for (a stray firmware timer, spurious): acknowledge and go on.
  kernel::apic::acknowledge_stray(vector);
  return frame;
}
