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
#include "Kernel/UserMode.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Kernel/Delay.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsException.hpp"
#include "console.hpp"

#if defined(__x86_64__)

extern "C" {
extern uint8_t tcos_tramp_begin[], tcos_tramp_syscall[], tcos_tramp_exit[], tcos_tramp_isr_slot[], tcos_tramp_end[];
void tcos_isr_common();
[[noreturn]] void tcos_resume_frame(kernel::Frame* frame);
}

namespace kernel {
namespace usermode {

namespace {

constexpr uint32_t kMsrEfer = 0xC0000080, kMsrStar = 0xC0000081, kMsrLstar = 0xC0000082, kMsrFmask = 0xC0000084, kMsrKernelGsBase = 0xC0000102;
constexpr uint64_t kEferSce = 1, kEferNxe = 1u << 11;
constexpr uint64_t kFlagsTrap = 0x100, kFlagsInterrupt = 0x200, kFlagsDirection = 0x400, kFlagsIopl = 0x3000, kFlagsNested = 0x4000, kFlagsAlign = 0x40000;
constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kMaxRanges = 3 + 128;      // trampoline code, interrupt table, the data of up to 128 cores... see Core in ThreadScheduler.cpp
constexpr uint64_t kStubSize = 16;

struct __attribute__((packed)) TablePointer {
  uint16_t limit;
  uint64_t base;
};
struct Gate {
  uint16_t offset_low, selector;
  uint8_t ist, type_attributes;
  uint16_t offset_middle;
  uint32_t offset_high, reserved;
};
static_assert(sizeof(Gate) == 16);

// The one lock and wake-up of the waiting for threads and for the synchronization objects that processes share.
class SystemSync final : public process::ISystemSync {
 public:
  void lock() override { mutex_.lock(); }
  void unlock() override { mutex_.unlock(); }
  bool wait(uint64_t timeout_ns) override {
    uint64_t us = 0;
    if (timeout_ns != ~uint64_t{0}) us = timeout_ns / 1000 + 1;
    return changed_.wait(mutex_, us);
  }
  void notify_all() override { changed_.notify_all(); }

 private:
  Mutex mutex_;
  ConditionVariable changed_;
};

struct State {
  allocators::IAllocator* allocator = nullptr;
  SystemSync* sync = nullptr;      // (made once and kept: the state has no destructor to run at exit)
  process::ObjectManager* objects = nullptr;
  uint8_t* code = nullptr;          // the copy of the entry stubs
  uint64_t code_bytes = 0;
  Gate* idt = nullptr;
  bool nx = false;
  bool ready = false;
  process::AddressRange ranges[kMaxRanges];
  uint32_t range_count = 0;
  uint32_t cores_loaded = 0;
};
State g;

uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return (uint64_t{high} << 32) | low;
}
void wrmsr(uint32_t msr, uint64_t value) { __asm__ volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32))); }
uint64_t read_cr3() {
  uint64_t v;
  __asm__ volatile("movq %%cr3, %0" : "=r"(v));
  return v;
}
uint64_t read_cr2() {
  uint64_t v;
  __asm__ volatile("movq %%cr2, %0" : "=r"(v));
  return v;
}
uint64_t address_of(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint64_t address_of(void (*function)()) { return reinterpret_cast<uint64_t>(function); }

// Where the copy of a symbol in the trampoline lives.
uint64_t copy_of(const uint8_t* symbol) { return address_of(g.code) + (address_of(symbol) - address_of(tcos_tramp_begin)); }

uint64_t tss_descriptor_low(uint64_t base, uint32_t limit) {
  return (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (uint64_t{0x89} << 40) | (uint64_t{(limit >> 16) & 0xF} << 48) | ((base >> 24 & 0xFF) << 56);
}

// Loads this core's descriptor tables, the SYSCALL registers and the kernel's side of the entry area. Runs on the core it sets up.
void load_core(void* argument) {
  const uint32_t core = static_cast<uint32_t>(address_of(argument));
  UserEntry* entry = core_user_entry(core);
  entry->kernel_cr3 = read_cr3();
  entry->entry_stack_top = core_entry_stack_top(core);
  entry->tramp_exit = copy_of(tcos_tramp_exit);
  entry->gdt[0] = 0;
  entry->gdt[1] = 0x00AF9A000000FFFFull;      // kernel code, 64-bit
  entry->gdt[2] = 0x00CF92000000FFFFull;      // kernel data
  entry->gdt[3] = 0x00CFF2000000FFFFull;      // user data (ring 3)
  entry->gdt[4] = 0x00AFFA000000FFFFull;      // user code, 64-bit (ring 3)
  const uint64_t tss = address_of(entry->tss);
  entry->gdt[5] = tss_descriptor_low(tss, 103);
  entry->gdt[6] = tss >> 32;
  entry->tss[1] = static_cast<uint32_t>(entry->entry_stack_top);      // RSP0: where an interrupt from ring 3 lands
  entry->tss[2] = static_cast<uint32_t>(entry->entry_stack_top >> 32);
  entry->tss[25] = 104u << 16;                                          // I/O map base: past the end of the TSS, so no port is open to user mode
  const TablePointer gdt = {sizeof(entry->gdt) - 1, address_of(entry->gdt)};
  const TablePointer idt = {static_cast<uint16_t>(256 * sizeof(Gate) - 1), address_of(g.idt)};
  const uint64_t flags = irq_save();
  __asm__ volatile("lgdt %0" ::"m"(gdt) : "memory");
  __asm__ volatile("ltr %0" ::"r"(kSelectorTss) : "memory");
  __asm__ volatile("lidt %0" ::"m"(idt) : "memory");
  // Global pages would survive the switch of page tables and could shadow a user address.
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  if (cr4 & (1u << 7)) __asm__ volatile("movq %0, %%cr4" ::"r"(cr4 & ~uint64_t{1u << 7}) : "memory");
  // Interrupts save the x87/SSE state only. With XSAVE on, a program could use AVX, whose upper halves would leak between programs, so user mode is told
  // there is none (CPUID then reports OSXSAVE clear).
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  if (cr4 & (1u << 18)) __asm__ volatile("movq %0, %%cr4" ::"r"(cr4 & ~uint64_t{1u << 18}) : "memory");
  wrmsr(kMsrEfer, rdmsr(kMsrEfer) | kEferSce);
  wrmsr(kMsrStar, (uint64_t{0x10} << 48) | (uint64_t{kSelectorCode} << 32));      // SYSCALL loads CS 0x08 / SS 0x10 (the SYSRET half is unused: we return with IRET)
  wrmsr(kMsrLstar, copy_of(tcos_tramp_syscall));
  wrmsr(kMsrFmask, kFlagsTrap | kFlagsInterrupt | kFlagsDirection | kFlagsIopl | kFlagsNested | kFlagsAlign);
  wrmsr(kMsrKernelGsBase, 0);
  irq_restore(flags);
  __atomic_add_fetch(&g.cores_loaded, 1, __ATOMIC_SEQ_CST);
}

// The registers of an interrupted user context as a CONTEXT-style record, and back.
void frame_to_context(const Frame& frame, process::X86Context* c) {
  const uint64_t* in[16] = {&frame.rax, &frame.rcx, &frame.rdx, &frame.rbx, &frame.rsp, &frame.rbp, &frame.rsi, &frame.rdi,
                            &frame.r8, &frame.r9, &frame.r10, &frame.r11, &frame.r12, &frame.r13, &frame.r14, &frame.r15};
  for (uint32_t i = 0; i < 16; ++i) c->gpr[i] = *in[i];
  c->rip = frame.rip;
  c->rflags = frame.rflags;
  for (uint32_t i = 0; i < 512; ++i) c->fpu[i] = frame.fpu[i];
}
void context_to_frame(const process::X86Context& c, Frame* frame) {
  uint64_t* out[16] = {&frame->rax, &frame->rcx, &frame->rdx, &frame->rbx, &frame->rsp, &frame->rbp, &frame->rsi, &frame->rdi,
                       &frame->r8, &frame->r9, &frame->r10, &frame->r11, &frame->r12, &frame->r13, &frame->r14, &frame->r15};
  for (uint32_t i = 0; i < 16; ++i) *out[i] = c.gpr[i];
  frame->rip = c.rip;
  frame->rflags = c.rflags;
  for (uint32_t i = 0; i < 512; ++i) frame->fpu[i] = c.fpu[i];
}

// The thread is about to end: whoever waits for the program may now join its CPU thread.
void thread_ends_now(UserThread* t) {
  __atomic_store_n(&t->finished, true, __ATOMIC_RELEASE);
  t->task->changed.notify_all();
}

UserThread* current_user_thread() {
  Thread* t = current_thread();
  return t ? static_cast<UserThread*>(t->user_context) : nullptr;
}
Task* current_task() {
  UserThread* u = current_user_thread();
  return u ? u->task : nullptr;
}

bool kill_pending(const Task* task) { return __atomic_load_n(&task->kill_requested, __ATOMIC_ACQUIRE); }

// Timer tick in user mode: a program that was told to stop gets its instruction pointer moved to page zero, which is never mapped. Its next step is a page
// fault, which on_user_fault() recognises as the end the program was asked for.
void on_user_tick(Frame* frame) {
  Task* task = current_task();
  if (task && (kill_pending(task) || task->program.process()->exited())) frame->rip = 0;
}

// SYSCALL from user mode (the stub made it look like an interrupt): the registers are in the frame.
Frame* on_syscall(Frame* frame, void*) {
  UserThread* self = current_user_thread();
  if (!self) panic("system call from a thread that is not a user program", frame);
  Task* task = self->task;
  process::Process* process = task->program.process();
  irq_enable();      // the frame is saved: the call may take as long as it needs and be preempted
  if (process->exited()) { thread_ends_now(self); exit_thread(); }      // another thread ended the program
  if (kill_pending(task)) {
    task->killed = true;
    process->terminate(130);
    thread_ends_now(self);
    exit_thread();
  }
  task->mutex.lock();      // the process's lock: the call, and what follows it, change the process's tables
  process::X86UserRegisters regs;
  regs.rax = frame->rax; regs.rcx = frame->rcx; regs.rdx = frame->rdx; regs.rbx = frame->rbx; regs.rsp = frame->rsp; regs.rbp = frame->rbp;
  regs.rsi = frame->rsi; regs.rdi = frame->rdi; regs.r8 = frame->r8; regs.r9 = frame->r9; regs.r10 = frame->r10; regs.r11 = frame->r11;
  process::SyscallRequest request;
  if (process::decode_x86_64(process->abi(), regs, &request)) frame->rax = process->syscall(request);
  else frame->rax = ~uint64_t{37};      // -ENOSYS
  uint64_t fs = 0, gs = 0;
  const bool fs_changed = process->take_fs_change(&fs), gs_changed = process->take_gs_change(&gs);
  if (fs_changed || gs_changed) set_current_user_bases(fs_changed, fs, gs_changed, gs);      // arch_prctl
  bool alive = !process->exited();
  uint32_t thread_code = 0;
  const bool thread_ends = process->take_thread_exit(&thread_code);
  uint64_t context_at = 0;
  if (alive && !thread_ends && process->take_context_restore(&context_at)) {      // NtContinue
    process::X86Context context;
    frame_to_context(*frame, &context);
    if (process::apply_context(*process, context_at, &context)) {
      context_to_frame(context, frame);
    } else {
      process->terminate(process::kStatusAccessViolation);
      alive = false;
    }
  }
  if (alive && kill_pending(task)) {      // asked to stop while the call ran (or in it)
    task->killed = true;
    process->terminate(130);
    alive = false;
  }
  // The call may have changed the memory layout: rebuild the page tables and hand the new ones to this thread's siblings and core.
  if (alive || thread_ends) {
    if (!task->sync_tables()) {
      console::puts("user program: out of memory for page tables\n");
      process->terminate(0xFF);
      alive = false;
    }
  }
  if (!alive || thread_ends) thread_ends_now(self);
  task->mutex.unlock();
  if (!alive || thread_ends) exit_thread();
  irq_disable();      // the way back is not re-entrant
  return frame;
}

// An exception in user mode ends that program only.
Frame* on_user_fault(Frame* frame) {
  Task* task = current_task();
  if (!task) panic("fault in user mode on a thread that is not a user program", frame);
  if ((kill_pending(task) || task->program.process()->exited()) && (frame->vector & 0xFF) == 14 && frame->rip == 0) {      // the end on_user_tick() arranged: not a fault of the program's
    if (!task->program.process()->exited()) task->killed = true;
    task->program.process()->terminate(130);      // (a program that already ended keeps its exit code)
    thread_ends_now(current_user_thread());
    irq_enable();
    exit_thread();
  }
  // A Windows program gets its exception delivered, to be handled by SEH or C++ code (or to end up in ntdll's last resort); only when that cannot be done is the program stopped.
  process::Process* process = task->program.process();
  if (process->abi() == process::SyscallAbi::Windows && process->exception_dispatcher() != 0) {
    process::X86Context at, entry;
    frame_to_context(*frame, &at);
    process::ExceptionInfo info;
    const uint64_t vector = frame->vector & 0xFF;
    const uint64_t address = vector == 14 ? read_cr2() : 0;
    bool delivered = false;
    if (process::fault_to_exception(vector, frame->error, address, &at, &info)) {
      irq_enable();      // (the frame is saved) the program's lock may have to be waited for: writing on its stack looks at its address space
      task->mutex.lock();
      delivered = process::dispatch_exception(*process, at, info, process->exception_dispatcher(), &entry);
      task->mutex.unlock();
      irq_disable();
    }
    if (delivered) {
      context_to_frame(entry, frame);
      return frame;
    }
  }
  task->faulted = true;
  task->fault_vector = frame->vector & 0xFF;
  task->fault_rip = frame->rip;
  task->fault_address = (frame->vector & 0xFF) == 14 ? read_cr2() : 0;
  console::puts("user program stopped: vector ");
  console::put_hex(task->fault_vector);
  console::puts(" error ");
  console::put_hex(frame->error);
  console::puts(" rip ");
  console::put_hex(frame->rip);
  console::puts(" address ");
  console::put_hex(task->fault_address);
  console::puts("\n");
  task->program.process()->terminate(128 + static_cast<uint32_t>(task->fault_vector));
  thread_ends_now(current_user_thread());
  irq_enable();      // the frame is saved; ending the thread has to be able to switch away
  exit_thread();
  return frame;
}

// A thread that becomes a thread of the program: sets up its context and "returns" into ring 3 through a frame of the kind an interrupt leaves behind.
void user_main(void* argument) {
  UserThread* ut = static_cast<UserThread*>(argument);
  Task* task = ut->task;
  const process::ThreadStart& start = ut->start;
  Thread* me = current_thread();
  me->user_fs = start.fs_base;
  me->user_gs = start.gs_base;
  task->mutex.lock();      // (the tables may be rebuilt by another thread right now)
  set_current_user_context(task->tables.root(), ut);
  task->mutex.unlock();
  Frame frame;      // on this stack, which stays untouched until the CPU is in ring 3 (the next entry starts at the top of the stack)
  __asm__ volatile("fxsave %0" : "=m"(frame.fpu));
  frame.r15 = frame.r14 = frame.r13 = frame.r12 = frame.r11 = frame.r10 = frame.r9 = frame.r8 = 0;
  frame.rdi = frame.rsi = frame.rbp = frame.rbx = frame.rax = 0;
  frame.rcx = start.argument;
  frame.rdx = start.argument2;
  frame.vector = frame.error = 0;
  frame.rip = start.entry;
  frame.cs = kSelectorUserCode;
  frame.rflags = kFlagsInterrupt | 2;
  frame.rsp = start.stack_pointer;
  frame.ss = kSelectorUserData;
  irq_disable();
  tcos_resume_frame(&frame);
}

}  // namespace

bool ready() { return g.ready; }

ArrayView<const process::AddressRange> reserved_ranges() { return ArrayView<const process::AddressRange>(g.ranges, g.range_count); }

bool init(allocators::IAllocator& allocator) {
  if (g.ready) return true;
  if (!running()) return false;
  uint32_t eax = 0x80000001, ebx, ecx = 0, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
  if (!(edx & (1u << 11))) return false;      // no SYSCALL
  g.allocator = &allocator;
  if (!g.sync) {
    void* sync_memory = allocator.allocate(sizeof(SystemSync));
    void* objects_memory = sync_memory ? allocator.allocate(sizeof(process::ObjectManager)) : nullptr;
    if (!objects_memory) { if (sync_memory) allocator.deallocate(sync_memory); return false; }
    g.sync = new (sync_memory) SystemSync();
    g.objects = new (objects_memory) process::ObjectManager(*g.sync);
    process::set_system_objects(g.objects);
  }
  g.nx = (rdmsr(kMsrEfer) & kEferNxe) != 0;

  const uint64_t used = address_of(tcos_tramp_end) - address_of(tcos_tramp_begin);
  if (address_of(tcos_tramp_syscall) - address_of(tcos_tramp_begin) < 256 * kStubSize || used > 2 * kPageSize) return false;
  g.code_bytes = 2 * kPageSize;
  g.code = static_cast<uint8_t*>(allocator.allocate_aligned(g.code_bytes, kPageSize));
  g.idt = static_cast<Gate*>(allocator.allocate_aligned(256 * sizeof(Gate), kPageSize));
  if (!g.code || !g.idt) return false;
  ByteView(g.code, g.code_bytes).fill(0);
  ByteView(g.code, used).copy_from(ConstByteView(tcos_tramp_begin, used));
  const uint64_t isr = address_of(&tcos_isr_common);
  for (uint32_t i = 0; i < 8; ++i) g.code[address_of(tcos_tramp_isr_slot) - address_of(tcos_tramp_begin) + i] = static_cast<uint8_t>(isr >> (8 * i));
  for (uint32_t v = 0; v < 256; ++v) {
    const uint64_t stub = address_of(g.code) + v * kStubSize;
    Gate& gate = g.idt[v];
    gate.offset_low = static_cast<uint16_t>(stub);
    gate.selector = kSelectorCode;
    gate.ist = 0;
    gate.type_attributes = v == 3 ? 0xEE : 0x8E;      // present, interrupt gate; ring 0 only except INT3, which a program may execute (a breakpoint)
    gate.offset_middle = static_cast<uint16_t>(stub >> 16);
    gate.offset_high = static_cast<uint32_t>(stub >> 32);
    gate.reserved = 0;
  }
  g.ranges[g.range_count++] = {address_of(g.code), g.code_bytes};
  g.ranges[g.range_count++] = {address_of(g.idt), kPageSize};
  set_interrupt_handler(kVectorSyscall, on_syscall, nullptr);
  set_user_fault_handler(on_user_fault);
  set_user_tick_hook(on_user_tick);

  // Every online core loads its own tables, from a thread pinned to it.
  uint32_t expected = 0;
  Thread* loaders[128] = {};
  for (uint32_t core = 0; core < core_count() && core < 128; ++core) {
    CoreInfo info;
    if (!core_info(core, &info) || !info.online) continue;
    if (g.range_count >= kMaxRanges) return false;
    g.ranges[g.range_count++] = {core_page(core), kPageSize};
    loaders[core] = kernel::spawn(load_core, reinterpret_cast<void*>(static_cast<uint64_t>(core)), "usermode-init", static_cast<int32_t>(core));
    if (!loaders[core]) return false;
    ++expected;
  }
  for (Thread* loader : loaders) if (loader) join(loader);
  g.ready = g.cores_loaded == expected && expected != 0;
  return g.ready;
}

namespace {
// The running programs. A task joins the list when it has started and leaves it in its destructor, so a snapshot (which holds the lock) never sees a destroyed one.
Mutex g_tasks_mutex;
Task* g_tasks = nullptr;
}  // namespace

void Task::set_name(StringView text) {
  uint32_t i = 0;
  for (; i + 1 < sizeof name && i < text.size(); ++i) name[i] = text[i];
  name[i] = 0;
}

Task::~Task() {
  if (registered) {
    ScopedMutex hold(g_tasks_mutex);
    for (Task** link = &g_tasks; *link; link = &(*link)->all_next)
      if (*link == this) {
        *link = all_next;
        break;
      }
  }
  while (threads) {
    UserThread* next = threads->next;
    threads->~UserThread();
    allocator_.deallocate(threads);
    threads = next;
  }
}

bool Task::wait(uint64_t timeout_ns) {
  const uint64_t us = timeout_ns == ~uint64_t{0} ? 0 : (timeout_ns + 999) / 1000;
  return changed.wait(mutex, us == 0 && timeout_ns != ~uint64_t{0} ? 1 : us);
}

bool Task::interrupted() { return kill_pending(this) || program.process()->exited(); }

uint64_t Task::current_thread_id() {
  UserThread* u = current_user_thread();
  return u ? u->id : 0;
}

UserThread* Task::add_thread(const process::ThreadStart& start) {
  void* memory = allocator_.allocate(sizeof(UserThread));
  if (!memory) return nullptr;
  UserThread* t = new (memory) UserThread();
  t->task = this;
  t->id = start.id;
  t->start = start;
  t->next = threads;
  threads = t;
  return t;
}

constexpr uint32_t kUserThreadKernelStack = 256 * 1024;

bool Task::launch(UserThread* t, uint32_t stack_bytes) {
  t->thread = kernel::spawn(user_main, t, "user-thread", home_core, -1, stack_bytes);
  t->spawned = t->thread != nullptr;
  return t->spawned;
}

bool Task::sync_tables() {
  // The old tables are freed when the new ones are ready, and the program's other threads (time-sliced on this core) still have the old ones: none of them may run in
  // between, so nothing interrupts the change of tables.
  const uint64_t flags = irq_save();
  const bool ok = tables.sync(program.address_space());
  if (ok) {
    const uint64_t root = tables.root();
    for (UserThread* t = threads; t; t = t->next) {
      if (t->spawned && t->thread && !t->joined && !t->finished && t->thread->user_cr3 != root) set_thread_user_cr3(t->thread, root);
    }
  }
  irq_restore(flags);
  return ok;
}

bool Task::start_thread(const process::ThreadStart& start, bool suspended) {
  Thread* me = current_thread();
  if (home_core < 0 && me) {      // the first extra thread: the program's threads stay on this core from now on
    home_core = static_cast<int32_t>(current_core());
    set_thread_affinity(me, home_core);
  }
  UserThread* t = add_thread(start);
  if (!t) return false;
  if (!sync_tables()) return false;      // the new thread's stack and TEB are in the tables before it runs
  return suspended || launch(t, kUserThreadKernelStack);
}

bool Task::resume_thread(uint64_t id) {
  for (UserThread* t = threads; t; t = t->next) {
    if (t->id != id) continue;
    return t->spawned || launch(t, kUserThreadKernelStack);
  }
  return false;
}

UniquePointer<Task> spawn(process::UserProgram& program, allocators::IAllocator& allocator, const char* name) {
  if (!g.ready || !program.loaded()) return UniquePointer<Task>();
  UniquePointer<Task> task = UniquePointer<Task>::make(allocator, allocator, program, g.nx);
  if (!task) return task;
  // The pages the CPU needs while it is on the program's tables: the entry stubs (code), the interrupt table, and the cores' pages (entry stacks).
  for (uint32_t i = 0; i < g.range_count; ++i) {
    const process::AddressRange& r = g.ranges[i];
    const bool code = i == 0, read_only = i <= 1;
    if (!task->tables.add_identity({r.base, r.size, !read_only, code})) return UniquePointer<Task>();
  }
  if (!task->tables.rebuild(program.address_space())) return UniquePointer<Task>();
  const process::ProgramStart& start = program.start();
  process::ThreadStart first;
  first.entry = start.entry;
  first.argument = start.argument;
  first.argument2 = start.argument2;
  first.stack_pointer = start.stack_pointer;
  first.fs_base = start.fs_base;
  first.gs_base = start.gs_base;
  first.id = program.process()->id();
  UserThread* t = task->add_thread(first);
  if (!t) return UniquePointer<Task>();
  program.process()->attach_runtime(task.get());
  t->thread = kernel::spawn(user_main, t, name);
  t->spawned = t->thread != nullptr;
  if (!t->spawned) return UniquePointer<Task>();
  task->set_name(StringView(name));
  {
    ScopedMutex hold(g_tasks_mutex);
    task->all_next = g_tasks;
    g_tasks = task.get();
    task->registered = true;
  }
  return task;
}

uint32_t process_snapshot(ProcessInfo* out, uint32_t capacity) {
  uint32_t n = 0;
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task && n < capacity; task = task->all_next) {
    if (!task->program.loaded()) continue;
    ProcessInfo& info = out[n++];
    info = ProcessInfo();
    info.id = task->program.process()->id();
    __builtin_memcpy(info.name, task->name, sizeof info.name);
    info.windows = task->program.process()->abi() == process::SyscallAbi::Windows;
    info.memory_bytes = task->program.address_space().mapped_bytes();
    ScopedMutex hold(task->mutex);
    info.cpu_ticks = task->retired_ticks;
    bool ready = false;
    for (const UserThread* t = task->threads; t; t = t->next) {
      if (!t->spawned || t->joined || !t->thread) continue;    // (a joined thread's memory is gone)
      ++info.threads;
      info.cpu_ticks += t->thread->cpu_ticks;
      if (t->thread->state == ThreadState::Running) info.state = ThreadState::Running;
      else if (t->thread->state == ThreadState::Ready) ready = true;
    }
    if (info.state != ThreadState::Running && ready) info.state = ThreadState::Ready;
  }
  return n;
}

void request_kill(Task& task) {
  __atomic_store_n(&task.kill_requested, true, __ATOMIC_RELEASE);
  task.changed.notify_all();      // a thread waiting for another one must see it
  if (process::ObjectManager* objects = process::system_objects()) objects->notify();
}

bool wait(Task& task, uint32_t* exit_code, uint64_t timeout_us) {
  const uint64_t ticks_per_us = hardware::tsc_ticks_per_us() ? hardware::tsc_ticks_per_us() : 1;
  const uint64_t deadline = timeout_us ? hardware::read_tsc() + timeout_us * ticks_per_us : 0;
  for (;;) {
    Thread* to_join = nullptr;
    bool running = false;
    {
      ScopedMutex hold(task.mutex);
      for (UserThread* t = task.threads; t; t = t->next) {
        if (!t->spawned || t->joined || !t->thread) continue;
        if (t->finished) {      // it is leaving: from here on nothing touches its CPU thread but this call
          to_join = t->thread;
          task.retired_ticks += t->thread->cpu_ticks;
          t->thread = nullptr;
          t->joined = true;
          break;
        }
        running = true;
      }
      if (!to_join && running) {
        uint64_t left = 0;
        if (deadline) {
          const uint64_t now = hardware::read_tsc();
          if (now >= deadline) return false;
          left = (deadline - now) / ticks_per_us + 1;
        }
        task.changed.wait(task.mutex, left);
        continue;
      }
    }
    if (!to_join) break;
    join(to_join, 0);      // (it only has to finish leaving)
  }
  *exit_code = task.program.process()->exit_code();
  return true;
}

}  // namespace usermode
}  // namespace kernel

#else

namespace kernel {
namespace usermode {
uint32_t process_snapshot(ProcessInfo*, uint32_t) { return 0; }      // (programs only run on x86-64 so far)
}  // namespace usermode
}  // namespace kernel

#endif
