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
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process {
namespace linuxabi {

// The thread calls of the Linux interface. They need the process's runtime (the thing that starts CPU threads and does the waiting); without one only the single-thread
// forms work and clone() says -ENOSYS.
//   clone: only the flag set a threading library uses (VM, FS, FILES, SIGHAND, THREAD, plus SETTLS and the tid pointers). The caller's registers are copied for the
//   child by whoever runs the program (Process::take_clone).
uint64_t sys_clone(Process& p, UserArch arch, const uint64_t* args);
//   futex: WAIT, WAKE, REQUEUE, CMP_REQUEUE, WAIT_BITSET, WAKE_BITSET (private flag ignored); the rest is -ENOSYS.
uint64_t sys_futex(Process& p, uint64_t address, uint64_t op, uint64_t value, uint64_t timeout_or_count, uint64_t address2, uint64_t value3);
uint64_t sys_set_tid_address(Process& p, uint64_t address);
uint64_t sys_gettid(Process& p);
// exit(): ends the calling thread; the last thread takes the process with it.
uint64_t sys_thread_exit(Process& p, uint32_t code);
uint64_t sys_sched_getaffinity(Process& p, uint64_t size, uint64_t mask_address);
// Wakes up to `count` threads waiting on the futex at `address`; returns how many.
uint64_t futex_wake(Process& p, uint64_t address, uint64_t count, uint32_t bitset);

}  // namespace linuxabi
}  // namespace process
