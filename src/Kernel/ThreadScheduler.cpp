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
#include "Kernel/Apic.hpp"
#include "Kernel/Paging.hpp"
#elif defined(__aarch64__)
#include "Arch/AArch64/Gic.hpp"
#include "Arch/AArch64/Timer.hpp"
#elif defined(__riscv) && __riscv_xlen == 64
#include "Arch/RiscV64/Sbi.hpp"
#include "Arch/RiscV64/Timer.hpp"
#endif
#include "Kernel/Thread.hpp"

namespace kernel {

namespace {

constexpr uint32_t kMaxCores = 128;
constexpr uint32_t kSliceTicks = 10;

struct Core {
  Core* self = nullptr;  // first field: read through the GS base
  uint32_t index = 0;
  uint64_t apic_id = 0;   // what addresses the core in an inter-processor interrupt (APIC id; MPIDR affinity on ARM)
  bool online = false;
  bool parked = false;
  hardware::CoreKind kind = hardware::CoreKind::Performance;
  uint32_t slice = kSliceTicks;
  Thread* current = nullptr;
  Thread* idle = nullptr;
  uint64_t busy_ticks = 0, idle_ticks = 0;
};

struct ApStart {
  uint32_t core;
};

struct Scheduler {
  uint32_t lock_word = 0;
  Core cores[kMaxCores];
  ApStart ap_start[kMaxCores];
  uint32_t core_count = 1;
  uint32_t boot_core = 0;
  Thread* queue_head = nullptr;
  Thread* queue_tail = nullptr;
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

// ---- the few places where the two architectures differ ----
#if defined(__x86_64__)
alignas(16) uint8_t g_fpu_template[512];

constexpr uint32_t kMsrGsBase = 0xC0000101;

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
void idle_wait() { __asm__ volatile("sti; hlt"); }
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

WaitList* run_queue() {
  static_assert(sizeof(WaitList) == 2 * sizeof(Thread*));
  return reinterpret_cast<WaitList*>(&g.queue_head);  // queue_head, queue_tail are adjacent
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

// A core running only its idle thread can be told that work arrived.
void poke_idle_core(const Thread* t) {
  const Core* self = g.running ? this_core() : nullptr;
  for (uint32_t i = 0; i < g.core_count; ++i) {
    const Core& c = g.cores[i];
    if (!c.online || &c == self || !c.current || !c.current->is_idle) continue;
    if (t->affinity >= 0 && static_cast<uint32_t>(t->affinity) != i) continue;
    send_wakeup(c.apic_id);
    return;
  }
}

void enqueue_locked(Thread* t) {
  t->state = ThreadState::Ready;
  list_append(run_queue(), t);
  poke_idle_core(t);
}

void make_ready_locked(Thread* t) {
  if (t->state == ThreadState::Zombie || t->state == ThreadState::Ready) return;
  t->state = ThreadState::Ready;
  if (!t->on_cpu) {  // otherwise its core queues it as soon as it has saved the registers
    list_append(run_queue(), t);
    poke_idle_core(t);
  }
}

bool eligible(const Thread* t, const Core* c, bool match_kind) {
  if (t->affinity >= 0 && static_cast<uint32_t>(t->affinity) != c->index) return false;
  if (match_kind && t->preferred_kind >= 0 && t->preferred_kind != static_cast<int8_t>(c->kind)) return false;
  return true;
}

Thread* find_eligible(const Core* c) {
  for (Thread* t = g.queue_head; t; t = t->next)
    if (eligible(t, c, true)) return t;
  for (Thread* t = g.queue_head; t; t = t->next)
    if (eligible(t, c, false)) return t;
  return nullptr;
}

// Decides what runs on this core after an interrupt or a yield. `frame` is the
// state of the interrupted context. Lock held. Returns the frame to resume.
Frame* reschedule_locked(Frame* frame, Core* c, bool voluntary) {
  Thread* prev = c->current;
  if (!voluntary && prev->state == ThreadState::Running) {
    Thread* wanted = find_eligible(c);
    if (!wanted) return frame;                                      // nobody else wants the core
    if (!prev->is_idle && c->slice > 0) return frame;               // keeps running until its slice ends
  }
  prev->saved = frame;
  prev->on_cpu = false;
  if (prev->state == ThreadState::Running) prev->state = ThreadState::Ready;
  if (prev->state == ThreadState::Ready && !prev->is_idle) list_append(run_queue(), prev);

  Thread* next = find_eligible(c);
  if (next) list_remove(run_queue(), next);
  else next = c->idle;
  next->state = ThreadState::Running;
  next->on_cpu = true;
  c->current = next;
  c->slice = kSliceTicks;
  return next->saved;
}

[[noreturn]] void park_core(Core* c) {
  c->parked = true;
  stop_tick();
  halt_forever();
}

Frame* on_timer(Frame* frame, void*) {
  ack_tick();
  Core* c = this_core();
  if (g.stopping && c->index != g.boot_core) park_core(c);
  const uint64_t flags = lock_irq();
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

Frame* on_wakeup(Frame* frame, void*) {
  ack_wakeup();
  Core* c = this_core();
  if (g.stopping && c->index != g.boot_core) park_core(c);
  const uint64_t flags = lock_irq();
  Frame* result = reschedule_locked(frame, c, false);
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
#endif
  t->saved = f;
}

Thread* make_thread(ThreadFunction fn, void* argument, const char* name, int32_t core, int8_t kind, uint32_t stack_bytes) {
  void* memory = g.allocator->allocate(sizeof(Thread));
  if (!memory) return nullptr;
  Thread* t = new (memory) Thread();
  t->stack = static_cast<uint8_t*>(g.allocator->allocate_aligned(stack_bytes, 16));
  if (!t->stack) {
    g.allocator->deallocate(t);
    return nullptr;
  }
  t->entry = fn;
  t->argument = argument;
  t->name = name;
  t->affinity = core;
  t->preferred_kind = kind;
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
  c->online = true;
  __atomic_add_fetch(&g.aps_ready, 1, __ATOMIC_SEQ_CST);
  start_tick(g.tick_hz);
  for (;;) idle_wait();
}
#endif

}  // namespace

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
    info.cpu_ticks = t->cpu_ticks;
  }
  unlock_irq(flags);
  return n;
}

Thread* current_thread() { return g.running ? this_core()->current : nullptr; }

bool start(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator, uint32_t tick_hz) {
  if (g.running || !hardware::tsc_ticks_per_us()) return false;
  g.cpu = &cpu;
  g.allocator = &allocator;
  g.tsc_per_us = hardware::tsc_ticks_per_us();
  g.tick_hz = tick_hz;
  g.boot_core = cpu.current_core();
  g.core_count = cpu.core_count() < kMaxCores ? cpu.core_count() : kMaxCores;
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
#if defined(__x86_64__) || defined(__riscv)
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
  Thread* idle = make_thread(idle_loop, nullptr, "idle", static_cast<int32_t>(g.boot_core), -1, 16 * 1024);
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
              uint32_t stack_bytes) {
  if (!g.running || !fn) return nullptr;
  reap_zombies_impl();
  Thread* t = make_thread(fn, argument, name, core, preferred_kind, stack_bytes);
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

bool wake_one_locked(WaitList* list) {
  Thread* t = list->head;
  if (!t) return false;
  list_remove(list, t);
  t->waiting_on = nullptr;
  if (t->in_sleepers) sleepers_remove(t);
  make_ready_locked(t);
  return true;
}

uint32_t wake_all_locked(WaitList* list) {
  uint32_t n = 0;
  while (wake_one_locked(list)) ++n;
  return n;
}

}  // namespace detail

}  // namespace kernel
