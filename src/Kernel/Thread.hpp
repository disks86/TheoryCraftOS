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
#include <type_traits>
#include "Allocators/IAllocator.hpp"
#include "CPUScheduler/IoAffinity.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Kernel/Interrupts.hpp"

namespace kernel {

enum class ThreadState : uint8_t { Ready, Running, Blocked, Zombie };

// Priority levels the ready queue keeps apart (patterned after Windows' 0..31 range, though without its priority boosting): 0 is never scheduled ahead of anything,
// 31 always runs first. Most threads stay at kDefaultPriority.
constexpr uint8_t kPriorityLevels = 32;
constexpr uint8_t kDefaultPriority = 8;

struct Thread;

// Threads waiting for something, oldest first, with the lock that guards the list and the state of what they wait for (a semaphore's count, an
// event's flag; see docs/SchedulerLocking.md). The run queues reuse the type (their lock word is unused: the core lock guards them).
struct WaitList {
  uint32_t lock = 0;
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
  uint64_t core_mask = ~uint64_t{0};      // the cores (bit n: core n, for the first 64) it may run on
  int8_t preferred_kind = -1;  // CoreKind it likes, or -1
  uint8_t priority = kDefaultPriority;    // 0 (least) .. 31 (most); the ready queue always runs the highest non-empty level first, round-robin within a level
  // Device-aware placement (CPUScheduler/IoAffinity.hpp): the core it ran on last, the core whose device interrupts it follows (learnt from the
  // interrupts that wake it, or the bound device's), the idle core it was sent to when it became ready, and when that was.
  int32_t last_core = -1;
  int32_t io_device = -1;                 // bound with bind_thread_to_io_device, or -1
  scheduling::LearnedIoCore learned_io;
  int32_t wake_core = -1;
  uint64_t ready_tsc = 0;
  int32_t queued_on = -1;                 // the core whose run queue it is in, or -1
  uint8_t queued_level = 0;               // the priority level it was queued at (its priority may change while it waits)
  uint64_t wake_tsc = 0;
  Thread* prev = nullptr;   // run queue links (and `next` for the zombie list)
  Thread* next = nullptr;
  Thread* wait_prev = nullptr;   // wait list links: a thread whose wait timed out stays on its wait list until it takes itself off (finish_wait),
  Thread* wait_next = nullptr;   // and may be in a run queue meanwhile
  // The wake-up protocol (docs/SchedulerLocking.md): the thread lock, which wait this is (a timeout armed for an earlier one must not end it), and
  // the core whose sleeper list holds its timeout.
  uint32_t wake_lock = 0;
  uint32_t wait_seq = 0;
  uint32_t sleep_seq = 0;
  int32_t sleep_core = -1;
  uint32_t timer_refs = 0;   // ticks that still point at it (it is not freed before 0)
  bool zombie_listed = false;
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

// Restricts the thread to one core (-1: any). Takes effect the next time it is scheduled.
void set_thread_affinity(Thread* thread, int32_t core);
// Changes which of the kPriorityLevels queues the thread runs from (clamped to 0..kPriorityLevels-1). Takes effect the next time it is scheduled: a thread already
// sitting in the ready queue runs once more at its old level, then moves.
void set_thread_priority(Thread* thread, uint8_t priority);
// Restricts the thread to the cores whose bit is set (cores past the 64th are always allowed). False for a mask that allows no online core. Takes effect when the thread is next
// scheduled (within a time slice if it runs on a core that is now excluded).
bool set_thread_core_mask(Thread* thread, uint64_t mask);
uint64_t online_core_mask();      // bit n: core n is online (the first 64)
// Runs `fn(argument)` once on every online core (the first 64), the calling core included, with interrupts masked on that core while it runs: for the per-core registers (MSRs)
// that a setting has to be written to on each core. The cores are interrupted, so `fn` must be short and must not block. Waits up to `timeout_us` for the others; false if some
// core did not run it in time. One call at a time.
bool run_on_every_core(void (*fn)(void*), void* argument, uint64_t timeout_us = 200000);
// x86: idle cores wait with MONITOR / MWAIT and this hint in EAX (the C-state to ask for: 0x00 C1, 0x01 C1E, 0x10 C3, 0x20 C6 ... on Intel parts) instead of HLT. 0xFFFFFFFF goes back
// to HLT. The caller checks that the processor has MWAIT with interrupts as break events (CPUID leaf 5) and, for a state where the local APIC timer stops, an always-running timer
// (ARAT). Ignored on other architectures.
void set_idle_mwait_hint(uint32_t hint);
// `io_device` (from register_io_device, or -1) binds the new thread to that device from its first placement on (see bind_thread_to_io_device).
Thread* spawn(ThreadFunction fn, void* argument, const char* name = "thread", int32_t core = -1,
              int8_t preferred_kind = -1, uint32_t stack_bytes = kDefaultStackBytes, int32_t io_device = -1);
// Gives up the core until the scheduler picks this thread again.
void yield();
void sleep_us(uint64_t us);
inline void sleep_ms(uint64_t ms) { sleep_us(ms * 1000); }
[[noreturn]] void exit_thread();
// Waits for a thread to finish (timeout_us 0 = forever) and frees it. False on timeout.
bool join(Thread* thread, uint64_t timeout_us = 0);
// The thread cleans itself up when it finishes.
void detach(Thread* thread);

// Runs `work` to its end on a new thread with a full-size stack, on the calling core (so it may still call firmware), and waits for it. The boot thread runs on the firmware's
// stack, which is small (128 KiB under EDK2) and has the firmware's own memory below it: a page layout or anything else deep in there writes past its end into the firmware's pool
// and breaks it (the next firmware call then faults, which was the hang at shutdown). Runs `work` right here when threads are not running or cannot be started.
uint32_t current_core();
template <typename Work>
void run_on_large_stack(Work&& work) {
  using Callable = std::remove_reference_t<Work>;
  Callable* callable = &work;
  Thread* thread = running() ? spawn([](void* context) { (*static_cast<Callable*>(context))(); }, callable, "main", static_cast<int32_t>(current_core())) : nullptr;
  if (!thread) {
    work();
    return;
  }
  join(thread);
}

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
  int32_t io_core = -1;    // the core whose device interrupts it follows, or -1
  int32_t io_device = -1;  // the device it is bound to, or -1
  uint64_t cpu_ticks = 0;
};
struct CoreInfo {
  bool online = false;
  hardware::CoreKind kind = hardware::CoreKind::Performance;
  uint64_t busy_ticks = 0, idle_ticks = 0;
  uint64_t lock_free_ticks = 0;   // ticks that found nothing to do and took no lock
};
// Copies up to `capacity` live threads (not finished ones); returns how many were written.
uint32_t thread_snapshot(ThreadInfo* out, uint32_t capacity);
bool core_info(uint32_t core, CoreInfo* out);
uint32_t tick_rate();

Thread* current_thread();
uint32_t current_core();
uint32_t core_count();

// --- Device-aware scheduling (I/O affinity). A thread runs best on the core that takes the interrupts of the device it waits for: the handler
// has just pulled the device's data (descriptors, completion entries, buffers) into that core's cache. The scheduler learns this by itself for
// every interrupt handler set with set_device_interrupt_handler: a thread that the handler wakes (Event::set, Semaphore::release, ...) is sent to
// the interrupting core, or an idle core sharing its last-level cache, if that is idle, and keeps preferring it while those interrupts keep coming.
// If this core was idle the woken thread starts as soon as the handler returns, not at the next tick. Placement is a preference: priorities
// stay strict, a thread is never kept off a core its affinity or core mask allows for long (kPlacementPatienceUs), and the core kind a thread
// asked for outweighs the device.

// Adds a device that raises interrupts on `interrupt_core` (-1: not routed yet). Returns its id, or -1 if the table is full. Safe before start().
int32_t register_io_device(const char* name, int32_t interrupt_core);
void unregister_io_device(int32_t device);
// The device's interrupt now arrives on `core` (after its MSI was reprogrammed); threads bound to it follow.
bool set_io_device_core(int32_t device, int32_t core);
// The core a new device's interrupt should be routed to: spreads devices over the online cores, preferring `kind` (-1: any) and leaving the boot
// core (which runs the desktop) alone while others are online. 0 before start().
// With `node` (the device's NUMA node, -1: none) the cores of that node come first.
uint32_t choose_io_interrupt_core(int8_t kind = -1, int64_t node = -1);
// The device's own NUMA node (from ACPI _PXM; -1: none), reported by io_device_info; pass it to choose_io_interrupt_core when routing the device.
bool set_io_device_node(int32_t device, int64_t node);
// The core whose interrupt-message id (core_hardware_id) is `hardware_id`, or -1.
int32_t core_from_hardware_id(uint64_t hardware_id);
// Puts an online core in a NUMA node (the ACPI SRAT proximity domain): placement then prefers, after a shared cache, a core on the node of the
// device's or the thread's last core. Every core is in node 0 until this is called. False for a core that is not online.
bool set_core_numa_node(uint32_t core, uint32_t node);
// The relative distance between two NUMA nodes (the ACPI SLIT: 10 local), for the first scheduling::NumaDistances::kMaxNodes nodes: among cores
// that suit a thread equally, the one on the nearest node wins. Unset distances count as 10 within a node and 20 between nodes. False out of range.
bool set_numa_distance(uint32_t from, uint32_t to, uint8_t distance);
// What addresses the core in an interrupt message: the local APIC id on x86 (MPIDR affinity on ARM, the hart id on RISC-V). ~0 if out of range.
uint64_t core_hardware_id(uint32_t core);
// set_interrupt_handler for a device's interrupt: the handler runs as before, and what it wakes is placed as above. A null handler removes it.
// False if every device vector slot is taken.
bool set_device_interrupt_handler(Vector vector, int32_t device, InterruptHandler handler, void* context);
// Makes the thread follow `device`'s interrupt core (whatever it learnt), -1 to go back to learning. Takes effect when it is next placed.
void bind_thread_to_io_device(Thread* thread, int32_t device);
bool io_device_info(int32_t device, scheduling::IoDeviceInfo* out);
// Moves the device's interrupt to `core` (whose interrupt-message id is `hardware_id`); true if it was moved. Called from a kernel thread, never
// from an interrupt or with a lock held.
using IoRetarget = bool (*)(void* context, uint32_t core, uint64_t hardware_id);
// Lets the scheduler move the device's interrupt to where its work runs: when most of the threads its interrupts wake cannot run on the
// interrupt's core (pinned to another core, or wanting another kind) and one core takes most of them (scheduling::InterruptSteering),
// `retarget` is called to send the interrupt there, and set_io_device_core follows if it succeeds. A null `retarget` stops this, after waiting for
// a call in progress. False for a device that is not registered.
bool set_io_device_retarget(int32_t device, IoRetarget retarget, void* context);
constexpr uint64_t kPlacementPatienceUs = 2000;

// For the synchronization primitives (Sync.hpp); the protocol is in docs/SchedulerLocking.md.
namespace detail {
// A wait list's lock: a spinlock that also masks interrupts on this core. Hold it only for a few instructions.
uint64_t lock_list(WaitList* list);
void unlock_list(WaitList* list, uint64_t flags);
// With `list`'s lock held (`list` may be null for a pure sleep, then with nothing held): the calling thread becomes Blocked, joins `list` and, with
// a timeout, its core's sleepers. The caller then unlocks, calls yield() and, when that returns, finish_wait().
void prepare_wait(WaitList* list, uint64_t timeout_us);
// Takes the calling thread off the sleepers and off `list` (whichever it is still on). True if the wait timed out.
bool finish_wait(WaitList* list);
// With `list`'s lock held: wakes the oldest waiter that is still waiting (one whose timeout already ended its wait is passed over). False if none.
bool wake_one(WaitList* list);
uint32_t wake_all(WaitList* list);
}  // namespace detail

}  // namespace kernel
