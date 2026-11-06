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

#include <new>
#include "Kernel/Delay.hpp"
#include "Kernel/Firmware.hpp"
#if defined(__x86_64__)
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"
#include "Kernel/Apic.hpp"
#include "Kernel/Paging.hpp"
#elif defined(__aarch64__)
#include "Arch/AArch64/Gic.hpp"
#include "Arch/AArch64/Timer.hpp"
#elif defined(__riscv) && __riscv_xlen == 64
#include "Arch/RiscV64/Sbi.hpp"
#include "Arch/RiscV64/Timer.hpp"
#elif defined(__loongarch64)
#include "Arch/LoongArch64/Csr.hpp"
#include "Arch/LoongArch64/Timer.hpp"
#endif
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/UserEntry.hpp"
#endif

namespace kernel {

namespace {

constexpr uint32_t kMaxCores = 128;
static_assert(kMaxCores <= scheduling::kMaxPlacementCores);
constexpr uint32_t kSliceTicks = 10;
// What a thread gets on top of its placement_score on the core it was sent to when it became ready (more than any score), so that the core
// that was woken for it takes it rather than something it suits slightly better.
constexpr uint32_t kScoreSentHere = 32;

struct alignas(4096) Core {
  Core* self = nullptr;  // first field: read through the GS base
#if defined(__x86_64__)
  UserEntry user;        // at offset 64: the entry stubs reach it through GS (and, on the user page tables, this page is mapped for them)
#endif
  uint32_t index = 0;
  uint64_t apic_id = 0;   // what addresses the core in an inter-processor interrupt (APIC id; MPIDR affinity on ARM)
  bool online = false;
  bool parked = false;
  hardware::CoreKind kind = hardware::CoreKind::Performance;
  uint32_t slice = kSliceTicks;
  Thread* current = nullptr;
  Thread* idle = nullptr;
  Thread* leaving = nullptr;      // the thread this core switched away from, still running its interrupt return on that thread's stack (see finish_switch)
  uint64_t busy_ticks = 0, idle_ticks = 0;
  uint64_t lock_free_ticks = 0;     // ticks that took no lock (on_timer's fast path)
  uint64_t kernel_entries = 0;      // interrupts of this core taken from ring 3 or the kernel that went through the scheduler (see wait_for_kernel_entry)
  int32_t io_device = -1;           // the device whose interrupt handler this core is running (on_device_interrupt), or -1
  bool resched = false;             // a thread was sent to this core while it was idle: switch to it on the way out of the interrupt
  bool exit_reschedules = false;    // the interrupt this core is in (the tick, a device's) looks at `resched` on its way out
#if defined(__x86_64__)
  // The stack an interrupt or SYSCALL from ring 3 lands on (TSS.rsp0); fills the rest of the page. Frames move to the thread's kernel stack at once.
  alignas(16) uint8_t entry_stack[3584];
#endif
};
#if defined(__x86_64__)
static_assert(__builtin_offsetof(Core, user) == 64 && sizeof(Core) == 4096, "the entry stubs and the TSS assume this layout of a core's page");
static_assert(__builtin_offsetof(UserEntry, user_cr3) == 8 && __builtin_offsetof(UserEntry, kernel_stack) == 16 && __builtin_offsetof(UserEntry, scratch) == 24 &&
              __builtin_offsetof(UserEntry, entry_stack_top) == 32 && __builtin_offsetof(UserEntry, tramp_exit) == 40);
#endif

struct ApStart {
  uint32_t core;
};

// What a core's lock guards (docs/SchedulerLocking.md): its run queue, kept apart by priority level (levels[0] the lowest), and its sleepers. A thread
// waits in the queue of the core it was sent to, or of the core that suits it best when no allowed core was idle (choose_queue_core); any core may
// take from any queue (find_eligible), through try_lock. Kept out of Core, whose page layout the entry stubs depend on.
struct PerCore {
  uint32_t lock = 0;
  WaitList levels[kPriorityLevels];
  Thread* sleepers = nullptr;    // by wake_tsc, through Thread::sleep_next
  uint64_t next_wake_tsc = 0;    // the first sleeper's wake_tsc, ~0 without one (atomic: on_timer looks without the lock)
  bool running_idle = true;      // the core runs its idle thread (atomic: other cores look, see sent_elsewhere)
};

struct Scheduler {
  Core cores[kMaxCores];
  PerCore per_core[kMaxCores];
  ApStart ap_start[kMaxCores];
  uint32_t core_count = 1;
  uint32_t boot_core = 0;
  // How many threads wait in each core's queue, and in each priority level over all queues (atomics: placement and the tick read them without a lock).
  uint32_t queue_lengths[kMaxCores];
  uint32_t level_counts[kPriorityLevels];
  // What placement knows of each core (CPUScheduler/IoAffinity.hpp). `idle` is cleared (compare-and-swap) as soon as a thread is sent to the core,
  // so that the next thread made ready goes to another idle core; it is set again whenever the core switches to its idle thread. Written with atomic
  // stores and read without a lock: a decision on a stale view only places a thread less well.
  scheduling::CorePlace places[kMaxCores];
  scheduling::NumaDistances numa;   // (all 0, "not known", until set_numa_distance)
  // Interrupt steering (set_io_device_retarget), by device id: the driver's callback, the tally of where woken threads run, and a move the
  // balancer thread is to make (-1: none). balancer_wait's lock guards all of it; the balancer sleeps on that list until a move is decided.
  struct Steering {
    IoRetarget retarget = nullptr;
    void* context = nullptr;
    scheduling::InterruptSteering tally;
    int32_t move_to = -1;
  } steering[scheduling::IoDeviceTable::kCapacity];
  WaitList balancer_wait;
  Thread* balancer = nullptr;
  bool balancer_started = false;
  int32_t balancer_busy = -1;   // the device whose callback the balancer is in (atomic), or -1
  uint32_t threads_lock = 0;   // guards `zombies` and `all`
  Thread* zombies = nullptr;
  Thread* all = nullptr;  // every thread that exists, through Thread::all_next
  allocators::IAllocator* allocator = nullptr;
  hardware::ICentralProcessingUnit* cpu = nullptr;
  uint64_t tsc_per_us = 0;
  uint32_t tick_hz = 1000;
  uint64_t next_id = 1;
  uint32_t aps_ready = 0;
  uint64_t firmware_gs_base = 0;
  volatile bool go = false;  // the boot core finished setting up; application processors may load the tables
  volatile bool running = false;
  volatile bool stopping = false;
};

Scheduler g;
scheduling::IoDeviceTable g_devices;   // (outside g: drivers register before start())

// ---- the few places where the two architectures differ ----
#if defined(__x86_64__)
alignas(16) uint8_t g_fpu_template[512];

constexpr uint32_t kMsrFsBase = 0xC0000100, kMsrGsBase = 0xC0000101, kMsrKernelGsBase = 0xC0000102;

uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return (uint64_t{high} << 32) | low;
}
void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)));
}

Core* this_core() {
  Core* c;
  __asm__ volatile("movq %%gs:0, %0" : "=r"(c));
  return c;
}

void send_wakeup(uint64_t hardware_id) { apic::send_ipi(static_cast<uint32_t>(hardware_id), kVectorWakeup); }
void stop_tick() { apic::stop_timer(); }
void ack_tick() { apic::eoi(); }
void ack_wakeup() { apic::eoi(); }
[[noreturn]] void halt_forever() { for (;;) __asm__ volatile("cli; hlt"); }
uint32_t g_idle_mwait_hint = 0xFFFFFFFFu;
void idle_wait() {
  const uint32_t hint = __atomic_load_n(&g_idle_mwait_hint, __ATOMIC_RELAXED);
  if (hint == 0xFFFFFFFFu) {
    __asm__ volatile("sti; hlt");
    return;
  }
  Core* c = this_core();
  __asm__ volatile("monitor" ::"a"(&c->parked), "c"(0), "d"(0));       // (armed before the interrupts are on, so a wake-up in between ends the wait at once)
  __asm__ volatile("sti; mwait" ::"a"(hint), "c"(1) : "memory");       // ECX 1: an interrupt ends the wait even with the flag masked
}
void start_tick(uint32_t hz) { apic::start_timer(hz); }
// Makes `c` this core's record and returns what the firmware had there.
uint64_t swap_core_pointer(Core* c) {
  const uint64_t old = rdmsr(kMsrGsBase);
  wrmsr(kMsrGsBase, reinterpret_cast<uint64_t>(c));
  return old;
}
void restore_core_pointer(uint64_t old) { wrmsr(kMsrGsBase, old); }
uint64_t this_hardware_id() { return apic::this_apic_id(); }
uint64_t with_interrupts_enabled(uint64_t flags) { return flags | kRflagsInterrupts; }
// The highest-level cache CPUID describes (leaf 4 on Intel, 0x8000001D on AMD and Hygon) and how many logical processors share it.
uint32_t this_cache_domain() {
  uint32_t level = 0, sharing = 0;
  auto scan = [&](uint32_t leaf) {
    for (uint32_t sub = 0; sub < 16; ++sub) {
      const hardware::CpuidResult r = hardware::cpuid(leaf, sub);
      if ((r.eax & 0x1F) == 0) break;   // no more caches
      const uint32_t cache_level = (r.eax >> 5) & 7;
      if (cache_level >= level) {
        level = cache_level;
        sharing = ((r.eax >> 14) & 0xFFF) + 1;
      }
    }
  };
  if (hardware::cpuid(0).eax >= 4) scan(4);
  if (!level && hardware::cpuid(0x80000000).eax >= 0x8000001D) scan(0x8000001D);
  return scheduling::cache_domain_from_apic_id(apic::this_apic_id(), sharing);
}
#elif defined(__aarch64__)
// The core's own record hangs off TPIDR_ELx (EL2 when the firmware runs us there), the way x86 uses GS.
uint64_t current_el() {
  uint64_t v;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
  return v >> 2;
}
uint64_t read_core_pointer() {
  uint64_t v;
  if (current_el() == 2) __asm__ volatile("mrs %0, tpidr_el2" : "=r"(v));
  else __asm__ volatile("mrs %0, tpidr_el1" : "=r"(v));
  return v;
}
void write_core_pointer(uint64_t v) {
  if (current_el() == 2) __asm__ volatile("msr tpidr_el2, %0" ::"r"(v));
  else __asm__ volatile("msr tpidr_el1, %0" ::"r"(v));
}
Core* this_core() { return reinterpret_cast<Core*>(read_core_pointer()); }

uint64_t this_mpidr() {
  uint64_t v;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
  return v & 0xFF00FFFFFFull;
}

void send_wakeup(uint64_t hardware_id) { gic::send_sgi(hardware_id, kVectorWakeup); }
void stop_tick() { gtimer::stop(); }
void ack_tick() { gtimer::rearm(); }   // the dispatcher ends the interrupt in the GIC once the handler returns
void ack_wakeup() {}
[[noreturn]] void halt_forever() { for (;;) { irq_disable(); __asm__ volatile("wfi"); } }
// WFI wakes for a pending interrupt even while it is masked, so unmasking first loses no wake-up for long.
void idle_wait() { irq_enable(); __asm__ volatile("wfi"); }
void start_tick(uint32_t hz) { gtimer::start(hz); }
uint64_t swap_core_pointer(Core* c) {
  const uint64_t old = read_core_pointer();
  write_core_pointer(reinterpret_cast<uint64_t>(c));
  return old;
}
void restore_core_pointer(uint64_t old) { write_core_pointer(old); }
uint64_t this_hardware_id() { return this_mpidr(); }
uint64_t with_interrupts_enabled(uint64_t daif) { return daif & ~kDaifIrqMasked; }
// Cores of one cluster (MPIDR Aff1, and Aff2 above it) share its cache.
uint32_t this_cache_domain() { return static_cast<uint32_t>((this_mpidr() >> 8) & 0xFFFF); }
#elif defined(__riscv) && __riscv_xlen == 64
// The core's own record hangs off sscratch, the way x86 uses GS (the trap entry does not use sscratch).
uint64_t read_core_pointer() {
  uint64_t v;
  __asm__ volatile("csrr %0, sscratch" : "=r"(v));
  return v;
}
void write_core_pointer(uint64_t v) { __asm__ volatile("csrw sscratch, %0" ::"r"(v)); }
Core* this_core() { return reinterpret_cast<Core*>(read_core_pointer()); }

void send_wakeup(uint64_t hart) { arch_riscv64::sbi::send_ipi(hart); }
void stop_tick() { stimer::stop(); }
void ack_tick() { stimer::rearm(); }
void ack_wakeup() { __asm__ volatile("csrci sip, 2" ::: "memory"); }   // clear the pending software interrupt
[[noreturn]] void halt_forever() { for (;;) { irq_disable(); __asm__ volatile("wfi"); } }
// WFI wakes for a pending interrupt even while it is masked, so unmasking first loses no wake-up for long.
void idle_wait() { irq_enable(); __asm__ volatile("wfi"); }
void start_tick(uint32_t hz) { stimer::start(hz); }
uint64_t swap_core_pointer(Core* c) {
  const uint64_t old = read_core_pointer();
  write_core_pointer(reinterpret_cast<uint64_t>(c));
  return old;
}
void restore_core_pointer(uint64_t old) { write_core_pointer(old); }
uint64_t this_hardware_id() { return arch_riscv64::boot_hart_id(); }   // the boot hart; the other harts read tp (see ap_main)
uint64_t with_interrupts_enabled(uint64_t sstatus) { return sstatus | kSstatusSie; }
uint32_t this_cache_domain() { return 0; }   // (nothing standard says which harts share a cache: treat them as one domain)
#elif defined(__loongarch64)
// The core's own record hangs off CSR.SAVE1, the way x86 uses GS (the trap entry saves registers on the stack and uses no SAVE register).
uint64_t read_core_pointer() { return arch_loongarch64::csr_read<arch_loongarch64::kCsrSave1>(); }
void write_core_pointer(uint64_t v) { arch_loongarch64::csr_write<arch_loongarch64::kCsrSave1>(v); }
Core* this_core() { return reinterpret_cast<Core*>(read_core_pointer()); }

void send_wakeup(uint64_t core) { arch_loongarch64::send_ipi(static_cast<uint32_t>(core), 1); }
void stop_tick() { stimer::stop(); }
void ack_tick() { stimer::rearm(); }
void ack_wakeup() { arch_loongarch64::ack_ipi(); }   // clearing the IPI status ends the pending interrupt
[[noreturn]] void halt_forever() { for (;;) { irq_disable(); __asm__ volatile("idle 0"); } }
// IDLE may not wake for an interrupt that is already pending while it is masked, so interrupts are unmasked first;
// an interrupt between the unmask and IDLE is taken at once, and at worst the next tick ends the wait.
void idle_wait() { irq_enable(); __asm__ volatile("idle 0"); }
void start_tick(uint32_t hz) { stimer::start(hz); }
uint64_t swap_core_pointer(Core* c) {
  const uint64_t old = read_core_pointer();
  write_core_pointer(reinterpret_cast<uint64_t>(c));
  return old;
}
void restore_core_pointer(uint64_t old) { write_core_pointer(old); }
uint64_t this_hardware_id() { return arch_loongarch64::this_core_id(); }   // the id the IPI unit and the ACPI MADT use
uint64_t with_interrupts_enabled(uint64_t crmd) { return crmd | kCrmdIe; }
uint32_t this_cache_domain() { return 0; }
#endif

inline void pause_hint() { cpu_relax(); }

// ---- locks (docs/SchedulerLocking.md): spinlocks; the _irq forms also mask interrupts on this core ----
// Order: wait-list lock, then thread lock (Thread::wake_lock), then core lock (PerCore::lock). A second core lock only through try_lock.

void spin_lock(uint32_t* word) {
  while (__atomic_exchange_n(word, 1u, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(word, __ATOMIC_RELAXED)) pause_hint();
  }
}
bool spin_try_lock(uint32_t* word) { return !__atomic_load_n(word, __ATOMIC_RELAXED) && !__atomic_exchange_n(word, 1u, __ATOMIC_ACQUIRE); }
void spin_unlock(uint32_t* word) { __atomic_store_n(word, 0u, __ATOMIC_RELEASE); }

uint64_t lock_irq(uint32_t* word) {
  const uint64_t flags = irq_save();
  spin_lock(word);
  return flags;
}
void unlock_irq(uint32_t* word, uint64_t flags) {
  spin_unlock(word);
  irq_restore(flags);
}

// (the generic forms: clang's _n forms take only integers and pointers, not an enum)
ThreadState state_of(const Thread* t) {
  ThreadState state;
  __atomic_load(&t->state, &state, __ATOMIC_ACQUIRE);
  return state;
}
void set_state(Thread* t, ThreadState state) { __atomic_store(&t->state, &state, __ATOMIC_RELEASE); }

// ---- the list of all threads (for monitoring); threads lock ----

void register_thread(Thread* t) {
  const uint64_t flags = lock_irq(&g.threads_lock);
  t->all_next = g.all;
  g.all = t;
  unlock_irq(&g.threads_lock, flags);
}

void unregister_locked(Thread* t) {
  for (Thread** link = &g.all; *link; link = &(*link)->all_next) {
    if (*link == t) {
      *link = t->all_next;
      return;
    }
  }
}

// ---- lists: run queues through prev/next (core lock), wait lists through wait_prev/wait_next (the list's lock) ----

void queue_append(WaitList* list, Thread* t) {
  t->next = nullptr;
  t->prev = list->tail;
  if (list->tail) list->tail->next = t;
  else list->head = t;
  list->tail = t;
}

void queue_remove(WaitList* list, Thread* t) {
  if (t->prev) t->prev->next = t->next;
  else list->head = t->next;
  if (t->next) t->next->prev = t->prev;
  else list->tail = t->prev;
  t->prev = t->next = nullptr;
}

void wait_append(WaitList* list, Thread* t) {
  t->wait_next = nullptr;
  t->wait_prev = list->tail;
  if (list->tail) list->tail->wait_next = t;
  else list->head = t;
  list->tail = t;
}

void wait_remove(WaitList* list, Thread* t) {
  if (t->wait_prev) t->wait_prev->wait_next = t->wait_next;
  else list->head = t->wait_next;
  if (t->wait_next) t->wait_next->wait_prev = t->wait_prev;
  else list->tail = t->wait_prev;
  t->wait_prev = t->wait_next = nullptr;
}

// With the core's lock held.
void queue_on_locked(Thread* t, uint32_t core) {
  const uint8_t level = t->priority;
  queue_append(&g.per_core[core].levels[level], t);
  __atomic_add_fetch(&g.queue_lengths[core], 1, __ATOMIC_RELEASE);
  __atomic_add_fetch(&g.level_counts[level], 1, __ATOMIC_RELEASE);
  t->queued_on = static_cast<int32_t>(core);
  t->queued_level = level;
}

// With the lock of the core whose queue holds `t` held.
void unqueue_locked(Thread* t) {
  if (t->queued_on < 0) return;
  const uint32_t core = static_cast<uint32_t>(t->queued_on);
  queue_remove(&g.per_core[core].levels[t->queued_level], t);
  __atomic_sub_fetch(&g.queue_lengths[core], 1, __ATOMIC_RELEASE);
  __atomic_sub_fetch(&g.level_counts[t->queued_level], 1, __ATOMIC_RELEASE);
  t->queued_on = -1;
}

// The highest priority level with a ready thread in any queue, or -1.
int32_t top_ready_level() {
  for (uint32_t level = kPriorityLevels; level-- > 0;)
    if (__atomic_load_n(&g.level_counts[level], __ATOMIC_ACQUIRE)) return static_cast<int32_t>(level);
  return -1;
}

// ---- sleepers: per core, by wake time; that core's lock ----

void refresh_next_wake_locked(PerCore& pc) { __atomic_store_n(&pc.next_wake_tsc, pc.sleepers ? pc.sleepers->wake_tsc : ~uint64_t{0}, __ATOMIC_RELEASE); }

void sleepers_insert_locked(PerCore& pc, Thread* t) {
  Thread** link = &pc.sleepers;
  while (*link && static_cast<int64_t>((*link)->wake_tsc - t->wake_tsc) <= 0) link = &(*link)->sleep_next;
  t->sleep_next = *link;
  *link = t;
  t->in_sleepers = true;
  refresh_next_wake_locked(pc);
}

void sleepers_remove_locked(PerCore& pc, Thread* t) {
  Thread** link = &pc.sleepers;
  while (*link && *link != t) link = &(*link)->sleep_next;
  if (*link) *link = t->sleep_next;
  t->sleep_next = nullptr;
  t->in_sleepers = false;
  refresh_next_wake_locked(pc);
}

// ---- device-aware placement (CPUScheduler/IoAffinity.hpp) ----

ArrayView<const scheduling::CorePlace> places() { return ArrayView<const scheduling::CorePlace>(g.places, g.core_count); }

void set_place_idle(uint32_t core, bool idle) { __atomic_store_n(&g.places[core].idle, idle, __ATOMIC_RELEASE); }

scheduling::CoreSet allowed_cores(const Thread* t) {
  scheduling::CoreSet set;
  const int32_t affinity = __atomic_load_n(&t->affinity, __ATOMIC_RELAXED);
  if (affinity >= 0) {
    set.add(static_cast<uint32_t>(affinity));
    return set;
  }
  set.words[0] = __atomic_load_n(&t->core_mask, __ATOMIC_RELAXED);
  set.words[1] = ~uint64_t{0};   // (cores past the 64th are always allowed)
  return set;
}

// The core whose device interrupts the thread follows: its bound device's, else the one it learnt.
int32_t io_core_of(const Thread* t) {
  if (t->io_device >= 0) {
    const int32_t core = g_devices.interrupt_core(t->io_device);
    if (core >= 0) return core;
  }
  return t->learned_io.hint();
}

scheduling::PlacementHints hints_of(const Thread* t) {
  scheduling::PlacementHints hints;
  hints.io_core = io_core_of(t);
  hints.last_core = t->last_core;
  hints.preferred_kind = t->preferred_kind;
  return hints;
}

bool eligible(const Thread* t, const Core* c) {
  const int32_t affinity = __atomic_load_n(&t->affinity, __ATOMIC_RELAXED);
  if (affinity >= 0 && static_cast<uint32_t>(affinity) != c->index) return false;
  if (c->index < 64 && !((__atomic_load_n(&t->core_mask, __ATOMIC_RELAXED) >> c->index) & 1)) return false;
  return true;
}

// With `t`'s thread lock held, `t` Ready and on no core: puts it in the run queue of the idle core that suits it best, which is woken for it (the
// idle flag is claimed with a compare-and-swap, so two threads made ready at once go to two cores). When that is this core (idle, so in an
// interrupt) and the interrupt reschedules on its way out (the tick, on_device_interrupt), it is only marked; otherwise it interrupts itself. No
// idle core: the queue of the busy core that suits it best (choose_queue_core), where it waits for a core to give up its current thread.
void enqueue_ready(Thread* t) {
  t->ready_tsc = hardware::read_tsc();
  t->wake_core = -1;
  const scheduling::PlacementHints hints = hints_of(t);
  const scheduling::CoreSet allowed = allowed_cores(t);
  int32_t target = -1;
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    const int32_t candidate = scheduling::choose_wake_core(hints, allowed, places(), &g.numa);
    if (candidate < 0) break;
    bool expected = true;
    if (__atomic_compare_exchange_n(&g.places[candidate].idle, &expected, false, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      target = candidate;
      break;
    }
  }
  int32_t core = target;
  if (core < 0) core = scheduling::choose_queue_core(hints, allowed, places(), ArrayView<const uint32_t>(g.queue_lengths, g.core_count), &g.numa);
  if (core < 0) core = static_cast<int32_t>(g.running ? this_core()->index : g.boot_core);   // (no allowed core online: any core may still take it)
  t->wake_core = target;
  PerCore& pc = g.per_core[core];
  spin_lock(&pc.lock);
  queue_on_locked(t, static_cast<uint32_t>(core));
  spin_unlock(&pc.lock);
  if (target < 0) return;
  Core& c = g.cores[target];
  if (g.running && &c == this_core() && c.exit_reschedules) {
    c.resched = true;
    return;
  }
  send_wakeup(c.apic_id);
}

// Where a thread woken by this core's device interrupt will run when it cannot run here (pinned elsewhere, or wanting another kind of core): the
// core it is pinned to, else the core it ran on last. -1 when it can run here (or has never run).
int32_t remote_consumer(const Thread* t, const Core* self) {
  const bool kind_ok = t->preferred_kind < 0 || t->preferred_kind == static_cast<int8_t>(self->kind);
  if (eligible(t, self) && kind_ok) return -1;
  const int32_t affinity = __atomic_load_n(&t->affinity, __ATOMIC_RELAXED);
  return affinity >= 0 ? affinity : t->last_core;
}

bool wake_one_impl(WaitList* list);

// A wake-up of `consumer` by `device`'s interrupt counts toward moving the interrupt to where its threads run (with no other lock held but,
// possibly, a wait list's: nothing takes another lock while holding balancer_wait's).
void note_steering(int32_t device, int32_t consumer, uint32_t self) {
  if (static_cast<uint32_t>(device) >= scheduling::IoDeviceTable::kCapacity) return;
  const uint64_t flags = lock_irq(&g.balancer_wait.lock);
  Scheduler::Steering& steering = g.steering[device];
  if (steering.retarget) {
    steering.tally.note(consumer);
    if (steering.tally.window_full()) {
      const int32_t to = steering.tally.decide();
      if (to >= 0 && static_cast<uint32_t>(to) != self && static_cast<uint32_t>(to) < g.core_count && g.cores[to].online) {
        steering.move_to = to;
        wake_one_impl(&g.balancer_wait);
      }
    }
  }
  unlock_irq(&g.balancer_wait.lock, flags);
}

constexpr uint32_t kAnyWait = ~uint32_t{0};

// Ends `t`'s wait if it is still the one meant: Blocked, and (for a timeout) in the wait the timeout was armed for. Exactly one of a wake-up, a
// timeout and the thread's own core (finish_switch) puts it in a run queue: the thread lock decides between them. False if `t` had already
// stopped waiting.
bool try_wake(Thread* t, uint32_t wait_seq, bool timed_out) {
  const uint64_t flags = lock_irq(&t->wake_lock);
  if (state_of(t) != ThreadState::Blocked || (wait_seq != kAnyWait && t->wait_seq != wait_seq)) {
    unlock_irq(&t->wake_lock, flags);
    return false;
  }
  t->timed_out = timed_out;
  // Inside a device's interrupt handler the thread learns that core (and the wake-up may count toward moving the interrupt, below).
  int32_t device = -1, consumer = -1;
  uint32_t self = 0;
  if (g.running && !timed_out) {
    const Core* c = this_core();
    device = c->io_device;
    self = c->index;
    if (device >= 0 && t != g.balancer) {
      t->learned_io.woken_by_interrupt_on(self);
      consumer = remote_consumer(t, c);
    } else {
      device = -1;
    }
  }
  set_state(t, ThreadState::Ready);
  if (!__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) enqueue_ready(t);   // (else its core queues it in finish_switch)
  unlock_irq(&t->wake_lock, flags);
  if (device >= 0) {
    g_devices.note_wake(device);
    note_steering(device, consumer, self);
  }
  return true;
}

// With `list`'s lock held: the oldest waiter that is still waiting is woken; one whose timeout already ended its wait is taken off and passed over,
// so the wake-up goes to somebody who can use it.
bool wake_one_impl(WaitList* list) {
  while (Thread* t = list->head) {
    wait_remove(list, t);
    t->waiting_on = nullptr;
    if (try_wake(t, kAnyWait, false)) return true;
  }
  return false;
}

// The thread was sent to another core that is still idle (that core has been woken and is about to take it).
bool sent_elsewhere(const Thread* t, const Core* c) {
  const int32_t target = t->wake_core;
  if (target < 0 || static_cast<uint32_t>(target) == c->index) return false;
  return g.cores[target].online && __atomic_load_n(&g.per_core[target].running_idle, __ATOMIC_ACQUIRE);
}

// The best thread of one list for core `c` so far: one that has waited kPlacementPatienceUs or longer beats any other (the oldest of those);
// otherwise the best placement_score, plus kScoreSentHere for the thread this core was woken for, the earlier one among equals. A thread sent to
// another core that is still idle is left for that core.
struct Pick {
  Thread* thread = nullptr;
  uint32_t score = 0;
  bool aged = false;
};

void consider(Pick& pick, Thread* t, const Core* c, uint64_t now, int64_t patience) {
  if (!eligible(t, c)) return;
  if (static_cast<int64_t>(now - t->ready_tsc) >= patience) {
    if (!pick.aged || static_cast<int64_t>(t->ready_tsc - pick.thread->ready_tsc) < 0) pick = Pick{t, 0, true};
    return;
  }
  if (pick.aged || sent_elsewhere(t, c)) return;
  uint32_t score = scheduling::placement_score(hints_of(t), c->index, places());
  if (t->wake_core == static_cast<int32_t>(c->index)) score += kScoreSentHere;
  if (!pick.thread || score > pick.score) pick = Pick{t, score, false};
}

// What this core runs next, with its own core lock held. Priorities stay strict: the highest level with a thread this core may run, in any queue,
// wins. At that level it takes from its own queue first (Pick), and from another core's queue (work stealing) when its own has nothing it may run -
// or when a thread there has waited past its patience, so that a core busy with higher-priority work cannot strand the threads queued on it. A
// list is in the order its threads became ready, so the first eligible thread of another queue is the only one that can be the oldest. Another
// queue is looked at only if its lock is free (try_lock): a busy one is looked at again at the next tick.
// `take` removes the thread found from its queue (and returns it); otherwise only its level is returned (-1: nothing).
struct Next {
  Thread* thread = nullptr;
  int32_t level = -1;
};

Next pick_next(const Core* c, bool take) {
  const uint64_t now = hardware::read_tsc();
  const int64_t patience = static_cast<int64_t>(kPlacementPatienceUs * g.tsc_per_us);
  PerCore& own_queue = g.per_core[c->index];
  for (int32_t level = top_ready_level(); level >= 0; --level) {
    if (!__atomic_load_n(&g.level_counts[level], __ATOMIC_ACQUIRE)) continue;
    Pick own;
    for (Thread* t = own_queue.levels[level].head; t; t = t->next) consider(own, t, c, now, patience);
    if (!own.aged) {
      for (uint32_t step = 1; step < g.core_count; ++step) {
        const uint32_t i = (c->index + step) % g.core_count;   // (the next cores first, so that idle cores do not all raid the same queue)
        if (!__atomic_load_n(&g.queue_lengths[i], __ATOMIC_ACQUIRE) || !spin_try_lock(&g.per_core[i].lock)) continue;
        Pick stolen;
        for (Thread* t = g.per_core[i].levels[level].head; t; t = t->next) {
          if (own.thread) {   // (only an aged thread could beat it)
            if (!eligible(t, c)) continue;
            if (static_cast<int64_t>(now - t->ready_tsc) >= patience) consider(stolen, t, c, now, patience);
            break;
          }
          consider(stolen, t, c, now, patience);
        }
        if (stolen.thread && (stolen.aged || !own.thread)) {
          if (take) unqueue_locked(stolen.thread);
          spin_unlock(&g.per_core[i].lock);
          return Next{take ? stolen.thread : nullptr, level};
        }
        spin_unlock(&g.per_core[i].lock);
      }
    }
    if (own.thread) {
      if (take) unqueue_locked(own.thread);
      return Next{take ? own.thread : nullptr, level};
    }
  }
  return Next{};
}

// Decides what runs on this core after an interrupt or a yield; `frame` is the state of the interrupted context, interrupts are masked. Takes the
// outgoing thread's lock, then this core's lock. Returns the frame to resume.
Frame* reschedule(Frame* frame, Core* c, bool voluntary) {
  Thread* prev = c->current;
  c->resched = false;
  c->exit_reschedules = false;
  PerCore& pc = g.per_core[c->index];
  spin_lock(&prev->wake_lock);
  spin_lock(&pc.lock);
  set_place_idle(c->index, prev->is_idle);   // (undoes enqueue_ready's claim if the thread it was woken for went elsewhere)
  const ThreadState state = state_of(prev);
  if (!voluntary && state == ThreadState::Running) {
    const Next wanted = pick_next(c, false);
    // Keeps running when nobody else wants the core, or until its slice ends unless its core was taken away or something of a strictly higher
    // priority is now ready (which does not wait out the slice).
    const bool stay = eligible(prev, c) && (wanted.level < 0 || (!prev->is_idle && c->slice > 0 && wanted.level <= prev->priority));
    if (stay) {
      spin_unlock(&pc.lock);
      spin_unlock(&prev->wake_lock);
      return frame;
    }
  }
  prev->saved = frame;
  // This core is still on prev's stack (below the saved frame: the return addresses of the handlers that called this) until the trap stub has loaded the next
  // frame. If prev were visible to another core before that, that core could resume it and its calls would overwrite those return addresses. So prev only becomes
  // runnable (and reapable: on_cpu) in finish_switch(), which the stub calls on the new thread's stack.
  if (state == ThreadState::Running) set_state(prev, ThreadState::Ready);
  const bool requeued = state_of(prev) == ThreadState::Ready && !prev->is_idle;
  if (requeued) {      // (so that it competes for this core as before; taken out again below)
    prev->ready_tsc = hardware::read_tsc();
    prev->wake_core = -1;
    queue_on_locked(prev, c->index);
  }
  Thread* next = pick_next(c, true).thread;
  if (!next) next = c->idle;
  if (next != prev) {
    if (requeued) unqueue_locked(prev);      // (finish_switch queues it)
    c->leaving = prev;
  }
  set_state(next, ThreadState::Running);
  __atomic_store_n(&next->on_cpu, true, __ATOMIC_RELEASE);
  next->last_core = static_cast<int32_t>(c->index);
  next->wake_core = -1;
  c->current = next;
  __atomic_store_n(&pc.running_idle, next->is_idle, __ATOMIC_RELEASE);
  set_place_idle(c->index, next->is_idle);
#if defined(__x86_64__)
  // A user-mode thread brings its page tables and its GS base along; the entry stubs find them in this core's page.
  if (prev->user_cr3) {
    prev->user_gs = rdmsr(kMsrKernelGsBase);
    prev->user_fs = rdmsr(kMsrFsBase);
  }
  c->user.user_cr3 = next->user_cr3;
  c->user.kernel_stack = reinterpret_cast<uint64_t>(next->stack_top);
  if (next->user_cr3) {
    wrmsr(kMsrKernelGsBase, next->user_gs);
    wrmsr(kMsrFsBase, next->user_fs);
  }
#endif
  c->slice = kSliceTicks;
  spin_unlock(&pc.lock);
  spin_unlock(&prev->wake_lock);
  return next->saved;
}

// Called by the interrupt stub (Interrupts.S, and the trap stubs of the other architectures) on the stack of the thread that now runs, after the old
// thread's stack is no longer in use by this core: the old thread leaves the core (on_cpu false, so it can be reaped or joined) and, when it is
// ready (preempted, or woken while it was on its way out: try_wake then left it to this), joins a run queue where another core can take it. Under
// the old thread's lock, the same lock try_wake reads on_cpu under, so exactly one of the two queues it.
void finish_switch() {
  Core* c = this_core();
  Thread* prev = c->leaving;
  if (!prev) return;
  c->leaving = nullptr;
  const uint64_t flags = lock_irq(&prev->wake_lock);
  __atomic_store_n(&prev->on_cpu, false, __ATOMIC_RELEASE);
  if (state_of(prev) == ThreadState::Ready && !prev->is_idle) enqueue_ready(prev);   // (an idle core elsewhere may take it at once)
  unlock_irq(&prev->wake_lock, flags);
}

[[noreturn]] void park_core(Core* c) {
  c->parked = true;
  stop_tick();
  halt_forever();
}

#if defined(__x86_64__)
UserTickHook g_user_tick = nullptr;
#endif

// Whether this tick has anything to do under a lock: a sleeper of this core is due, a thread was sent here, the running thread may no longer run
// here, or a ready thread outranks it (or matches its level once its slice is over; anything at all for an idle core). Read without locks: a
// change that races with it is seen by the next tick, and whoever makes a thread ready wakes an idle core for it anyway.
bool tick_needs_lock(const Core* c, uint64_t now) {
  if (c->resched) return true;
  if (now >= __atomic_load_n(&g.per_core[c->index].next_wake_tsc, __ATOMIC_ACQUIRE)) return true;   // (unsigned: ~0, no sleeper, is never due)
  const Thread* current = c->current;
  if (!current->is_idle && !eligible(current, c)) return true;
  const int32_t top = top_ready_level();
  if (top < 0) return false;
  if (current->is_idle) return true;
  return top > current->priority || (c->slice == 0 && top == current->priority);
}

// The timeouts that are due on this core: taken off its sleepers under its lock and pinned (timer_refs, so that join cannot free them meanwhile),
// then ended with the core lock let go (try_wake takes the thread lock, which comes before it). At most kBatch a tick; the rest wait for the next.
void expire_sleepers(Core* c, uint64_t now) {
  constexpr uint32_t kBatch = 32;
  struct Due {
    Thread* thread;
    uint32_t seq;
  } due[kBatch];
  uint32_t count = 0;
  PerCore& pc = g.per_core[c->index];
  spin_lock(&pc.lock);
  while (count < kBatch && pc.sleepers && static_cast<int64_t>(pc.sleepers->wake_tsc - now) <= 0) {
    Thread* t = pc.sleepers;
    pc.sleepers = t->sleep_next;
    t->sleep_next = nullptr;
    t->in_sleepers = false;
    __atomic_add_fetch(&t->timer_refs, 1, __ATOMIC_ACQ_REL);
    due[count++] = Due{t, t->sleep_seq};
  }
  refresh_next_wake_locked(pc);
  spin_unlock(&pc.lock);
  for (uint32_t i = 0; i < count; ++i) {
    try_wake(due[i].thread, due[i].seq, true);
    __atomic_sub_fetch(&due[i].thread->timer_refs, 1, __ATOMIC_ACQ_REL);
  }
}

Frame* on_timer(Frame* frame, void*) {
  ack_tick();
  __atomic_fetch_add(&this_core()->kernel_entries, 1, __ATOMIC_SEQ_CST);
#if defined(__x86_64__)
  if ((frame->cs & 3) == 3 && g_user_tick) g_user_tick(frame);
#endif
  Core* c = this_core();
  if (g.stopping && c->index != g.boot_core) park_core(c);
  // The accounting is this core's own (interrupts are masked here); monitoring reads it with relaxed loads.
  if (c->current->is_idle) {
    __atomic_store_n(&c->idle_ticks, c->idle_ticks + 1, __ATOMIC_RELAXED);
  } else {
    __atomic_store_n(&c->busy_ticks, c->busy_ticks + 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c->current->cpu_ticks, 1, __ATOMIC_RELAXED);
  }
  if (c->slice > 0) --c->slice;
  const uint64_t now = hardware::read_tsc();
  if (!tick_needs_lock(c, now)) {   // most ticks: nothing changes, so no lock is taken on this one's account
    __atomic_store_n(&c->lock_free_ticks, c->lock_free_ticks + 1, __ATOMIC_RELAXED);
    return frame;
  }
  c->exit_reschedules = true;   // (a timeout that sends its thread here is switched to below)
  expire_sleepers(c, now);
  return reschedule(frame, c, false);
}

Frame* on_yield(Frame* frame, void*) { return reschedule(frame, this_core(), true); }

// Work for every core (run_on_every_core): the function, and the bit of each core that still has to run it.
struct CoreWork {
  void (*fn)(void*) = nullptr;
  void* argument = nullptr;
  uint64_t pending = 0;
  uint32_t busy = 0;
};
CoreWork g_core_work;

void run_core_work(Core* c) {
  if (c->index >= 64) return;
  const uint64_t bit = uint64_t{1} << c->index;
  if (!(__atomic_load_n(&g_core_work.pending, __ATOMIC_ACQUIRE) & bit)) return;
  g_core_work.fn(g_core_work.argument);
  __atomic_fetch_and(&g_core_work.pending, ~bit, __ATOMIC_ACQ_REL);
}

Frame* on_wakeup(Frame* frame, void*) {
  ack_wakeup();
  Core* c = this_core();
  run_core_work(c);
  __atomic_fetch_add(&c->kernel_entries, 1, __ATOMIC_SEQ_CST);
  if (g.stopping && c->index != g.boot_core) park_core(c);
  return reschedule(frame, c, false);
}

// ---- device interrupts (set_device_interrupt_handler) ----

struct DeviceVector {
  Vector vector = 0;
  bool used = false;
  int32_t device = -1;
  InterruptHandler handler = nullptr;   // read by the interrupt with an atomic load: cleared first when the slot is given up
  void* context = nullptr;
};
constexpr uint32_t kDeviceVectors = 32;
DeviceVector g_device_vectors[kDeviceVectors];
uint32_t g_device_vectors_lock = 0;

// Runs the driver's handler with this core marked as handling `device`, so that the threads it wakes learn the core (try_wake). If one of them
// was sent to this core, which was idle, it starts as the interrupt returns instead of at the next tick.
Frame* on_device_interrupt(Frame* frame, void* argument) {
  DeviceVector* slot = static_cast<DeviceVector*>(argument);
  const InterruptHandler handler = __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE);
  if (!handler) return frame;
  if (!g.running) return handler(frame, slot->context);
  Core* c = this_core();
  const int32_t outer = c->io_device;
  c->io_device = slot->device;
  c->exit_reschedules = true;
  g_devices.note_interrupt(slot->device);
  Frame* result = handler(frame, slot->context);
  c->io_device = outer;
  if (result != frame || !c->resched) {
    if (c->resched) send_wakeup(c->apic_id);   // (the handler switched frames itself: the woken thread waits for the wake-up instead)
    c->exit_reschedules = false;
    c->resched = false;
    return result;
  }
  return reschedule(frame, c, false);
}

// ---- thread creation ----

void thread_main(Thread* t) {
  t->entry(t->argument);
  exit_thread();
}

void build_initial_frame(Thread* t, uint32_t stack_bytes) {
  uint8_t* top = t->stack + stack_bytes;  // 16-byte aligned
  Frame* f = reinterpret_cast<Frame*>(reinterpret_cast<uint64_t>(top - 64 - sizeof(Frame)) & ~uint64_t{15});
#if defined(__x86_64__)
  __builtin_memcpy(f->fpu, g_fpu_template, sizeof(f->fpu));
  f->r15 = f->r14 = f->r13 = f->r12 = f->r11 = f->r10 = f->r9 = f->r8 = 0;
  f->rdi = f->rsi = f->rbp = f->rbx = f->rdx = f->rax = 0;
  f->rcx = reinterpret_cast<uint64_t>(t);  // first argument (Microsoft x64 ABI)
  f->vector = 0;
  f->error = 0;
  f->rip = reinterpret_cast<uint64_t>(&thread_main);
  f->cs = kSelectorCode;
  f->rflags = 0x202;
  f->rsp = reinterpret_cast<uint64_t>(top) - 8;  // as if entered by a call
  f->ss = kSelectorData;
#elif defined(__aarch64__)
  for (uint64_t& r : f->x) r = 0;
  f->x[0] = reinterpret_cast<uint64_t>(t);  // first argument (AAPCS64)
  f->sp = reinterpret_cast<uint64_t>(f) + sizeof(Frame);  // where the stack is once the frame is popped
  f->elr = reinterpret_cast<uint64_t>(&thread_main);
  f->vector = f->esr = f->far = 0;
  // Resume in the exception level we are running at, on its own stack pointer (SPx), interrupts unmasked.
  f->spsr = current_el() == 2 ? 0x9 : 0x5;
#elif defined(__riscv) && __riscv_xlen == 64
  for (uint64_t& r : f->x) r = 0;
  f->x[9] = reinterpret_cast<uint64_t>(t);   // a0: first argument
  f->x[1] = reinterpret_cast<uint64_t>(f) + sizeof(Frame);   // sp: where the stack is once the frame is popped
  f->sepc = reinterpret_cast<uint64_t>(&thread_main);
  f->scause = f->stval = 0;
  f->sstatus = kSstatusSpp | kSstatusSpie;   // sret returns to supervisor mode with interrupts enabled
#elif defined(__loongarch64)
  for (uint64_t& r : f->x) r = 0;
  f->x[3] = reinterpret_cast<uint64_t>(t);   // a0 (r4): first argument
  f->x[2] = reinterpret_cast<uint64_t>(f) + sizeof(Frame);   // sp (r3): where the stack is once the frame is popped
  f->era = reinterpret_cast<uint64_t>(&thread_main);
  f->estat = f->badv = 0;
  f->prmd = kPrmdPie;   // ertn returns to PLV0 with interrupts enabled
#endif
  t->saved = f;
}

Thread* make_thread(ThreadFunction fn, void* argument, const char* name, int32_t core, int8_t kind, uint32_t stack_bytes, int32_t io_device) {
  void* memory = g.allocator->allocate(sizeof(Thread));
  if (!memory) return nullptr;
  Thread* t = new (memory) Thread();
  t->stack = static_cast<uint8_t*>(g.allocator->allocate_aligned(stack_bytes, 16));
  if (!t->stack) {
    g.allocator->deallocate(t);
    return nullptr;
  }
  t->stack_top = t->stack + stack_bytes;
  t->entry = fn;
  t->argument = argument;
  t->name = name;
  t->affinity = core;
  t->preferred_kind = kind;
  t->io_device = io_device;
  build_initial_frame(t, stack_bytes);
  register_thread(t);
  return t;
}

// A thread object for the context already running on this core (no stack of its own).
Thread* adopt_context(const char* name, bool idle, int32_t core) {
  void* memory = g.allocator->allocate(sizeof(Thread));
  if (!memory) return nullptr;
  Thread* t = new (memory) Thread();
  t->name = name;
  t->is_idle = idle;
  t->affinity = core;
  t->state = ThreadState::Running;
  t->on_cpu = true;
  register_thread(t);
  return t;
}

// A finished thread may be freed once no core holds its registers and no tick still points at it.
bool releasable(const Thread* t) { return !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) && !__atomic_load_n(&t->timer_refs, __ATOMIC_ACQUIRE); }

uint32_t reap_zombies_impl() {
  if (!__atomic_load_n(&g.zombies, __ATOMIC_ACQUIRE)) return 0;
  Thread* done = nullptr;
  const uint64_t flags = lock_irq(&g.threads_lock);
  Thread** link = &g.zombies;
  while (*link) {
    Thread* t = *link;
    if (!releasable(t)) {
      link = &t->next;
      continue;
    }
    *link = t->next;
    unregister_locked(t);
    t->next = done;
    done = t;
  }
  uint32_t left = 0;
  for (Thread* t = g.zombies; t; t = t->next) ++left;
  unlock_irq(&g.threads_lock, flags);
  while (done) {
    Thread* t = done;
    done = t->next;
    g.allocator->deallocate(t->stack);
    g.allocator->deallocate(t);
  }
  return left;
}

void idle_loop(void*) {
  for (;;) {
    reap_zombies_impl();
    idle_wait();
  }
}

// Makes the core known to placement (its kind and its cache domain, measured on the core itself).
void publish_place(Core* c, bool idle) {
  scheduling::CorePlace place;
  place.online = true;
  place.idle = idle;
  place.kind = static_cast<int8_t>(c->kind);
  place.cache_domain = this_cache_domain();
  g.places[c->index] = place;   // (before the core is online: nothing places threads on it yet)
  __atomic_store_n(&g.per_core[c->index].running_idle, idle, __ATOMIC_RELEASE);
}

#if defined(__x86_64__)
// What each application processor runs, forever, after the firmware hands it over.
void ap_main(void* context) {
  const ApStart* start = static_cast<ApStart*>(context);
  while (!__atomic_load_n(&g.go, __ATOMIC_ACQUIRE)) pause_hint();
  Core* c = &g.cores[start->core];
  interrupts_load(false);
  load_shared_pat_on_this_core();   // the same write-combining setup as the boot core
  apic::enable_this_core();
  c->self = c;
  c->index = start->core;
  c->apic_id = apic::this_apic_id();
  c->kind = g.cpu->core_kind(start->core);
  wrmsr(kMsrGsBase, reinterpret_cast<uint64_t>(c));
  Thread* idle = adopt_context("idle", true, static_cast<int32_t>(start->core));
  if (!idle) for (;;) __asm__ volatile("cli; hlt");
  c->idle = c->current = idle;
  publish_place(c, true);
  c->online = true;
  __atomic_add_fetch(&g.aps_ready, 1, __ATOMIC_SEQ_CST);
  apic::start_timer(g.tick_hz);
  for (;;) __asm__ volatile("sti; hlt");
}
#elif defined(__aarch64__)
// What each secondary core runs, forever, after PSCI started it and Arch/AArch64/Secondary.S gave it the boot core's MMU setup.
void ap_main(void* context) {
  const ApStart* start = static_cast<ApStart*>(context);
  while (!__atomic_load_n(&g.go, __ATOMIC_ACQUIRE)) pause_hint();
  Core* c = &g.cores[start->core];
  interrupts_load(false);
  gic::enable_this_core();
  c->self = c;
  c->index = start->core;
  c->apic_id = this_hardware_id();
  c->kind = g.cpu->core_kind(start->core);
  write_core_pointer(reinterpret_cast<uint64_t>(c));
  Thread* idle = adopt_context("idle", true, static_cast<int32_t>(start->core));
  if (!idle) halt_forever();
  c->idle = c->current = idle;
  publish_place(c, true);
  c->online = true;
  __atomic_add_fetch(&g.aps_ready, 1, __ATOMIC_SEQ_CST);
  gic::enable_interrupt(kVectorWakeup);
  start_tick(g.tick_hz);
  for (;;) idle_wait();
}
#elif defined(__riscv) && __riscv_xlen == 64
// What each other hart runs, forever, after SBI started it and Arch/RiscV64/Secondary.S gave it a stack.
void ap_main(void* context) {
  const ApStart* start = static_cast<ApStart*>(context);
  while (!__atomic_load_n(&g.go, __ATOMIC_ACQUIRE)) pause_hint();
  Core* c = &g.cores[start->core];
  interrupts_load(false);
  c->self = c;
  c->index = start->core;
  c->apic_id = arch_riscv64::this_hart_id();
  c->kind = g.cpu->core_kind(start->core);
  write_core_pointer(reinterpret_cast<uint64_t>(c));
  Thread* idle = adopt_context("idle", true, static_cast<int32_t>(start->core));
  if (!idle) halt_forever();
  c->idle = c->current = idle;
  publish_place(c, true);
  c->online = true;
  __atomic_add_fetch(&g.aps_ready, 1, __ATOMIC_SEQ_CST);
  start_tick(g.tick_hz);
  for (;;) idle_wait();
}
#elif defined(__loongarch64)
// What each other core runs, forever, after the mailbox start and Arch/LoongArch64/Secondary.S gave it a stack.
void ap_main(void* context) {
  const ApStart* start = static_cast<ApStart*>(context);
  while (!__atomic_load_n(&g.go, __ATOMIC_ACQUIRE)) pause_hint();
  Core* c = &g.cores[start->core];
  interrupts_load(false);
  c->self = c;
  c->index = start->core;
  c->apic_id = this_hardware_id();
  c->kind = g.cpu->core_kind(start->core);
  write_core_pointer(reinterpret_cast<uint64_t>(c));
  Thread* idle = adopt_context("idle", true, static_cast<int32_t>(start->core));
  if (!idle) halt_forever();
  c->idle = c->current = idle;
  publish_place(c, true);
  c->online = true;
  __atomic_add_fetch(&g.aps_ready, 1, __ATOMIC_SEQ_CST);
  start_tick(g.tick_hz);
  for (;;) idle_wait();
}
#endif

}  // namespace

extern "C" void tcos_finish_switch() { finish_switch(); }      // (called from the trap stubs: isr_common in Interrupts.S, Vectors.S, Trap.S)

// ---------------------------------------------------------------- public API

bool running() { return g.running; }

uint32_t reap_zombies() { return reap_zombies_impl(); }

uint32_t current_core() { return g.running ? this_core()->index : 0; }
uint32_t core_count() { return g.running ? g.core_count : 1; }

uint32_t tick_rate() { return g.tick_hz; }

bool core_info(uint32_t core, CoreInfo* out) {
  if (!g.running || core >= g.core_count) return false;
  const Core& c = g.cores[core];   // (counters that core alone writes)
  out->online = c.online;
  out->kind = c.kind;
  out->busy_ticks = __atomic_load_n(&c.busy_ticks, __ATOMIC_RELAXED);
  out->idle_ticks = __atomic_load_n(&c.idle_ticks, __ATOMIC_RELAXED);
  out->lock_free_ticks = __atomic_load_n(&c.lock_free_ticks, __ATOMIC_RELAXED);
  return true;
}

uint32_t thread_snapshot(ThreadInfo* out, uint32_t capacity) {
  if (!g.running) return 0;
  uint32_t n = 0;
  const uint64_t flags = lock_irq(&g.threads_lock);   // (the threads stay; their fields are a snapshot, read without their own locks)
  for (const Thread* t = g.all; t && n < capacity; t = t->all_next) {
    if (state_of(t) == ThreadState::Zombie) continue;
    ThreadInfo& info = out[n++];
    info.id = t->id;
    uint32_t i = 0;
    for (; i + 1 < sizeof info.name && t->name && t->name[i]; ++i) info.name[i] = t->name[i];
    info.name[i] = 0;
    info.state = state_of(t);
    info.idle = t->is_idle;
    info.core = -1;
    for (uint32_t c = 0; c < g.core_count; ++c)
      if (__atomic_load_n(&g.cores[c].current, __ATOMIC_RELAXED) == t) info.core = static_cast<int32_t>(c);
    info.affinity = t->affinity;
    info.io_core = io_core_of(t);
    info.io_device = t->io_device;
    info.cpu_ticks = __atomic_load_n(&t->cpu_ticks, __ATOMIC_RELAXED);
  }
  unlock_irq(&g.threads_lock, flags);
  return n;
}

#if defined(__x86_64__)
UserEntry* core_user_entry(uint32_t core) { return core < kMaxCores ? &g.cores[core].user : nullptr; }
uint64_t core_page(uint32_t core) { return core < kMaxCores ? reinterpret_cast<uint64_t>(&g.cores[core]) : 0; }
void set_user_tick_hook(UserTickHook hook) { g_user_tick = hook; }
uint64_t core_entry_stack_top(uint32_t core) {
  return core < kMaxCores ? reinterpret_cast<uint64_t>(g.cores[core].entry_stack) + sizeof(g.cores[core].entry_stack) : 0;
}
void set_current_user_bases(bool set_fs, uint64_t fs, bool set_gs, uint64_t gs) {
  const uint64_t flags = irq_save();
  Thread* t = this_core()->current;
  if (set_fs) {
    t->user_fs = fs;
    wrmsr(kMsrFsBase, fs);
  }
  if (set_gs) {
    t->user_gs = gs;
    wrmsr(kMsrKernelGsBase, gs);      // while in the kernel the program's GS base waits in the other register (SWAPGS brings it back)
  }
  irq_restore(flags);
}
int32_t set_thread_user_cr3(Thread* t, uint64_t cr3) {
  int32_t running_on = -1;
  if (!__atomic_load_n(&t->user_cr3, __ATOMIC_ACQUIRE)) return -1;   // (a thread that has not started in user mode yet takes the tables it finds when it does: see set_current_user_context)
  __atomic_store_n(&t->user_cr3, cr3, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < g.core_count; ++i) {
    // Under each core's lock, which every switch on that core takes: either the switch onto `t` read the new tables above, or `t` is current here
    // and gets them now (what that core returns to ring 3 with: a store of one word).
    const uint64_t flags = lock_irq(&g.per_core[i].lock);
    if (g.cores[i].current == t) {
      g.cores[i].user.user_cr3 = cr3;
      running_on = static_cast<int32_t>(i);
    }
    unlock_irq(&g.per_core[i].lock, flags);
  }
  return running_on;
}

bool wait_for_kernel_entry(const CoreMask& cores) {
  uint64_t before[kMaxCores] = {};
  const uint32_t self = current_core();
  for (uint32_t i = 0; i < g.core_count; ++i) {
    if (i == self || !cores.has(i)) continue;
    before[i] = __atomic_load_n(&g.cores[i].kernel_entries, __ATOMIC_SEQ_CST);
    send_wakeup(g.cores[i].apic_id);
  }
  const uint64_t limit = hardware::read_tsc() + hardware::tsc_ticks_per_us() * 1000000;      // (a second: a core that does not answer is stuck anyway)
  for (uint32_t i = 0; i < g.core_count; ++i) {
    if (i == self || !cores.has(i)) continue;
    while (__atomic_load_n(&g.cores[i].kernel_entries, __ATOMIC_SEQ_CST) == before[i]) {
      if (hardware::read_tsc() > limit) return false;
      pause_hint();
    }
  }
  return true;
}
void set_current_user_context(uint64_t cr3, void* context) {
  const uint64_t flags = irq_save();
  Core* c = this_core();
  Thread* t = c->current;
  if (cr3 && !t->user_cr3) {      // the thread pointers the program starts with
    wrmsr(kMsrKernelGsBase, t->user_gs);
    wrmsr(kMsrFsBase, t->user_fs);
  }
  t->user_cr3 = cr3;
  t->user_context = context;
  c->user.user_cr3 = cr3;
  irq_restore(flags);
}
#endif

void set_idle_mwait_hint(uint32_t hint) {
#if defined(__x86_64__)
  __atomic_store_n(&g_idle_mwait_hint, hint, __ATOMIC_RELAXED);
#else
  (void)hint;
#endif
}

bool run_on_every_core(void (*fn)(void*), void* argument, uint64_t timeout_us) {
  if (!g.running) {
    fn(argument);
    return true;
  }
  while (__atomic_exchange_n(&g_core_work.busy, 1u, __ATOMIC_ACQUIRE)) yield();
  const uint64_t flags = irq_save();                                    // (this core stays this core while it is read and while the function runs)
  Core* self = this_core();
  const uint64_t others = online_core_mask() & ~(self->index < 64 ? uint64_t{1} << self->index : 0);
  g_core_work.fn = fn;
  g_core_work.argument = argument;
  __atomic_store_n(&g_core_work.pending, others, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < g.core_count && i < 64; ++i)
    if (others & (uint64_t{1} << i)) send_wakeup(g.cores[i].apic_id);
  fn(argument);
  irq_restore(flags);
  uint64_t waited = 0;
  while (__atomic_load_n(&g_core_work.pending, __ATOMIC_ACQUIRE) != 0 && waited < timeout_us) {
    hardware::delay_us(50);
    waited += 50;
  }
  const bool done = __atomic_load_n(&g_core_work.pending, __ATOMIC_ACQUIRE) == 0;
  __atomic_store_n(&g_core_work.pending, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_core_work.busy, 0u, __ATOMIC_RELEASE);
  return done;
}

uint64_t online_core_mask() {
  uint64_t mask = 0;
  for (uint32_t i = 0; i < g.core_count && i < 64; ++i) if (!g.running || g.cores[i].online) mask |= uint64_t{1} << i;
  return g.running ? mask : 1;
}

bool set_thread_core_mask(Thread* t, uint64_t mask) {
  const uint64_t online = online_core_mask();
  bool beyond = g.core_count > 64;
  if (!(mask & online) && !beyond) return false;
  __atomic_store_n(&t->core_mask, mask, __ATOMIC_RELEASE);   // (read once per decision by the cores that place and pick threads)
  return true;
}

void set_thread_affinity(Thread* t, int32_t core) { __atomic_store_n(&t->affinity, core, __ATOMIC_RELEASE); }

void set_thread_priority(Thread* t, uint8_t priority) {
  if (priority >= kPriorityLevels) priority = kPriorityLevels - 1;
  __atomic_store_n(&t->priority, priority, __ATOMIC_RELEASE);   // (a queued thread stays at the level it was queued at: Thread::queued_level)
}

Thread* current_thread() {
  if (!g.running) return nullptr;
  const uint64_t flags = irq_save();   // (the core and its current thread read together: preempted in between, this could be another core's)
  Thread* t = this_core()->current;
  irq_restore(flags);
  return t;
}

int32_t register_io_device(const char* name, int32_t interrupt_core) { return g_devices.add(StringView(name ? name : ""), interrupt_core); }

void unregister_io_device(int32_t device) {
  if (!g_devices.live(device)) return;
  set_io_device_retarget(device, nullptr, nullptr);
  {
    const uint64_t flags = lock_irq(&g.threads_lock);   // (threads bound to it go back to learning: the slot may soon be another device's)
    for (Thread* t = g.all; t; t = t->all_next)
      if (t->io_device == device) __atomic_store_n(&t->io_device, -1, __ATOMIC_RELEASE);
    unlock_irq(&g.threads_lock, flags);
  }
  g_devices.remove(device);
}

bool set_io_device_core(int32_t device, int32_t core) { return g_devices.set_interrupt_core(device, core); }

bool io_device_info(int32_t device, scheduling::IoDeviceInfo* out) { return out && g_devices.info(device, out); }

bool set_io_device_node(int32_t device, int64_t node) { return g_devices.set_numa_node(device, node); }

uint32_t choose_io_interrupt_core(int8_t kind, int64_t node) {
  if (!g.running) return 0;
  uint32_t per_core[kMaxCores];
  g_devices.devices_per_core(ArrayView<uint32_t>(per_core, g.core_count));
  const int32_t core = scheduling::choose_interrupt_core(places(), ArrayView<const uint32_t>(per_core, g.core_count), scheduling::CoreSet::all(), kind,
                                                         static_cast<int32_t>(g.boot_core), node);
  return core < 0 ? g.boot_core : static_cast<uint32_t>(core);
}

int32_t core_from_hardware_id(uint64_t hardware_id) {
  if (!g.running) return -1;
  for (uint32_t i = 0; i < g.core_count; ++i)
    if (g.cores[i].online && g.cores[i].apic_id == hardware_id) return static_cast<int32_t>(i);
  return -1;
}

bool set_core_numa_node(uint32_t core, uint32_t node) {
  if (!g.running || core >= g.core_count || !g.cores[core].online) return false;
  __atomic_store_n(&g.places[core].numa_node, node, __ATOMIC_RELEASE);
  return true;
}

bool set_numa_distance(uint32_t from, uint32_t to, uint8_t distance) {
  if (from >= scheduling::NumaDistances::kMaxNodes || to >= scheduling::NumaDistances::kMaxNodes) return false;
  __atomic_store_n(&g.numa.distance[from][to], distance, __ATOMIC_RELEASE);
  return true;
}

uint64_t core_hardware_id(uint32_t core) {
  if (!g.running || core >= g.core_count || !g.cores[core].online) return ~uint64_t{0};
  return g.cores[core].apic_id;
}

bool set_device_interrupt_handler(Vector vector, int32_t device, InterruptHandler handler, void* context) {
  while (__atomic_exchange_n(&g_device_vectors_lock, 1u, __ATOMIC_ACQUIRE)) pause_hint();
  DeviceVector* slot = nullptr;
  DeviceVector* unused = nullptr;
  for (DeviceVector& candidate : g_device_vectors) {
    if (candidate.used && candidate.vector == vector) slot = &candidate;
    else if (!candidate.used && !unused) unused = &candidate;
  }
  bool ok = true;
  set_interrupt_handler(vector, nullptr, nullptr);
  if (slot) __atomic_store_n(&slot->handler, static_cast<InterruptHandler>(nullptr), __ATOMIC_RELEASE);   // (an interrupt already in on another core sees no handler)
  if (handler) {
    if (!slot) slot = unused;
    if (slot) {
      slot->vector = vector;
      slot->used = true;
      slot->device = device;
      slot->context = context;
      __atomic_store_n(&slot->handler, handler, __ATOMIC_RELEASE);
      set_interrupt_handler(vector, on_device_interrupt, slot);
    } else {
      ok = false;
    }
  } else if (slot) {
    slot->used = false;
  }
  __atomic_store_n(&g_device_vectors_lock, 0u, __ATOMIC_RELEASE);
  return ok;
}

namespace {
// Makes the interrupt moves note_wake_source_locked decided on, outside any lock (the drivers write PCI configuration space or MMIO).
void io_balancer_main(void*) {
  for (;;) {
    const uint64_t flags = lock_irq(&g.balancer_wait.lock);
    int32_t device = -1;
    for (uint32_t i = 0; i < scheduling::IoDeviceTable::kCapacity && device < 0; ++i)
      if (g.steering[i].move_to >= 0 && g.steering[i].retarget) device = static_cast<int32_t>(i);
    if (device < 0) {
      detail::prepare_wait(&g.balancer_wait, 0);
      unlock_irq(&g.balancer_wait.lock, flags);
      yield();
      detail::finish_wait(&g.balancer_wait);
      continue;
    }
    Scheduler::Steering& steering = g.steering[device];
    const uint32_t to = static_cast<uint32_t>(steering.move_to);
    const IoRetarget retarget = steering.retarget;
    void* context = steering.context;
    const uint64_t hardware_id = g.cores[to].apic_id;
    steering.move_to = -1;
    __atomic_store_n(&g.balancer_busy, device, __ATOMIC_RELEASE);
    unlock_irq(&g.balancer_wait.lock, flags);
    if (retarget(context, to, hardware_id)) g_devices.set_interrupt_core(device, static_cast<int32_t>(to));
    __atomic_store_n(&g.balancer_busy, -1, __ATOMIC_RELEASE);
  }
}
}  // namespace

bool set_io_device_retarget(int32_t device, IoRetarget retarget, void* context) {
  if (!g_devices.live(device) || static_cast<uint32_t>(device) >= scheduling::IoDeviceTable::kCapacity) return false;
  const uint64_t flags = lock_irq(&g.balancer_wait.lock);
  Scheduler::Steering& steering = g.steering[device];
  steering.retarget = retarget;
  steering.context = context;
  steering.tally = scheduling::InterruptSteering();
  steering.move_to = -1;
  unlock_irq(&g.balancer_wait.lock, flags);
  if (!retarget) {   // (the driver may free the context once this returns)
    while (__atomic_load_n(&g.balancer_busy, __ATOMIC_ACQUIRE) == device) yield();
    return true;
  }
  if (g.running && !__atomic_exchange_n(&g.balancer_started, true, __ATOMIC_ACQ_REL)) {   // (the first driver to ask starts it)
    Thread* balancer = spawn(io_balancer_main, nullptr, "io-balancer", -1, -1, 64 * 1024);
    if (balancer) {
      detach(balancer);
      __atomic_store_n(&g.balancer, balancer, __ATOMIC_RELEASE);
    } else {
      __atomic_store_n(&g.balancer_started, false, __ATOMIC_RELEASE);
    }
  }
  return true;
}

void bind_thread_to_io_device(Thread* t, int32_t device) {
  if (!t) return;
  __atomic_store_n(&t->io_device, device, __ATOMIC_RELEASE);
}

bool start(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator, uint32_t tick_hz) {
  if (g.running || !hardware::tsc_ticks_per_us()) return false;
  g.cpu = &cpu;
  g.allocator = &allocator;
  g.tsc_per_us = hardware::tsc_ticks_per_us();
  g.tick_hz = tick_hz;
  g.boot_core = cpu.current_core();
  g.core_count = cpu.core_count() < kMaxCores ? cpu.core_count() : kMaxCores;
  // The kernel runs no static constructors, so the fields whose "none" is not zero are set here, before any core or interrupt reads them.
  for (Core& c : g.cores) {
    c.io_device = -1;
    c.resched = false;
    c.exit_reschedules = false;
  }
  for (Scheduler::Steering& steering : g.steering) steering.move_to = -1;
  for (PerCore& pc : g.per_core) {
    pc.next_wake_tsc = ~uint64_t{0};
    pc.running_idle = true;
  }
  g.balancer_busy = -1;
#if defined(__x86_64__)
  __asm__ volatile("fxsave %0" : "=m"(g_fpu_template));
#endif
  // Application processors are started first, while the firmware's services still
  // work normally; they wait in ap_main until the boot core has set everything up.
  uint32_t expected = 0;
  for (uint32_t core = 0; core < g.core_count; ++core) {
    if (core == g.boot_core) continue;
    g.ap_start[core].core = core;
    if (cpu.start_on_core(core, ap_main, &g.ap_start[core])) ++expected;
  }
  const uint64_t flags = irq_save();
  interrupts_init();
  interrupts_load(true);
#if defined(__x86_64__)
  const bool controller_ok = apic::init_boot_core();
#elif defined(__aarch64__)
  const bool controller_ok = gic::version() != 0;   // the caller brought the GIC up (it needs the ACPI tables)
#else
  const bool controller_ok = stimer::available();   // the tick needs only the SBI timer extension
#endif
  if (!controller_ok) {
    interrupts_restore_firmware();
    irq_restore(flags);
    return false;
  }
#if defined(__x86_64__) || defined(__riscv) || defined(__loongarch64)
  set_interrupt_handler(kVectorTimer, on_timer, nullptr);
#else
  set_interrupt_handler(static_cast<Vector>(gtimer::interrupt()), on_timer, nullptr);
#endif
  set_interrupt_handler(kVectorYield, on_yield, nullptr);
  set_interrupt_handler(kVectorWakeup, on_wakeup, nullptr);
#if defined(__aarch64__)
  gic::enable_interrupt(kVectorWakeup);
#endif

  Core* boot = &g.cores[g.boot_core];
  boot->self = boot;
  boot->index = g.boot_core;
  boot->apic_id = this_hardware_id();
  boot->kind = cpu.core_kind(g.boot_core);
  g.firmware_gs_base = swap_core_pointer(boot);

  // The caller becomes the main thread, pinned here so it may keep calling firmware.
  Thread* main_thread = adopt_context("main", false, static_cast<int32_t>(g.boot_core));
  Thread* idle = make_thread(idle_loop, nullptr, "idle", static_cast<int32_t>(g.boot_core), -1, 16 * 1024, -1);
  if (!main_thread || !idle) {
    restore_core_pointer(g.firmware_gs_base);
    interrupts_restore_firmware();
    irq_restore(flags);
    return false;
  }
  idle->is_idle = true;
  idle->state = ThreadState::Ready;
  main_thread->id = g.next_id++;
  boot->current = main_thread;
  boot->idle = idle;
  main_thread->last_core = static_cast<int32_t>(g.boot_core);
  publish_place(boot, false);
  boot->online = true;
  g.running = true;

  __atomic_store_n(&g.go, true, __ATOMIC_RELEASE);
  for (int i = 0; i < 2000 && __atomic_load_n(&g.aps_ready, __ATOMIC_SEQ_CST) < expected; ++i) hardware::delay_us(1000);
  start_tick(tick_hz);
  hardware::set_sleep_function(sleep_us);  // long driver delays now give the core away
  irq_restore(with_interrupts_enabled(flags));
  irq_enable();
  firmware_service_start();  // from now on firmware calls go through kernel::firmware()
  return true;
}

void stop() {
  if (!g.running) return;
  hardware::set_sleep_function(nullptr);
  g.stopping = true;
  for (uint32_t i = 0; i < g.core_count; ++i) {
    Core& c = g.cores[i];
    if (!c.online || i == g.boot_core) continue;
    send_wakeup(c.apic_id);
    for (int k = 0; k < 100 && !c.parked; ++k) hardware::delay_us(1000);
  }
  irq_disable();
  stop_tick();
  g.running = false;
  restore_core_pointer(g.firmware_gs_base);
  interrupts_restore_firmware();
#if defined(__aarch64__)
  gic::restore_firmware();
#endif
  irq_enable();
}

Thread* spawn(ThreadFunction fn, void* argument, const char* name, int32_t core, int8_t preferred_kind,
              uint32_t stack_bytes, int32_t io_device) {
  if (!g.running || !fn) return nullptr;
  reap_zombies_impl();
  Thread* t = make_thread(fn, argument, name, core, preferred_kind, stack_bytes, io_device);
  if (!t) return nullptr;
  t->id = __atomic_fetch_add(&g.next_id, 1, __ATOMIC_RELAXED);
  const uint64_t flags = lock_irq(&t->wake_lock);
  set_state(t, ThreadState::Ready);
  enqueue_ready(t);
  unlock_irq(&t->wake_lock, flags);
  return t;
}

void yield() {
  if (!g.running) return;
  // Giving up the core with interrupts masked means the caller holds a spinlock
  // (or is in an interrupt-off section): another thread would then run inside
  // that critical section. That is always a bug, so stop at once.
  if (!irq_enabled()) panic("yield or blocking wait with interrupts disabled (a spinlock held?)", nullptr);
#if defined(__x86_64__)
  __asm__ volatile("int %0" ::"i"(kVectorYield) : "memory");
#elif defined(__aarch64__)
  __asm__ volatile("svc #0" ::: "memory");   // a synchronous exception: the dispatcher calls the yield handler
#elif defined(__loongarch64)
  __asm__ volatile("break 0" ::: "memory");  // a synchronous exception (always 4 bytes, the handler skips it): the dispatcher calls the yield handler
#else
  // A synchronous exception: the dispatcher calls the yield handler. An ECALL would reach the SBI firmware
  // instead, so EBREAK (always 4 bytes here, the handler skips it) is the self-trap.
  __asm__ volatile(".option push\n.option norvc\nebreak\n.option pop" ::: "memory");
#endif
}

void sleep_us(uint64_t us) {
  if (!g.running) {
    hardware::delay_spin_us(us);
    return;
  }
  if (us == 0) {
    yield();
    return;
  }
  detail::prepare_wait(nullptr, us);
  yield();
  detail::finish_wait(nullptr);
}

void exit_thread() {
  Thread* me = current_thread();
  // Zombie under the joiners' lock, so that a join either sees it or is on the list when they are woken.
  uint64_t flags = detail::lock_list(&me->joiners);
  spin_lock(&me->wake_lock);
  set_state(me, ThreadState::Zombie);
  spin_unlock(&me->wake_lock);
  detail::wake_all(&me->joiners);
  detail::unlock_list(&me->joiners, flags);
  flags = lock_irq(&g.threads_lock);
  if (me->detached && !me->zombie_listed) {   // (detach may have listed it already, having seen the Zombie)
    me->zombie_listed = true;
    me->next = g.zombies;
    g.zombies = me;
  }
  unlock_irq(&g.threads_lock, flags);
  for (;;) yield();
}

bool join(Thread* t, uint64_t timeout_us) {
  if (!t) return false;
  // Once the scheduler has been stopped no thread runs any more, so none can finish: waiting for one would never end.
  if (!g.running) return false;
  const uint64_t deadline = timeout_us ? hardware::read_tsc() + timeout_us * g.tsc_per_us : 0;
  for (;;) {
    const uint64_t flags = detail::lock_list(&t->joiners);
    if (state_of(t) == ThreadState::Zombie) {
      detail::unlock_list(&t->joiners, flags);
      break;
    }
    uint64_t remaining = 0;
    if (deadline) {
      const int64_t left = static_cast<int64_t>(deadline - hardware::read_tsc());
      if (left <= 0) {
        detail::unlock_list(&t->joiners, flags);
        return false;
      }
      remaining = static_cast<uint64_t>(left) / g.tsc_per_us + 1;
    }
    detail::prepare_wait(&t->joiners, remaining);
    detail::unlock_list(&t->joiners, flags);
    yield();
    detail::finish_wait(&t->joiners);
  }
  while (!releasable(t)) yield();  // its core is still leaving it, or a tick still points at it
  {
    const uint64_t flags = lock_irq(&g.threads_lock);
    unregister_locked(t);
    unlock_irq(&g.threads_lock, flags);
  }
  g.allocator->deallocate(t->stack);
  g.allocator->deallocate(t);
  return true;
}

void detach(Thread* t) {
  if (!t) return;
  const uint64_t flags = lock_irq(&g.threads_lock);
  t->detached = true;
  if (state_of(t) == ThreadState::Zombie && !t->zombie_listed) {
    t->zombie_listed = true;
    t->next = g.zombies;
    g.zombies = t;
  }
  unlock_irq(&g.threads_lock, flags);
}

namespace detail {

uint64_t lock_list(WaitList* list) { return lock_irq(&list->lock); }
void unlock_list(WaitList* list, uint64_t flags) { unlock_irq(&list->lock, flags); }

void prepare_wait(WaitList* list, uint64_t timeout_us) {
  const uint64_t flags = irq_save();   // (this core stays this core: the timeout goes on its sleepers)
  Core* c = this_core();
  Thread* t = c->current;
  spin_lock(&t->wake_lock);
  ++t->wait_seq;
  t->timed_out = false;
  set_state(t, ThreadState::Blocked);
  spin_unlock(&t->wake_lock);
  if (list) {
    wait_append(list, t);
    t->waiting_on = list;
  }
  if (timeout_us) {
    PerCore& pc = g.per_core[c->index];
    spin_lock(&pc.lock);
    t->wake_tsc = hardware::read_tsc() + timeout_us * g.tsc_per_us;
    t->sleep_seq = t->wait_seq;
    t->sleep_core = static_cast<int32_t>(c->index);
    sleepers_insert_locked(pc, t);
    spin_unlock(&pc.lock);
  }
  irq_restore(flags);
}

bool finish_wait(WaitList* list) {
  Thread* t = current_thread();
  if (t->sleep_core >= 0) {   // the timeout, unless the tick took it already
    PerCore& pc = g.per_core[t->sleep_core];
    const uint64_t flags = lock_irq(&pc.lock);
    if (t->in_sleepers) sleepers_remove_locked(pc, t);
    unlock_irq(&pc.lock, flags);
    t->sleep_core = -1;
  }
  if (list) {   // the list, unless the waker took it off (a thread whose timeout ended the wait is still on it)
    const uint64_t flags = lock_list(list);
    if (t->waiting_on == list) {
      wait_remove(list, t);
      t->waiting_on = nullptr;
    }
    unlock_list(list, flags);
  }
  return t->timed_out;
}

bool wake_one(WaitList* list) { return wake_one_impl(list); }

uint32_t wake_all(WaitList* list) {
  uint32_t n = 0;
  while (wake_one(list)) ++n;
  return n;
}

}  // namespace detail

}  // namespace kernel
