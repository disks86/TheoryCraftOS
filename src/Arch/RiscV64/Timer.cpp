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

#include "Arch/RiscV64/Timer.hpp"
#include "Arch/RiscV64/Sbi.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Interrupts.hpp"

namespace kernel::stimer {

namespace {
uint64_t g_frequency = 0;
uint64_t g_period = 0;  // time ticks per tick
constexpr uint64_t kSieTimer = 1u << 5;   // sie.STIE

uint64_t now() {
  uint64_t t;
  __asm__ volatile("rdtime %0" : "=r"(t));
  return t;
}
}  // namespace

bool available() {
  return arch_riscv64::sbi::probe(arch_riscv64::sbi::kExtTime) || arch_riscv64::sbi::spec_version() != 0;
}

void set_frequency(uint64_t hz) { g_frequency = hz; }

uint64_t frequency() {
  // Without a table value, the boot-time calibration against the firmware's Stall() says how fast time runs.
  return g_frequency ? g_frequency : hardware::tsc_ticks_per_us() * 1000000;
}

void start(uint32_t hz) {
  g_period = frequency() / (hz ? hz : 1);
  arch_riscv64::sbi::set_timer(now() + g_period);
  __asm__ volatile("csrs sie, %0" ::"r"(kSieTimer));
}

void rearm() { arch_riscv64::sbi::set_timer(now() + g_period); }

void stop() {
  __asm__ volatile("csrc sie, %0" ::"r"(kSieTimer));
  arch_riscv64::sbi::set_timer(~uint64_t{0});
}

}  // namespace kernel::stimer
