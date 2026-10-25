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
#include "Kernel/Delay.hpp"
#include "Kernel/Interrupts.hpp"

namespace hardware {

namespace {
efi::SystemTable* g_st = nullptr;
uint64_t g_ticks_per_us = 0;
uint64_t g_origin = 0;
void (*g_sleep)(uint64_t) = nullptr;

#if defined(__x86_64__)
inline uint64_t rdtsc() {
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return (uint64_t{high} << 32) | low;
}
#elif defined(__aarch64__)
// The generic timer's virtual count: constant rate, readable at any exception level.
inline uint64_t rdtsc() {
  uint64_t count;
  __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(count)::"memory");
  return count;
}
#endif
inline void cpu_pause() { kernel::cpu_relax(); }
}  // namespace

uint64_t read_tsc() { return rdtsc(); }

void delay_init(efi::SystemTable* st) {
  g_st = st;
  constexpr uint64_t kCalibrationUs = 10000;
  // Stall() never returns early, only late: an interruption makes a run take
  // longer and so count MORE ticks. The run with the fewest ticks is the least disturbed.
  uint64_t fewest = ~0ull;
  for (int run = 0; run < 3; ++run) {
    const uint64_t start = rdtsc();
    st->boot_services->stall(kCalibrationUs);
    const uint64_t ticks = rdtsc() - start;
    if (ticks < fewest) fewest = ticks;
  }
  g_ticks_per_us = fewest / kCalibrationUs;
  g_origin = rdtsc();
}

uint64_t tsc_ticks_per_us() { return g_ticks_per_us; }

uint64_t uptime_us() { return g_ticks_per_us ? (rdtsc() - g_origin) / g_ticks_per_us : 0; }

void set_sleep_function(void (*sleep_us)(uint64_t)) { g_sleep = sleep_us; }

void delay_us(uint64_t us) {
  if (g_sleep && us >= 200 && kernel::irq_enabled()) {
    g_sleep(us);
    return;
  }
  delay_spin_us(us);
}

void delay_spin_us(uint64_t us) {
  if (!g_ticks_per_us) {
    if (g_st) g_st->boot_services->stall(us);
    return;
  }
  const uint64_t end = rdtsc() + us * g_ticks_per_us;
  while (static_cast<int64_t>(rdtsc() - end) < 0) cpu_pause();
}

}  // namespace hardware
