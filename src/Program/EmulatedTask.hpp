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
#include "Emulation/GuestCpu.hpp"
#include "Kernel/Thread.hpp"
#include "Process/UserProgram.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

// An emulated program running on a kernel thread of its own: the counterpart of kernel::usermode::Task for a program whose CPU is not the machine's. The thread
// interprets the guest (emulation::run_emulated) and makes its system calls into the same process::Process a native program would use, so nothing about files,
// pipes or the console differs. Guest threads (clone, CreateThread) are kernel threads of their own that take turns on one lock; what is missing is everything
// that needs the hardware's own user mode (Windows exceptions, SuspendThread).
// The system-wide table of synchronization objects (events, mutexes, ... that Windows programs share) must exist before an emulated Windows program runs; made once, on
// the kernel's own primitives. False if memory ran out. A native x86-64 program's user-mode init shares the same table.
bool ensure_system_objects(allocators::IAllocator& allocator);

class EmulatedTask final : public emulation::IRunControl {
 public:
  EmulatedTask(process::UserProgram& program, UniquePointer<emulation::IGuestCpu> cpu, allocators::IAllocator& allocator)
      : program_(program), cpu_(static_cast<UniquePointer<emulation::IGuestCpu>&&>(cpu)), allocator_(allocator) {}
  EmulatedTask(const EmulatedTask&) = delete;
  EmulatedTask& operator=(const EmulatedTask&) = delete;

  // Starts the thread. False if there is no memory for it.
  bool start(const char* name);
  // Ends the program at its next slice boundary or system call (a blocked read must be woken by the host's interrupt()). Safe from any thread.
  void request_stop() { __atomic_store_n(&stop_, true, __ATOMIC_RELEASE); }
  // Waits for the program to end (timeout_us 0 = forever); false on timeout. After true, outcome() is valid.
  bool wait(uint64_t timeout_us);
  const emulation::EmulatedOutcome& outcome() const { return outcome_; }

  // emulation::IRunControl
  bool stop_requested() override { return __atomic_load_n(&stop_, __ATOMIC_ACQUIRE); }
  void yield() override { kernel::yield(); }

 private:
  static void thread_main(void* self);

  process::UserProgram& program_;
  UniquePointer<emulation::IGuestCpu> cpu_;
  allocators::IAllocator& allocator_;
  emulation::EmulatedOutcome outcome_;
  kernel::Thread* thread_ = nullptr;
  bool joined_ = false;
  bool stop_ = false;
};

}  // namespace desktop
