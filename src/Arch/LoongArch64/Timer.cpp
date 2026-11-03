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

#include "Arch/LoongArch64/Csr.hpp"
#include "Arch/LoongArch64/Timer.hpp"
#include "Kernel/Delay.hpp"

namespace kernel::stimer {

namespace {
using namespace arch_loongarch64;
uint64_t g_frequency = 0;

// CPUCFG 4 is the crystal frequency and CPUCFG 5 the multiplier (bits 15:0) and divider (31:16) of the constant frequency counter.
uint64_t cpucfg_frequency() {
  const uint64_t crystal = __builtin_loongarch_cpucfg(4);
  const uint32_t ratio = __builtin_loongarch_cpucfg(5);
  const uint64_t mul = ratio & 0xFFFF, div = ratio >> 16;
  return crystal && mul && div ? crystal * mul / div : 0;
}
}  // namespace

bool available() { return true; }

void set_frequency(uint64_t hz) { g_frequency = hz; }

uint64_t frequency() {
  if (g_frequency) return g_frequency;
  const uint64_t hz = cpucfg_frequency();
  // Without CPUCFG data the boot-time calibration against the firmware's Stall() says how fast time runs.
  return hz ? hz : hardware::tsc_ticks_per_us() * 1000000;
}

void start(uint32_t hz) {
  uint64_t period = frequency() / (hz ? hz : 1);
  if (period < 8) period = 8;
  csr_write<kCsrTcfg>((period & ~uint64_t{3}) | kTcfgEnable | kTcfgPeriodic);
  csr_write<kCsrTiclr>(1);
  csr_exchange<kCsrEcfg>(uint64_t{1} << kIntTimer, uint64_t{1} << kIntTimer);
}

void rearm() { csr_write<kCsrTiclr>(1); }

void stop() {
  csr_exchange<kCsrEcfg>(0, uint64_t{1} << kIntTimer);
  csr_write<kCsrTcfg>(0);
  csr_write<kCsrTiclr>(1);
}

}  // namespace kernel::stimer
