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
#include "Process/WindowsException.hpp"
#include "Utility/UniquePointer.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace kernel {

namespace usermode {

// Sets up everything a user-mode process needs, once, after kernel::start(): the trampoline (entry stubs, interrupt table), the SYSCALL registers and
// the per-core tables on every core. False if the machine or the memory does not allow it (nothing is changed then, except memory already allocated).
bool init(allocators::IAllocator& allocator);
bool ready();
// Where the amount of memory the machine has and has left comes from (the allocator; set once by the code that owns it). Without it programs are told 0.
using MemorySource = void (*)(void* context, uint64_t* total_bytes, uint64_t* free_bytes);
void set_memory_source(MemorySource source, void* context);
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
  // Stopping and ending by another thread (SuspendThread, TerminateThread): asked for under the program's lock, acted on by the thread itself at its next system call or
  // timer tick (a timer tick sends it to address 0, where the page fault it causes is recognised: see UserMode.cpp).
  bool suspend_requested = false, terminate_requested = false;
  uint32_t terminate_code = 0;
  bool parked = false, redirected = false, context_dirty = false;
  uint64_t core_mask = ~uint64_t{0};      // the cores it may run on (SetThreadAffinityMask)
  uint64_t saved_rip = 0;
  process::X86Context saved;      // its registers while it is parked
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
  uint32_t kill_code = 130;    // the exit code of a program that request_kill() ended (TerminateProcess says its own)
  uint64_t fault_vector = 0, fault_address = 0, fault_rip = 0;
  UserThread* threads = nullptr;      // every thread the program has had, newest first
  Mutex mutex;
  ConditionVariable changed;
  char name[24] = {};             // what the process manager calls the program (see set_name())
  uint64_t retired_ticks = 0;     // CPU ticks of the threads that have ended (under `mutex`)
  Task* all_next = nullptr;       // every running program (see process_snapshot()); under the registry's lock
  bool registered = false;
  uint32_t running_threads = 0;      // threads that have been started and have not reached their end (atomic): when the last one does, the process lets go of its handles
  uint64_t parent_id = 0;      // the process that started it with CreateProcess (0: none)
  uint64_t process_mask = ~uint64_t{0};      // the cores new threads may run on (SetProcessAffinityMask)
  UserThread* first_thread = nullptr;      // the program's first thread, when it was created but not started (CreateProcess with CREATE_SUSPENDED)

  // The programs this one started with CreateProcess (they end with it: usermode::wait() stops them).
  struct Child;
  Child* children = nullptr;

  // process::IProcessRuntime
  void lock() override { mutex.lock(); }
  void unlock() override { mutex.unlock(); }
  bool wait(uint64_t timeout_ns) override;
  void notify() override { changed.notify_all(); }
  bool interrupted() override;
  bool start_thread(const process::ThreadStart& start, bool suspended) override;
  bool resume_thread(uint64_t id) override;
  uint64_t current_thread_id() override;
  void request_suspend(uint64_t id) override;
  void request_resume(uint64_t id) override;
  bool is_parked(uint64_t id) override;
  bool get_context(uint64_t id, process::X86Context* out) override;
  bool set_context(uint64_t id, const process::X86Context& context) override;
  void request_terminate(uint64_t id, uint32_t code) override;
  uint32_t processor_count() override;
  void memory_status(uint64_t* total_bytes, uint64_t* free_bytes) override;
  bool set_affinity(uint64_t thread_id, uint64_t mask) override;
  uint64_t affinity(uint64_t thread_id) override;
  bool cpu_time(uint64_t thread_id, uint64_t* hundred_ns) override;
  uint32_t current_processor() override;
  bool process_exists(uint64_t pid) override;
  uint32_t list_processes(ProcessEntry* out, uint32_t capacity) override;
  uint64_t allocate_process_id() override;
  uint32_t spawn_process(const process::ChildSpec& spec) override;
  bool terminate_process(uint64_t pid, uint32_t code) override;
  bool resume_process(uint64_t pid) override;
  // Frees the children that have ended; with `all`, stops the others first and waits for them.
  void reap_children(bool all);
  bool start_first_thread();
  UserThread* find_thread(uint64_t id);

  void set_name(StringView text);
  UserThread* add_thread(const process::ThreadStart& start);
  bool launch(UserThread* thread, uint32_t stack_bytes);
  // Rebuilds the page tables if the address space changed and gives them to all the threads. False if memory ran out.
  bool sync_tables();

 private:
  allocators::IAllocator& allocator_;
};

// Starts the program's first thread in ring 3. `program` must be loaded (with the reserved ranges set on its address space before loading) and must stay
// alive until the task is gone. Null if there is no memory or user mode is not ready.
UniquePointer<Task> spawn(process::UserProgram& program, allocators::IAllocator& allocator, const char* name, bool suspended = false);
// --- Monitoring (the computer manager). Cumulative CPU counters; a tick is 1 / tick_rate() seconds on one core.
struct ProcessInfo {
  uint64_t id = 0, parent_id = 0;
  char name[24] = {};
  bool windows = false;        // otherwise Linux
  uint32_t threads = 0;        // live threads
  ThreadState state = ThreadState::Blocked;   // Running if any thread runs, else Ready if any is ready, else Blocked
  uint64_t cpu_ticks = 0;      // all its threads together, including those that have ended
  uint64_t memory_bytes = 0;   // mapped in its address space
};
// Copies up to `capacity` running programs; returns how many were written. Safe from any thread.
uint32_t process_snapshot(ProcessInfo* out, uint32_t capacity);


// Ends the program as soon as it can: at its next system call or timer tick, whichever comes first (a blocked wait must be woken by the caller, for
// instance by the host's interrupt()). The exit code is 130. Safe from any thread.
void request_kill(Task& task, uint32_t exit_code = 130);
// Waits for the program to end (timeout_us 0 = forever); false on timeout. The exit code is the program's own, or 128 + the vector of the fault that
// stopped it. Call once; afterwards the task may be destroyed.
bool wait(Task& task, uint32_t* exit_code, uint64_t timeout_us = 0);

}  // namespace usermode

}  // namespace kernel
