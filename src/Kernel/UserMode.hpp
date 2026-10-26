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
#include "Allocators/IAllocator.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Kernel/UserEntry.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"
#include "Process/UserPageTables.hpp"
#include "Process/UserProgram.hpp"
#include "Utility/UniquePointer.hpp"
#include "Utility/ArrayView.hpp"

namespace kernel {

namespace usermode {

// Sets up everything a user-mode process needs, once, after kernel::start(): the trampoline (entry stubs, interrupt table), the SYSCALL registers and
// the per-core tables on every core. False if the machine or the memory does not allow it (nothing is changed then, except memory already allocated).
bool init(allocators::IAllocator& allocator);
bool ready();
// Addresses a user address space must stay away from (pages the CPU needs on the user page tables): hand them to AddressSpace::set_reserved().
ArrayView<const process::AddressRange> reserved_ranges();

struct Task;

// One thread of a program: a CPU thread that runs in ring 3 (and in the kernel for its system calls). All the threads of a program share the Task.
struct UserThread {
  Task* task = nullptr;
  Thread* thread = nullptr;
  uint64_t id = 0;
  process::ThreadStart start;
  bool spawned = false, joined = false;
  volatile bool finished = false;      // it has reached the end of its life (set just before the CPU thread ends; only then may the CPU thread be joined)
  UserThread* next = nullptr;
};

// One running program: its page tables, its threads and how it ended. It is also the process's runtime (see process::IProcessRuntime): the lock that system calls
// hold, the waiting between threads, and the starting of new threads. A program with more than one thread keeps all of them on one core (they are time-sliced, not run
// side by side), which is what lets them share page tables without cross-core invalidation.
struct Task final : process::IProcessRuntime {
  Task(allocators::IAllocator& allocator, process::UserProgram& program, bool nx) : program(program), tables(allocator, nx), allocator_(allocator) {}
  ~Task();
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  process::UserProgram& program;
  process::UserPageTables tables;
  bool faulted = false;        // the CPU stopped it (page fault, invalid instruction, ...)
  bool kill_requested = false; // set by request_kill(), read by the program's own threads
  bool killed = false;         // it ended because of request_kill()
  uint64_t fault_vector = 0, fault_address = 0, fault_rip = 0;
  UserThread* threads = nullptr;      // every thread the program has had, newest first
  Mutex mutex;
  ConditionVariable changed;
  int32_t home_core = -1;

  // process::IProcessRuntime
  void lock() override { mutex.lock(); }
  void unlock() override { mutex.unlock(); }
  bool wait(uint64_t timeout_ns) override;
  void notify() override { changed.notify_all(); }
  bool interrupted() override;
  bool start_thread(const process::ThreadStart& start, bool suspended) override;
  bool resume_thread(uint64_t id) override;
  uint64_t current_thread_id() override;

  UserThread* add_thread(const process::ThreadStart& start);
  bool launch(UserThread* thread, uint32_t stack_bytes);
  // Rebuilds the page tables if the address space changed and gives them to all the threads. False if memory ran out.
  bool sync_tables();

 private:
  allocators::IAllocator& allocator_;
};

// Starts the program's first thread in ring 3. `program` must be loaded (with the reserved ranges set on its address space before loading) and must stay
// alive until the task is gone. Null if there is no memory or user mode is not ready.
UniquePointer<Task> spawn(process::UserProgram& program, allocators::IAllocator& allocator, const char* name);
// Ends the program as soon as it can: at its next system call or timer tick, whichever comes first (a blocked wait must be woken by the caller, for
// instance by the host's interrupt()). The exit code is 130. Safe from any thread.
void request_kill(Task& task);
// Waits for the program to end (timeout_us 0 = forever); false on timeout. The exit code is the program's own, or 128 + the vector of the fault that
// stopped it. Call once; afterwards the task may be destroyed.
bool wait(Task& task, uint32_t* exit_code, uint64_t timeout_us = 0);

}  // namespace usermode

}  // namespace kernel
