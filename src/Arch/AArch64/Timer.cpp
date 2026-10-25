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

#include "Arch/AArch64/Timer.hpp"
#include "Arch/AArch64/Gic.hpp"
#include "Kernel/Interrupts.hpp"

namespace kernel::gtimer {

namespace {
uint64_t g_period = 0;  // counter ticks per tick

uint64_t current_el() {
  uint64_t v;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
  return v >> 2;
}
// Running at EL2 the tick comes from the hypervisor physical timer (CNTHP, PPI 26); at EL1 from the
// virtual timer (CNTV, PPI 27), which the firmware leaves alone.
bool at_el2() { return current_el() == 2; }

void write_tval(uint64_t v) {
  if (at_el2()) __asm__ volatile("msr cnthp_tval_el2, %0; isb" ::"r"(v));
  else __asm__ volatile("msr cntv_tval_el0, %0; isb" ::"r"(v));
}
void write_ctl(uint64_t v) {
  if (at_el2()) __asm__ volatile("msr cnthp_ctl_el2, %0; isb" ::"r"(v));
  else __asm__ volatile("msr cntv_ctl_el0, %0; isb" ::"r"(v));
}
}  // namespace

uint64_t frequency() {
  uint64_t v;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}

uint32_t interrupt() { return at_el2() ? 26 : 27; }

void start(uint32_t hz) {
  g_period = frequency() / (hz ? hz : 1);
  write_ctl(0);
  write_tval(g_period);
  write_ctl(1);  // ENABLE, interrupt unmasked
  gic::enable_interrupt(interrupt());
}

void rearm() { write_tval(g_period); }

void stop() {
  write_ctl(0);
  gic::disable_interrupt(interrupt());
}

}  // namespace kernel::gtimer
