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

// Stand-ins for the kernel pieces the portable code calls, so the network stack and
// friends can be compiled and tested on the development machine (single thread).
#include "Kernel/Sync.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Interrupts.hpp"
#include <stdio.h>
#include <stdlib.h>
namespace kernel {
bool running() { return false; }
Thread* current_thread() { return nullptr; }
uint32_t current_core() { return 0; }
uint32_t core_count() { return 1; }
void yield() {}
void sleep_us(uint64_t) {}
Thread* spawn(ThreadFunction, void*, const char*, int32_t, int8_t, uint32_t) { return nullptr; }
bool join(Thread*, uint64_t) { return true; }
void detach(Thread*) {}
namespace detail {
uint64_t lock() { return 0; }
void unlock(uint64_t) {}
void block_current_locked(WaitList*, uint64_t) {}
bool wake_one_locked(WaitList*) { return false; }
uint32_t wake_all_locked(WaitList*) { return 0; }
}
}
namespace kernel { [[noreturn]] void panic(const char* what, const Frame*) { fprintf(stderr, "PANIC: %s\n", what); abort(); } }
namespace hardware { uint64_t read_tsc() { return 0; } uint64_t tsc_ticks_per_us() { return 1; } }

#include <stdio.h>
#include <stdlib.h>
// In the host tests a bounds violation is a failed test.
void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}
