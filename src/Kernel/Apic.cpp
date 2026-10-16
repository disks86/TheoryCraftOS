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

#include "Kernel/Apic.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Interrupts.hpp"

namespace kernel::apic {

namespace {
constexpr uint32_t kMsrApicBase = 0x1B;
constexpr uint64_t kApicBaseEnable = 1u << 11, kApicBaseX2 = 1u << 10;
constexpr uint32_t kRegId = 0x20, kRegEoi = 0xB0, kRegSpurious = 0xF0, kRegIcrLow = 0x300, kRegIcrHigh = 0x310,
                   kRegLvtTimer = 0x320, kRegTimerInitial = 0x380, kRegTimerCurrent = 0x390, kRegTimerDivide = 0x3E0;
constexpr uint32_t kLvtMasked = 1u << 16, kLvtPeriodic = 1u << 17;
constexpr uint32_t kIcrDeliveryPending = 1u << 12;
constexpr uint32_t kDivideBy16 = 0x3;

uint64_t g_base = 0xFEE00000;
bool g_x2apic = false;
uint64_t g_timer_ticks_per_us_x16 = 0;  // APIC timer ticks per microsecond, scaled by 16 for precision

uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return (uint64_t{high} << 32) | low;
}
void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)));
}

uint32_t read(uint32_t reg) {
  if (g_x2apic) return static_cast<uint32_t>(rdmsr(0x800 + (reg >> 4)));
  return *reinterpret_cast<volatile uint32_t*>(g_base + reg);
}
void write(uint32_t reg, uint32_t value) {
  if (g_x2apic) wrmsr(0x800 + (reg >> 4), value);
  else *reinterpret_cast<volatile uint32_t*>(g_base + reg) = value;
}
}  // namespace

uint32_t this_apic_id() {
  const hardware::CpuidResult leaf = hardware::cpuid(1);
  uint32_t id = leaf.ebx >> 24;
  if (hardware::cpuid(0).eax >= 0xB) id = hardware::cpuid(0xB).edx;  // x2APIC id (full 32 bits)
  return id;
}

bool init_boot_core() {
  const uint64_t base = rdmsr(kMsrApicBase);
  g_x2apic = base & kApicBaseX2;
  if (!(base & kApicBaseEnable)) wrmsr(kMsrApicBase, base | kApicBaseEnable);
  g_base = base & 0xFFFFF000ull;
  enable_this_core();

  // Calibrate: count APIC timer ticks across a fixed stretch of TSC time.
  write(kRegTimerDivide, kDivideBy16);
  write(kRegLvtTimer, kLvtMasked | kVectorTimer);
  write(kRegTimerInitial, 0xFFFFFFFF);
  hardware::delay_us(10000);
  const uint32_t elapsed = 0xFFFFFFFFu - read(kRegTimerCurrent);
  write(kRegTimerInitial, 0);
  g_timer_ticks_per_us_x16 = uint64_t{elapsed} * 16 / 10000;
  return g_timer_ticks_per_us_x16 != 0;
}

void enable_this_core() {
  const uint64_t base = rdmsr(kMsrApicBase);
  if (!(base & kApicBaseEnable)) wrmsr(kMsrApicBase, base | kApicBaseEnable);
  write(kRegSpurious, 0x100 | kVectorSpurious);  // software enable + spurious vector
}

void start_timer(uint32_t hz) {
  const uint64_t ticks = g_timer_ticks_per_us_x16 * (1000000 / hz) / 16;
  write(kRegTimerDivide, kDivideBy16);
  write(kRegLvtTimer, kLvtPeriodic | kVectorTimer);
  write(kRegTimerInitial, static_cast<uint32_t>(ticks));
}

void stop_timer() {
  write(kRegLvtTimer, kLvtMasked | kVectorTimer);
  write(kRegTimerInitial, 0);
}

void eoi() { write(kRegEoi, 0); }

void send_ipi(uint32_t apic_id, uint8_t vector) {
  const uint64_t flags = irq_save();
  if (g_x2apic) {
    wrmsr(0x800 + (kRegIcrLow >> 4), (uint64_t{apic_id} << 32) | vector);
  } else {
    write(kRegIcrHigh, apic_id << 24);
    write(kRegIcrLow, vector);
    while (read(kRegIcrLow) & kIcrDeliveryPending) __asm__ volatile("pause");
  }
  irq_restore(flags);
}

void acknowledge_stray(uint8_t vector) {
  if (vector == kVectorSpurious) return;  // spurious interrupts must not be acknowledged
  eoi();
  if (vector >= 0x60 && vector < 0x80) {  // the firmware's remapped legacy PIC
    if (vector >= 0x70) hardware::IoPort(0xA0).write8(0x20);
    hardware::IoPort(0x20).write8(0x20);
  }
}

}  // namespace kernel::apic
