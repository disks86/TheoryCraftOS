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
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Kernel/Interrupts.hpp"

namespace kernel {

enum class ThreadState : uint8_t { Ready, Running, Blocked, Zombie };

struct Thread;

// Threads waiting for something, oldest first. Only touched with the scheduler lock held.
struct WaitList {
  Thread* head = nullptr;
  Thread* tail = nullptr;
};

// A kernel thread. The fields are the scheduler's business; use the functions below.
struct Thread {
  Frame* saved = nullptr;  // register state while not running
  uint8_t* stack = nullptr;
  void (*entry)(void*) = nullptr;
  void* argument = nullptr;
  const char* name = "";
  uint64_t id = 0;
  ThreadState state = ThreadState::Ready;
  bool on_cpu = false;      // its registers live on a core, not in `saved`
  bool detached = false;
  bool is_idle = false;
  bool timed_out = false;   // the last wait ended by timeout
  bool in_sleepers = false;
  int32_t affinity = -1;    // the only core allowed to run it, or -1
  int8_t preferred_kind = -1;  // CoreKind it likes, or -1
  uint64_t wake_tsc = 0;
  Thread* prev = nullptr;   // run queue / wait list links
  Thread* next = nullptr;
  Thread* sleep_next = nullptr;
  Thread* all_next = nullptr;  // every live thread, newest first (see thread_snapshot)
  uint64_t cpu_ticks = 0;      // timer ticks that found it running
  WaitList* waiting_on = nullptr;
  WaitList joiners;
  // User mode (x86-64): a thread that runs a user-mode program has its page tables here (0 for kernel threads); the scheduler hands them to the core
  // it runs on. `user_gs` is the program's GS base while the thread is in the kernel. `stack_top` is where its kernel stack ends.
  uint8_t* stack_top = nullptr;
  uint64_t user_cr3 = 0;
  uint64_t user_gs = 0;
  uint64_t user_fs = 0;        // the program's FS base (saved when the thread is switched out)
  void* user_context = nullptr;
};

constexpr uint32_t kDefaultStackBytes = 8 * 1024 * 1024;   // (what Linux gives a thread; reading a big web page and running its scripts is deep)

using ThreadFunction = void (*)(void* argument);

// Starts preemptive multithreading on every core: loads the interrupt tables,
// starts the timer tick on each core, and turns the calling context into the "main"
// thread (pinned to this core, the boot core, so it may still call firmware).
// Needs hardware::delay_init() first; the allocator must be safe from all cores.
bool start(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator, uint32_t tick_hz = 1000);
// Ends multithreading: stops the other cores, the timer, and gives the firmware its
// interrupt tables back. Call from the main thread with no other thread needed.
void stop();
bool running();

Thread* spawn(ThreadFunction fn, void* argument, const char* name = "thread", int32_t core = -1,
              int8_t preferred_kind = -1, uint32_t stack_bytes = kDefaultStackBytes);
// Gives up the core until the scheduler picks this thread again.
void yield();
void sleep_us(uint64_t us);
inline void sleep_ms(uint64_t ms) { sleep_us(ms * 1000); }
[[noreturn]] void exit_thread();
// Waits for a thread to finish (timeout_us 0 = forever) and frees it. False on timeout.
bool join(Thread* thread, uint64_t timeout_us = 0);
// The thread cleans itself up when it finishes.
void detach(Thread* thread);

// Frees the stacks of detached threads that have finished. Done automatically by
// idle cores and by spawn(); call it to have the memory back at a known point.
// Returns how many finished detached threads are still waiting (a core may still be
// leaving one).
uint32_t reap_zombies();

// --- Monitoring (the computer manager). Cumulative counters; a tick is 1 / tick_rate() seconds on one core.
struct ThreadInfo {
  uint64_t id = 0;
  char name[24] = {};
  ThreadState state = ThreadState::Ready;
  bool idle = false;       // a core's idle thread
  int32_t core = -1;       // the core it is running on right now, or -1
  int32_t affinity = -1;   // the only core allowed to run it, or -1
  uint64_t cpu_ticks = 0;
};
struct CoreInfo {
  bool online = false;
  hardware::CoreKind kind = hardware::CoreKind::Performance;
  uint64_t busy_ticks = 0, idle_ticks = 0;
};
// Copies up to `capacity` live threads (not finished ones); returns how many were written.
uint32_t thread_snapshot(ThreadInfo* out, uint32_t capacity);
bool core_info(uint32_t core, CoreInfo* out);
uint32_t tick_rate();

Thread* current_thread();
uint32_t current_core();
uint32_t core_count();

// For the synchronization primitives (Sync.hpp). The scheduler lock is a spinlock
// that also masks interrupts; hold it only for a few instructions.
namespace detail {
uint64_t lock();
void unlock(uint64_t flags);
// With the lock held: the calling thread joins `list` (may be null for a pure sleep)
// and becomes Blocked, with an optional timeout. The caller then unlocks and calls
// yield(); when it returns the thread was woken (or timed out: current_thread()->timed_out).
void block_current_locked(WaitList* list, uint64_t timeout_us);
// With the lock held: makes the oldest waiter runnable. False if none.
bool wake_one_locked(WaitList* list);
uint32_t wake_all_locked(WaitList* list);
}  // namespace detail

}  // namespace kernel
