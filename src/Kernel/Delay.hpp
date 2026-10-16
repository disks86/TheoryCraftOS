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
#include "uefi.hpp"

namespace hardware {

// Short busy-wait delays and a microsecond timebase that work on any core, in any
// context (no firmware call once calibrated), so drivers can wait for hardware
// from any thread. Before delay_init() the firmware's Stall() is used.

// Measures the timestamp counter against the firmware's Stall() (about 30 ms,
// boot core, boot services still active). Call once, early.
void delay_init(efi::SystemTable* st);
// Waits at least `us` microseconds. Spins; for long waits prefer a thread sleep.
void delay_us(uint64_t us);
// Microseconds since delay_init(), from the timestamp counter.
uint64_t uptime_us();
// Like delay_us() but never sleeps.
void delay_spin_us(uint64_t us);
// Long delays give the core away once the scheduler registers a sleep function:
// delay_us(us >= 200) then sleeps instead of spinning, but only where sleeping is
// legal (interrupts enabled).
void set_sleep_function(void (*sleep_us)(uint64_t));
// Counter ticks per microsecond (0 before delay_init()).
uint64_t tsc_ticks_per_us();
uint64_t read_tsc();

}  // namespace hardware
