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

struct Scheduler {
  uint32_t lock_word = 0;
  Core cores[kMaxCores];
  ApStart ap_start[kMaxCores];
  uint32_t core_count = 1;
  uint32_t boot_core = 0;
  // The ready threads: a run queue per core, each kept apart by priority level (levels[0] the lowest). A thread waits in the queue of the core it
  // was sent to, or of the core that suits it best when no allowed core was idle (choose_queue_core). Any core may take from any queue (see
  // find_eligible): the queues say where a thread is expected to run, not where it must. level_counts / ready_levels count the ready threads of
  // each level over all queues, so a core finds the highest level with work without looking at every queue.
  struct RunQueue {
    WaitList levels[kPriorityLevels];
  } run_queues[kMaxCores];
  uint32_t queue_lengths[kMaxCores];
  uint32_t level_counts[kPriorityLevels];
  uint32_t ready_levels = 0;   // bit L: level_counts[L] != 0
  // What placement knows of each core (CPUScheduler/IoAffinity.hpp). `idle` is cleared as soon as a thread is sent to the core, so that the
  // next thread made ready goes to another idle core; it is set again whenever the core switches to its idle thread.
  scheduling::CorePlace places[kMaxCores];
  // Interrupt steering (set_io_device_retarget), by device id: the driver's callback, the tally of where woken threads run, and a move the
  // balancer thread is to make (-1: none). The balancer sleeps on balancer_wait until a decision is made.
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
  Thread* sleepers = nullptr;
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

// ---- the scheduler lock (spin + interrupts masked) ----

uint64_t lock_irq() {
  const uint64_t flags = irq_save();
  while (__atomic_exchange_n(&g.lock_word, 1, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&g.lock_word, __ATOMIC_RELAXED)) pause_hint();
  }
  return flags;
}
void unlock_irq(uint64_t flags) {
  __atomic_store_n(&g.lock_word, 0, __ATOMIC_RELEASE);
  irq_restore(flags);
}

// ---- the list of all threads (for monitoring); lock held for unregister ----

void register_thread(Thread* t) {
  const uint64_t flags = lock_irq();
  t->all_next = g.all;
  g.all = t;
  unlock_irq(flags);
}

void unregister_locked(Thread* t) {
  for (Thread** link = &g.all; *link; link = &(*link)->all_next) {
    if (*link == t) {
      *link = t->all_next;
      return;
    }
  }
}

// ---- run queue / wait lists (doubly linked through prev/next); lock held ----

void list_append(WaitList* list, Thread* t) {
  t->next = nullptr;
  t->prev = list->tail;
  if (list->tail) list->tail->next = t;
  else list->head = t;
  list->tail = t;
}

void list_remove(WaitList* list, Thread* t) {
  if (t->prev) t->prev->next = t->next;
  else list->head = t->next;
  if (t->next) t->next->prev = t->prev;
  else list->tail = t->prev;
  t->prev = t->next = nullptr;
}

static_assert(kPriorityLevels <= 32, "ready_levels has a bit per level");

void queue_on_locked(Thread* t, uint32_t core) {
  const uint8_t level = t->priority;
  list_append(&g.run_queues[core].levels[level], t);
  ++g.queue_lengths[core];
  if (g.level_counts[level]++ == 0) g.ready_levels |= 1u << level;
  t->queued_on = static_cast<int32_t>(core);
  t->queued_level = level;
}

void unqueue_locked(Thread* t) {
  if (t->queued_on < 0) return;
  const uint32_t core = static_cast<uint32_t>(t->queued_on);
  list_remove(&g.run_queues[core].levels[t->queued_level], t);
  --g.queue_lengths[core];
  if (--g.level_counts[t->queued_level] == 0) g.ready_levels &= ~(1u << t->queued_level);
  t->queued_on = -1;
}

void sleepers_insert(Thread* t) {
  Thread** link = &g.sleepers;
  while (*link && static_cast<int64_t>((*link)->wake_tsc - t->wake_tsc) <= 0) link = &(*link)->sleep_next;
  t->sleep_next = *link;
  *link = t;
  t->in_sleepers = true;
}

void sleepers_remove(Thread* t) {
  Thread** link = &g.sleepers;
  while (*link && *link != t) link = &(*link)->sleep_next;
  if (*link) *link = t->sleep_next;
  t->sleep_next = nullptr;
  t->in_sleepers = false;
}

// ---- device-aware placement (CPUScheduler/IoAffinity.hpp); lock held ----

ArrayView<const scheduling::CorePlace> places() { return ArrayView<const scheduling::CorePlace>(g.places, g.core_count); }

scheduling::CoreSet allowed_cores(const Thread* t) {
  scheduling::CoreSet set;
  if (t->affinity >= 0) {
    set.add(static_cast<uint32_t>(t->affinity));
    return set;
  }
  set.words[0] = t->core_mask;
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

// A thread that has just become ready joins the run queue of the idle core that suits it best, which is woken for it. When that is this core
// (idle, so in an interrupt) and the interrupt reschedules on its way out (the tick, on_device_interrupt), it is only marked; otherwise it
// interrupts itself. No idle core: it joins the queue of the busy core that suits it best (choose_queue_core) and waits for a core to give up
// its current thread.
void ready_locked(Thread* t) {
  t->ready_tsc = hardware::read_tsc();
  t->wake_core = -1;
  const scheduling::PlacementHints hints = hints_of(t);
  const scheduling::CoreSet allowed = allowed_cores(t);
  const int32_t target = scheduling::choose_wake_core(hints, allowed, places());
  if (target < 0) {
    int32_t core = scheduling::choose_queue_core(hints, allowed, places(), ArrayView<const uint32_t>(g.queue_lengths, g.core_count));
    if (core < 0) core = static_cast<int32_t>(g.running ? this_core()->index : g.boot_core);   // (no allowed core online: any core may still take it)
    queue_on_locked(t, static_cast<uint32_t>(core));
    return;
  }
  queue_on_locked(t, static_cast<uint32_t>(target));
  t->wake_core = target;
  g.places[target].idle = false;   // (spoken for: the next thread made ready looks for another core)
  Core& c = g.cores[target];
  if (g.running && &c == this_core() && c.exit_reschedules) {
    c.resched = true;
    return;
  }
  send_wakeup(c.apic_id);
}

bool eligible(const Thread* t, const Core* c);
bool wake_one_locked_impl(WaitList* list);

// Where a thread woken by this core's device interrupt will run when it cannot run here (pinned elsewhere, or wanting another kind of core): the
// core it is pinned to, else the core it ran on last. -1 when it can run here (or has never run).
int32_t remote_consumer(const Thread* t, const Core* self) {
  const bool kind_ok = t->preferred_kind < 0 || t->preferred_kind == static_cast<int8_t>(self->kind);
  if (eligible(t, self) && kind_ok) return -1;
  return t->affinity >= 0 ? t->affinity : t->last_core;
}

// With the lock held, in whatever context makes `t` ready: inside a device's interrupt handler the thread learns that core, and, for a device
// whose interrupt may be moved, the wake-up counts toward moving it to where its threads run.
void note_wake_source_locked(Thread* t) {
  if (!g.running) return;
  const Core* self = this_core();
  const int32_t device = self->io_device;
  if (device < 0 || t == g.balancer) return;
  t->learned_io.woken_by_interrupt_on(self->index);
  g_devices.note_wake(device);
  if (static_cast<uint32_t>(device) >= scheduling::IoDeviceTable::kCapacity) return;
  Scheduler::Steering& steering = g.steering[device];
  if (!steering.retarget) return;
  steering.tally.note(remote_consumer(t, self));
  if (!steering.tally.window_full()) return;
  const int32_t to = steering.tally.decide();
  if (to < 0 || static_cast<uint32_t>(to) == self->index || static_cast<uint32_t>(to) >= g.core_count || !g.cores[to].online) return;
  steering.move_to = to;
  wake_one_locked_impl(&g.balancer_wait);
}

void enqueue_locked(Thread* t) {
  t->state = ThreadState::Ready;
  ready_locked(t);
}

void make_ready_locked(Thread* t) {
  if (t->state == ThreadState::Zombie || t->state == ThreadState::Ready) return;
  t->state = ThreadState::Ready;
  note_wake_source_locked(t);
  if (!t->on_cpu) ready_locked(t);  // (else its core queues it as soon as it has saved the registers)
}

bool wake_one_locked_impl(WaitList* list) {
  Thread* t = list->head;
  if (!t) return false;
  list_remove(list, t);
  t->waiting_on = nullptr;
  if (t->in_sleepers) sleepers_remove(t);
  make_ready_locked(t);
  return true;
}

bool eligible(const Thread* t, const Core* c) {
  if (t->affinity >= 0 && static_cast<uint32_t>(t->affinity) != c->index) return false;
  if (c->index < 64 && !((t->core_mask >> c->index) & 1)) return false;
  return true;
}

// The thread was sent to another core that is still idle (that core has been woken and is about to take it).
bool sent_elsewhere(const Thread* t, const Core* c) {
  if (t->wake_core < 0 || static_cast<uint32_t>(t->wake_core) == c->index) return false;
  const Core& target = g.cores[t->wake_core];
  return target.online && target.current && target.current->is_idle;
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

// The thread this core runs next. Priorities stay strict: the highest level with an eligible thread in any queue wins. At that level this core
// takes from its own queue first (Pick), and from the other queues (work stealing) when its own has nothing it may run - or when a thread
// there has waited past its patience, so that a core busy with higher-priority work cannot strand the threads queued on it. A list is in the
// order its threads became ready, so the first eligible thread of another queue is the only one that can be the oldest.
Thread* find_eligible(const Core* c) {
  const uint64_t now = hardware::read_tsc();
  const int64_t patience = static_cast<int64_t>(kPlacementPatienceUs * g.tsc_per_us);
  for (uint32_t levels = g.ready_levels; levels;) {
    const uint32_t level = 31 - static_cast<uint32_t>(__builtin_clz(levels));
    levels &= ~(1u << level);
    Pick own;
    for (Thread* t = g.run_queues[c->index].levels[level].head; t; t = t->next) consider(own, t, c, now, patience);
    if (own.aged) return own.thread;
    Pick stolen;
    for (uint32_t i = 0; i < g.core_count; ++i) {
      if (i == c->index || !g.queue_lengths[i]) continue;
      for (Thread* t = g.run_queues[i].levels[level].head; t; t = t->next) {
        if (own.thread) {   // (only an aged thread could beat it: the first eligible one is the oldest here)
          if (!eligible(t, c)) continue;
          if (static_cast<int64_t>(now - t->ready_tsc) >= patience) consider(stolen, t, c, now, patience);
          break;
        }
        consider(stolen, t, c, now, patience);
      }
    }
    if (stolen.aged || (stolen.thread && !own.thread)) return stolen.thread;
    if (own.thread) return own.thread;
  }
  return nullptr;
}

// Decides what runs on this core after an interrupt or a yield. `frame` is the
// state of the interrupted context. Lock held. Returns the frame to resume.
Frame* reschedule_locked(Frame* frame, Core* c, bool voluntary) {
  Thread* prev = c->current;
  c->resched = false;
  c->exit_reschedules = false;
  g.places[c->index].idle = prev->is_idle;   // (undoes ready_locked's claim if the thread it was woken for went elsewhere)
  if (!voluntary && prev->state == ThreadState::Running) {
    Thread* wanted = find_eligible(c);
    if (!wanted && eligible(prev, c)) return frame;          // nobody else wants the core
    // Keeps running until its slice ends, unless its core was taken away or something of a strictly higher priority is now ready (which does not wait out the slice).
    if (!prev->is_idle && c->slice > 0 && eligible(prev, c) && (!wanted || wanted->priority <= prev->priority)) return frame;
  }
  prev->saved = frame;
  // This core is still on prev's stack (below the saved frame: the return addresses of the handlers that called this) until the trap stub has loaded the next
  // frame. If prev were visible to another core before that, that core could resume it and its calls would overwrite those return addresses. So prev only becomes
  // runnable (and reapable: on_cpu) in finish_switch(), which the stub calls on the new thread's stack.
  if (prev->state == ThreadState::Running) prev->state = ThreadState::Ready;
  if (prev->state == ThreadState::Ready && !prev->is_idle) {      // (so that it competes for this core as before; taken out again below)
    prev->ready_tsc = hardware::read_tsc();
    prev->wake_core = -1;
    queue_on_locked(prev, c->index);
  }

  Thread* next = find_eligible(c);
  if (next) unqueue_locked(next);
  else next = c->idle;
  if (next != prev) {
    if (prev->state == ThreadState::Ready && !prev->is_idle) unqueue_locked(prev);      // (finish_switch queues it)
    c->leaving = prev;
  }
  next->state = ThreadState::Running;
  next->on_cpu = true;
  next->last_core = static_cast<int32_t>(c->index);
  next->wake_core = -1;
  c->current = next;
  g.places[c->index].idle = next->is_idle;
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
  return next->saved;
}

// Called by the interrupt stub (Interrupts.S, and the trap stubs of the other architectures) on the stack of the thread that now runs, after the old thread's stack is no longer in use by this core: the old thread
// leaves the core (on_cpu false, so it can be reaped or joined) and, when it is ready, joins the run queue where another core can take it. A thread that was made ready
// meanwhile (state Ready, on_cpu still true) was not queued by make_ready_locked, so it is queued here.
void finish_switch() {
  Core* c = this_core();
  if (!c->leaving) return;
  const uint64_t flags = lock_irq();
  Thread* prev = c->leaving;
  c->leaving = nullptr;
  prev->on_cpu = false;
  if (prev->state == ThreadState::Ready && !prev->is_idle) ready_locked(prev);   // (an idle core elsewhere may take it at once)
  unlock_irq(flags);
}

[[noreturn]] void park_core(Core* c) {
  c->parked = true;
  stop_tick();
  halt_forever();
}

#if defined(__x86_64__)
UserTickHook g_user_tick = nullptr;
#endif

Frame* on_timer(Frame* frame, void*) {
  ack_tick();
  __atomic_fetch_add(&this_core()->kernel_entries, 1, __ATOMIC_SEQ_CST);
#if defined(__x86_64__)
  if ((frame->cs & 3) == 3 && g_user_tick) g_user_tick(frame);
#endif
  Core* c = this_core();
  if (g.stopping && c->index != g.boot_core) park_core(c);
  const uint64_t flags = lock_irq();
  c->exit_reschedules = true;   // (reschedule_locked below)
  const uint64_t now = hardware::read_tsc();
  while (g.sleepers && static_cast<int64_t>(g.sleepers->wake_tsc - now) <= 0) {
    Thread* t = g.sleepers;
    g.sleepers = t->sleep_next;
    t->sleep_next = nullptr;
    t->in_sleepers = false;
    if (t->waiting_on) {
      list_remove(t->waiting_on, t);
      t->waiting_on = nullptr;
      t->timed_out = true;
    }
    make_ready_locked(t);
  }
  if (c->current->is_idle) {
    ++c->idle_ticks;
  } else {
    ++c->busy_ticks;
    ++c->current->cpu_ticks;
  }
  if (c->slice > 0) --c->slice;
  Frame* result = reschedule_locked(frame, c, false);
  unlock_irq(flags);
  return result;
}

Frame* on_yield(Frame* frame, void*) {
  Core* c = this_core();
  const uint64_t flags = lock_irq();
  Frame* result = reschedule_locked(frame, c, true);
  unlock_irq(flags);
  return result;
}

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
  const uint64_t flags = lock_irq();
  Frame* result = reschedule_locked(frame, c, false);
  unlock_irq(flags);
  return result;
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

// Runs the driver's handler with this core marked as handling `device`, so that the threads it wakes learn the core (make_ready_locked). If one of them
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
  const uint64_t flags = lock_irq();
  result = reschedule_locked(frame, c, false);
  unlock_irq(flags);
  return result;
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

uint32_t reap_zombies_impl() {
  if (!g.zombies) return 0;
  Thread* done = nullptr;
  const uint64_t flags = lock_irq();
  Thread** link = &g.zombies;
  while (*link) {
    Thread* t = *link;
    if (t->on_cpu) {
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
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  g.places[c->index] = place;
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  const Core& c = g.cores[core];
  out->online = c.online;
  out->kind = c.kind;
  out->busy_ticks = c.busy_ticks;
  out->idle_ticks = c.idle_ticks;
  unlock_irq(flags);
  return true;
}

uint32_t thread_snapshot(ThreadInfo* out, uint32_t capacity) {
  if (!g.running) return 0;
  uint32_t n = 0;
  const uint64_t flags = lock_irq();
  for (const Thread* t = g.all; t && n < capacity; t = t->all_next) {
    if (t->state == ThreadState::Zombie) continue;
    ThreadInfo& info = out[n++];
    info.id = t->id;
    uint32_t i = 0;
    for (; i + 1 < sizeof info.name && t->name && t->name[i]; ++i) info.name[i] = t->name[i];
    info.name[i] = 0;
    info.state = t->state;
    info.idle = t->is_idle;
    info.core = -1;
    for (uint32_t c = 0; c < g.core_count; ++c)
      if (g.cores[c].current == t) info.core = static_cast<int32_t>(c);
    info.affinity = t->affinity;
    info.io_core = io_core_of(t);
    info.io_device = t->io_device;
    info.cpu_ticks = t->cpu_ticks;
  }
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  if (t->user_cr3) {      // (a thread that has not started in user mode yet takes the tables it finds when it does: see set_current_user_context)
    t->user_cr3 = cr3;
    for (uint32_t i = 0; i < g.core_count; ++i) {
      if (g.cores[i].current != t) continue;
      g.cores[i].user.user_cr3 = cr3;      // (what that core returns to ring 3 with: a store of one word, taken under the lock that every switch takes)
      running_on = static_cast<int32_t>(i);
    }
  }
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();                                    // (this core stays this core while it is read and while the function runs)
  Core* self = this_core();
  const uint64_t others = online_core_mask() & ~(self->index < 64 ? uint64_t{1} << self->index : 0);
  g_core_work.fn = fn;
  g_core_work.argument = argument;
  __atomic_store_n(&g_core_work.pending, others, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < g.core_count && i < 64; ++i)
    if (others & (uint64_t{1} << i)) send_wakeup(g.cores[i].apic_id);
  fn(argument);
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  t->core_mask = mask;
  unlock_irq(flags);
  return true;
}

void set_thread_affinity(Thread* t, int32_t core) {
  const uint64_t flags = lock_irq();
  t->affinity = core;
  unlock_irq(flags);
}

void set_thread_priority(Thread* t, uint8_t priority) {
  if (priority >= kPriorityLevels) priority = kPriorityLevels - 1;
  const uint64_t flags = lock_irq();
  t->priority = priority;
  unlock_irq(flags);
}

Thread* current_thread() { return g.running ? this_core()->current : nullptr; }

int32_t register_io_device(const char* name, int32_t interrupt_core) { return g_devices.add(StringView(name ? name : ""), interrupt_core); }

void unregister_io_device(int32_t device) {
  if (!g_devices.live(device)) return;
  set_io_device_retarget(device, nullptr, nullptr);
  {
    const uint64_t flags = lock_irq();   // (threads bound to it go back to learning: the slot may soon be another device's)
    for (Thread* t = g.all; t; t = t->all_next)
      if (t->io_device == device) t->io_device = -1;
    unlock_irq(flags);
  }
  g_devices.remove(device);
}

bool set_io_device_core(int32_t device, int32_t core) { return g_devices.set_interrupt_core(device, core); }

bool io_device_info(int32_t device, scheduling::IoDeviceInfo* out) { return out && g_devices.info(device, out); }

uint32_t choose_io_interrupt_core(int8_t kind) {
  if (!g.running) return 0;
  uint32_t per_core[kMaxCores];
  g_devices.devices_per_core(ArrayView<uint32_t>(per_core, g.core_count));
  const uint64_t flags = lock_irq();
  const int32_t core = scheduling::choose_interrupt_core(places(), ArrayView<const uint32_t>(per_core, g.core_count), scheduling::CoreSet::all(), kind,
                                                         static_cast<int32_t>(g.boot_core));
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  g.places[core].numa_node = node;
  unlock_irq(flags);
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
    uint64_t flags = lock_irq();
    int32_t device = -1;
    for (uint32_t i = 0; i < scheduling::IoDeviceTable::kCapacity && device < 0; ++i)
      if (g.steering[i].move_to >= 0 && g.steering[i].retarget) device = static_cast<int32_t>(i);
    if (device < 0) {
      detail::block_current_locked(&g.balancer_wait, 0);
      unlock_irq(flags);
      yield();
      continue;
    }
    Scheduler::Steering& steering = g.steering[device];
    const uint32_t to = static_cast<uint32_t>(steering.move_to);
    const IoRetarget retarget = steering.retarget;
    void* context = steering.context;
    const uint64_t hardware_id = g.cores[to].apic_id;
    steering.move_to = -1;
    __atomic_store_n(&g.balancer_busy, device, __ATOMIC_RELEASE);
    unlock_irq(flags);
    if (retarget(context, to, hardware_id)) g_devices.set_interrupt_core(device, static_cast<int32_t>(to));
    __atomic_store_n(&g.balancer_busy, -1, __ATOMIC_RELEASE);
  }
}
}  // namespace

bool set_io_device_retarget(int32_t device, IoRetarget retarget, void* context) {
  if (!g_devices.live(device) || static_cast<uint32_t>(device) >= scheduling::IoDeviceTable::kCapacity) return false;
  uint64_t flags = lock_irq();
  Scheduler::Steering& steering = g.steering[device];
  steering.retarget = retarget;
  steering.context = context;
  steering.tally = scheduling::InterruptSteering();
  steering.move_to = -1;
  unlock_irq(flags);
  if (!retarget) {   // (the driver may free the context once this returns)
    while (__atomic_load_n(&g.balancer_busy, __ATOMIC_ACQUIRE) == device) yield();
    return true;
  }
  if (g.running && !__atomic_exchange_n(&g.balancer_started, true, __ATOMIC_ACQ_REL)) {   // (the first driver to ask starts it)
    Thread* balancer = spawn(io_balancer_main, nullptr, "io-balancer", -1, -1, 64 * 1024);
    if (balancer) {
      detach(balancer);
      flags = lock_irq();
      g.balancer = balancer;
      unlock_irq(flags);
    } else {
      __atomic_store_n(&g.balancer_started, false, __ATOMIC_RELEASE);
    }
  }
  return true;
}

void bind_thread_to_io_device(Thread* t, int32_t device) {
  if (!t) return;
  const uint64_t flags = lock_irq();
  t->io_device = device;
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  t->id = g.next_id++;
  enqueue_locked(t);
  unlock_irq(flags);
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
  const uint64_t flags = lock_irq();
  detail::block_current_locked(nullptr, us);
  unlock_irq(flags);
  yield();
}

void exit_thread() {
  Thread* me = current_thread();
  const uint64_t flags = lock_irq();
  me->state = ThreadState::Zombie;
  detail::wake_all_locked(&me->joiners);
  if (me->detached) {
    me->next = g.zombies;
    g.zombies = me;
  }
  unlock_irq(flags);
  for (;;) yield();
}

bool join(Thread* t, uint64_t timeout_us) {
  if (!t) return false;
  // Once the scheduler has been stopped no thread runs any more, so none can finish: waiting for one would never end.
  if (!g.running) return false;
  const uint64_t deadline = timeout_us ? hardware::read_tsc() + timeout_us * g.tsc_per_us : 0;
  for (;;) {
    const uint64_t flags = lock_irq();
    if (t->state == ThreadState::Zombie) {
      unlock_irq(flags);
      break;
    }
    uint64_t remaining = 0;
    if (deadline) {
      const int64_t left = static_cast<int64_t>(deadline - hardware::read_tsc());
      if (left <= 0) {
        unlock_irq(flags);
        return false;
      }
      remaining = static_cast<uint64_t>(left) / g.tsc_per_us + 1;
    }
    detail::block_current_locked(&t->joiners, remaining);
    unlock_irq(flags);
    yield();
  }
  while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) yield();  // its core is still leaving it
  {
    const uint64_t flags = lock_irq();
    unregister_locked(t);
    unlock_irq(flags);
  }
  g.allocator->deallocate(t->stack);
  g.allocator->deallocate(t);
  return true;
}

void detach(Thread* t) {
  if (!t) return;
  const uint64_t flags = lock_irq();
  t->detached = true;
  if (t->state == ThreadState::Zombie) {
    t->next = g.zombies;
    g.zombies = t;
  }
  unlock_irq(flags);
}

namespace detail {

uint64_t lock() { return lock_irq(); }
void unlock(uint64_t flags) { unlock_irq(flags); }

void block_current_locked(WaitList* list, uint64_t timeout_us) {
  Thread* t = this_core()->current;
  t->state = ThreadState::Blocked;
  t->timed_out = false;
  t->waiting_on = list;
  if (list) list_append(list, t);
  if (timeout_us) {
    t->wake_tsc = hardware::read_tsc() + timeout_us * g.tsc_per_us;
    sleepers_insert(t);
  }
}

bool wake_one_locked(WaitList* list) { return wake_one_locked_impl(list); }

uint32_t wake_all_locked(WaitList* list) {
  uint32_t n = 0;
  while (wake_one_locked(list)) ++n;
  return n;
}

}  // namespace detail

}  // namespace kernel
