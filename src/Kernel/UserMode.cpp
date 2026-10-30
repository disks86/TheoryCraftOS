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
#include "Executable/ExecutableKind.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsException.hpp"
#include "Process/WindowsMailslots.hpp"
#include "Process/WindowsRegistry.hpp"
#include "Process/WindowsMemory.hpp"
#include "Process/WindowsPipes.hpp"
#include "Process/WindowsSockets.hpp"
#include "Process/WindowsThreads.hpp"
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
  uint64_t now_ns() override {
    const uint64_t per_us = hardware::tsc_ticks_per_us() ? hardware::tsc_ticks_per_us() : 1;
    return hardware::read_tsc() / per_us * 1000 + (hardware::read_tsc() % per_us) * 1000 / per_us;
  }

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
  if (__atomic_sub_fetch(&t->task->running_threads, 1, __ATOMIC_ACQ_REL) == 0) t->task->program.process()->release_handles();      // (the last one: nothing else touches the table)
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

// Has the program been asked to end: by request_kill(), or because the job it belongs to was ended (the flag in the job is read without a lock: this runs from the timer tick).
bool kill_pending(Task* task) {
  if (__atomic_load_n(&task->kill_requested, __ATOMIC_ACQUIRE)) return true;
  uint32_t code = 0;
  process::ObjectManager* objects = process::system_objects();
  if (objects && objects->job_kill_for(task->program.process()->id(), &code)) {
    task->kill_code = code;
    __atomic_store_n(&task->kill_requested, true, __ATOMIC_RELEASE);
    task->killed = true;
    return true;
  }
  return false;
}

// The thread was asked to stop (SuspendThread) or to end (TerminateThread by another thread). With the program's lock held and interrupts on: a thread told to end ends (its
// stack goes, and the process with its last thread); one told to stop keeps its registers, says it is parked, and waits to be resumed (it may find other registers in the
// frame then: SetThreadContext). True if the thread has ended and must leave.
bool stop_point(UserThread* ut, Frame* frame) {
  Task* task = ut->task;
  process::Process* process = task->program.process();
  while (true) {
    if (process->exited() || kill_pending(task)) break;      // (the caller deals with the end of the program)
    if (__atomic_load_n(&ut->terminate_requested, __ATOMIC_ACQUIRE)) {
      if (process::Process::ThreadRecord* record = process->find_thread(ut->id)) process::windowsabi::end_thread(*process, *record, ut->terminate_code);
      return true;
    }
    if (!__atomic_load_n(&ut->suspend_requested, __ATOMIC_ACQUIRE)) break;
    if (!ut->parked) {
      frame_to_context(*frame, &ut->saved);
      ut->parked = true;
      task->changed.notify_all();
    }
    task->changed.wait(task->mutex, 0);
  }
  if (ut->parked) {
    ut->parked = false;
    if (ut->context_dirty) { context_to_frame(ut->saved, frame); ut->context_dirty = false; }
  }
  return false;
}

// Timer tick in user mode: a program that was told to stop gets its instruction pointer moved to page zero, which is never mapped. Its next step is a page
// fault, which on_user_fault() recognises as the end the program was asked for.
void on_user_tick(Frame* frame) {
  Task* task = current_task();
  if (task && (kill_pending(task) || task->program.process()->exited())) {
    frame->rip = 0;
  } else if (UserThread* u = current_user_thread()) {      // asked to stop or end: send it to address 0 (the fault there is how it gets into the kernel in a state it can wait in)
    if (!u->redirected && (__atomic_load_n(&u->suspend_requested, __ATOMIC_ACQUIRE) || __atomic_load_n(&u->terminate_requested, __ATOMIC_ACQUIRE))) {
      u->saved_rip = frame->rip;
      u->redirected = true;
      frame->rip = 0;
    }
  }
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
    process->terminate(task->kill_code);
    thread_ends_now(self);
    exit_thread();
  }
  task->mutex.lock();      // the process's lock: the call, and what follows it, change the process's tables
  process::X86UserRegisters regs;
  regs.rax = frame->rax; regs.rcx = frame->rcx; regs.rdx = frame->rdx; regs.rbx = frame->rbx; regs.rsp = frame->rsp; regs.rbp = frame->rbp;
  regs.rsi = frame->rsi; regs.rdi = frame->rdi; regs.r8 = frame->r8; regs.r9 = frame->r9; regs.r10 = frame->r10; regs.r11 = frame->r11;
  process::SyscallRequest request;
  self->syscall_frame = frame;
  if (process::decode_x86_64(process->abi(), regs, &request)) frame->rax = process->syscall(request);
  else frame->rax = ~uint64_t{37};      // -ENOSYS
  self->syscall_frame = nullptr;
  uint64_t fs = 0, gs = 0;
  const bool fs_changed = process->take_fs_change(&fs), gs_changed = process->take_gs_change(&gs);
  if (fs_changed || gs_changed) set_current_user_bases(fs_changed, fs, gs_changed, gs);      // arch_prctl
  bool alive = !process->exited();
  uint32_t thread_code = 0;
  bool thread_ends = process->take_thread_exit(&thread_code);
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
  if (alive && !thread_ends && stop_point(self, frame)) thread_ends = true;      // SuspendThread / TerminateThread from another thread
  if (alive && kill_pending(task)) {      // asked to stop while the call ran (or in it)
    task->killed = true;
    process->terminate(task->kill_code);
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
  // CR2 is read first, with interrupts still off: once they are on, a thread switch can bring another fault to this core, which changes CR2.
  const uint64_t page_fault_address = (frame->vector & 0xFF) == 14 ? read_cr2() : 0;
  Task* task = current_task();
  if (!task) panic("fault in user mode on a thread that is not a user program", frame);
  if ((kill_pending(task) || task->program.process()->exited()) && (frame->vector & 0xFF) == 14 && frame->rip == 0) {      // the end on_user_tick() arranged: not a fault of the program's
    if (!task->program.process()->exited()) task->killed = true;
    task->program.process()->terminate(task->kill_code);      // (a program that already ended keeps its exit code)
    thread_ends_now(current_user_thread());
    irq_enable();
    exit_thread();
  }
  if (UserThread* ut = current_user_thread(); ut && ut->redirected && (frame->vector & 0xFF) == 14 && frame->rip == 0) {      // the tick sent it here (see on_user_tick)
    ut->redirected = false;
    frame->rip = ut->saved_rip;
    irq_enable();
    task->mutex.lock();
    const bool ends = stop_point(ut, frame);
    if (ends) {
      task->sync_tables();      // (its stack is gone)
      thread_ends_now(ut);
    }
    task->mutex.unlock();
    if (ends) exit_thread();
    irq_disable();
    return frame;
  }
  // A guard page (PAGE_GUARD) gives way on its first access: a stack grows by a page, anything else raises STATUS_GUARD_PAGE_VIOLATION once.
  int guard = 0;
  process::ExceptionInfo guard_info;
  if ((frame->vector & 0xFF) == 14 && task->program.process()->abi() == process::SyscallAbi::Windows) {
    const uint64_t guard_address = page_fault_address;
    uint32_t low = 0, high = 0;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000102u));      // (the program's GS base, the TEB, is parked in KERNEL_GS_BASE while we run)
    irq_enable();
    task->mutex.lock();
    const process::windowsabi::GuardResult result = process::windowsabi::guard_fault(*task->program.process(), (uint64_t{high} << 32) | low, guard_address, (frame->error & 2) != 0, &guard_info);
    if (result != process::windowsabi::GuardResult::NotGuard) task->sync_tables();
    task->mutex.unlock();
    irq_disable();
    if (result == process::windowsabi::GuardResult::Retry) return frame;
    if (result == process::windowsabi::GuardResult::Exception) guard = 1;
  }
  // A page the program may use but the CPU does not have in the tables it runs with (another thread, or a parent changing this program's memory, rebuilt them since): the
  // tables are rebuilt and the instruction runs again. A fault at the same place a few times in a row is real.
  if ((frame->vector & 0xFF) == 14 && !(frame->error & 1) && task->program.process()->abi() == process::SyscallAbi::Windows) {
    const uint64_t stale_address = page_fault_address;
    process::AddressSpace* space = task->program.process()->address_space();
    UserThread* self = current_user_thread();
    if (space && self && stale_address < 0x0000800000000000ull) {
      const uint8_t need = (frame->error & 0x10) ? process::kProtExecute : (frame->error & 2) ? process::kProtWrite : process::kProtRead;
      bool again = false;
      irq_enable();
      task->mutex.lock();
      if (space->check(stale_address, 1, need)) {
        if (self->stale_rip == frame->rip && self->stale_address == stale_address) ++self->stale_count; else { self->stale_rip = frame->rip; self->stale_address = stale_address; self->stale_count = 1; }
        again = self->stale_count <= 3 && task->sync_tables();
      }
      task->mutex.unlock();
      irq_disable();
      if (again) return frame;
    }
  }
  // A write to a copy-on-write page (PAGE_WRITECOPY) is not a fault: the page gets its own copy and the instruction runs again.
  if ((frame->vector & 0xFF) == 14 && (frame->error & 3) == 3 && task->program.process()->abi() == process::SyscallAbi::Windows) {
    const uint64_t cow_address = page_fault_address;
    irq_enable();
    task->mutex.lock();
    process::AddressSpace* space = task->program.process()->address_space();
    const bool copied = space && ((space->break_copy_on_write(cow_address, 1) && task->sync_tables()) || space->check(cow_address, 1, process::kProtWrite));      // (or another thread got there first)
    task->mutex.unlock();
    irq_disable();
    if (copied) return frame;
  }
  // A Windows program gets its exception delivered, to be handled by SEH or C++ code (or to end up in ntdll's last resort); only when that cannot be done is the program stopped.
  process::Process* process = task->program.process();
  if ((frame->vector & 0xFF) == 13 && frame->error == 0x16A && frame->rax == 1 && process->abi() == process::SyscallAbi::Windows) {      // INT 2D, service 1 (a prompt): nothing to do
    frame->rip += 2;
    return frame;
  }
  if (process->abi() == process::SyscallAbi::Windows && process->exception_dispatcher() != 0) {
    process::X86Context at, entry;
    frame_to_context(*frame, &at);
    process::ExceptionInfo info;
    const uint64_t vector = frame->vector & 0xFF;
    const uint64_t address = vector == 14 ? page_fault_address : 0;
    bool delivered = false;
    if (guard ? (info = guard_info, true) : process::fault_to_exception(vector, frame->error, address, &at, &info)) {
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
  task->fault_address = page_fault_address;
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
  process::g_process_log = [](const char* text) { console::puts(text); };
  if (g.ready) return true;
  if (!running()) return false;
  uint32_t eax = 0x80000001, ebx, ecx = 0, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
  if (!(edx & (1u << 11))) return false;      // no SYSCALL
  g.allocator = &allocator;
  if (!g.sync && !process::system_objects()) {      // (an emulated program may have made the system-wide objects already: both kinds of program share them)
    void* sync_memory = allocator.allocate(sizeof(SystemSync));
    void* objects_memory = sync_memory ? allocator.allocate(sizeof(process::ObjectManager)) : nullptr;
    if (!objects_memory) { if (sync_memory) allocator.deallocate(sync_memory); return false; }
    g.sync = new (sync_memory) SystemSync();
    g.objects = new (objects_memory) process::ObjectManager(*g.sync);
    g.objects->set_allocator(&allocator);
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
  reap_children(true);
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

bool Task::interrupted() {
  if (kill_pending(this) || program.process()->exited()) return true;      // (also true when the job was ended)
  UserThread* u = current_user_thread();
  return u && __atomic_load_n(&u->terminate_requested, __ATOMIC_ACQUIRE);      // (a thread told to end stops waiting)
}

UserThread* Task::find_thread(uint64_t id) {
  for (UserThread* t = threads; t; t = t->next) if (t->id == id) return t;
  return nullptr;
}
void Task::request_suspend(uint64_t id) {
  if (UserThread* t = find_thread(id)) __atomic_store_n(&t->suspend_requested, true, __ATOMIC_RELEASE);
}
void Task::request_resume(uint64_t id) {
  if (UserThread* t = find_thread(id)) __atomic_store_n(&t->suspend_requested, false, __ATOMIC_RELEASE);
  changed.notify_all();
}
int Task::park_point() {
  UserThread* ut = current_user_thread();
  if (!ut || ut->task != this || !ut->syscall_frame || !__atomic_load_n(&ut->suspend_requested, __ATOMIC_ACQUIRE)) return 0;
  return stop_point(ut, ut->syscall_frame) ? 2 : 1;
}
bool Task::is_parked(uint64_t id) {
  UserThread* t = find_thread(id);
  return t && t->parked;
}
bool Task::get_context(uint64_t id, process::X86Context* out) {
  UserThread* t = find_thread(id);
  if (!t) return false;
  if (!t->spawned) {      // it has not run yet: its registers are the ones it will start with
    *out = process::X86Context();
    out->rip = t->start.entry;
    out->gpr[process::X86Context::kRsp] = t->start.stack_pointer;
    out->gpr[process::X86Context::kRcx] = t->start.argument;
    out->gpr[process::X86Context::kRdx] = t->start.argument2;
    return true;
  }
  if (!t->parked) return false;
  *out = t->saved;
  return true;
}
bool Task::set_context(uint64_t id, const process::X86Context& context) {
  UserThread* t = find_thread(id);
  if (!t) return false;
  if (!t->spawned) {
    t->start.entry = context.rip;
    t->start.stack_pointer = context.gpr[process::X86Context::kRsp];
    t->start.argument = context.gpr[process::X86Context::kRcx];
    t->start.argument2 = context.gpr[process::X86Context::kRdx];
    return true;
  }
  if (!t->parked) return false;
  t->saved = context;
  t->context_dirty = true;
  return true;
}
void Task::request_terminate(uint64_t id, uint32_t code) {
  UserThread* t = find_thread(id);
  if (!t) return;
  t->terminate_code = code;
  __atomic_store_n(&t->terminate_requested, true, __ATOMIC_RELEASE);
  changed.notify_all();      // (a parked thread wakes to end)
}

uint64_t Task::current_thread_id() {
  UserThread* u = current_user_thread();
  return u ? u->id : 0;
}

UserThread* Task::add_thread(const process::ThreadStart& start) {
  void* memory = allocator_.allocate(sizeof(UserThread));
  if (!memory) return nullptr;
  UserThread* t = new (memory) UserThread();
  t->task = this;
  t->core_mask = process_mask;
  t->id = start.id;
  t->start = start;
  t->next = threads;
  threads = t;
  return t;
}

constexpr uint32_t kUserThreadKernelStack = 256 * 1024;

bool Task::launch(UserThread* t, uint32_t stack_bytes) {
  __atomic_add_fetch(&running_threads, 1, __ATOMIC_ACQ_REL);      // (before the thread can run and end)
  t->thread = kernel::spawn(user_main, t, "user-thread", -1, -1, stack_bytes);
  t->spawned = t->thread != nullptr;
  if (!t->spawned) __atomic_sub_fetch(&running_threads, 1, __ATOMIC_ACQ_REL);
  if (t->spawned && t->core_mask != ~uint64_t{0}) set_thread_core_mask(t->thread, t->core_mask);
  if (t->spawned && t->priority != kernel::kDefaultPriority) kernel::set_thread_priority(t->thread, t->priority);
  return t->spawned;
}

namespace {
MemorySource g_memory_source = nullptr;
void* g_memory_context = nullptr;
}  // namespace

void set_memory_source(MemorySource source, void* context) {
  g_memory_source = source;
  g_memory_context = context;
}

uint32_t Task::processor_count() {
  const uint64_t online = online_core_mask();
  uint32_t n = 0;
  for (uint64_t m = online; m; m &= m - 1) ++n;
  return n ? n : 1;
}

void Task::memory_status(uint64_t* total_bytes, uint64_t* free_bytes) {
  *total_bytes = *free_bytes = 0;
  if (g_memory_source) g_memory_source(g_memory_context, total_bytes, free_bytes);
}

bool Task::set_priority(uint64_t thread_id, int32_t absolute_priority) {
  if (absolute_priority < 0) absolute_priority = 0;
  if (absolute_priority > kernel::kPriorityLevels - 1) absolute_priority = kernel::kPriorityLevels - 1;
  UserThread* t = find_thread(thread_id);
  if (!t) return false;
  t->priority = static_cast<uint8_t>(absolute_priority);
  if (t->spawned && t->thread && !t->joined && !t->finished) kernel::set_thread_priority(t->thread, t->priority);
  return true;
}

void Task::flush_write_buffers() {
  CoreMask all;
  const uint64_t online = online_core_mask();
  for (uint32_t i = 0; i < 64; ++i) if ((online >> i) & 1) all.add(i);
  wait_for_kernel_entry(all);      // (an interrupt drains the store buffer of the core it lands on)
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

bool Task::set_affinity(uint64_t thread_id, uint64_t mask) {
  const uint64_t online = online_core_mask();
  if (thread_id == ~uint64_t{0}) {      // the mask new threads get, with the threads there are left alone
    if (!(mask & online)) return false;
    process_mask = mask;
    return true;
  }
  if (thread_id == 0) {      // the process's mask: a thread that follows it takes it; one with a mask of its own keeps what is left of that (or takes it if nothing is)
    if (!(mask & online)) return false;
    process_mask = mask;
    for (UserThread* t = threads; t; t = t->next) {
      if (t->affinity_explicit && (t->core_mask & mask & online)) t->core_mask &= mask;
      else { t->core_mask = mask; t->affinity_explicit = false; }
      if (t->spawned && t->thread && !t->joined && !t->finished) set_thread_core_mask(t->thread, t->core_mask);
    }
    return true;
  }
  UserThread* t = find_thread(thread_id);
  if (!t) return false;
  if (mask == 0) {      // no mask: back to following the process's
    t->core_mask = process_mask;
    t->affinity_explicit = false;
  } else {
    if (!(mask & online)) return false;
    t->core_mask = mask;
    t->affinity_explicit = true;
  }
  if (t->spawned && t->thread && !t->joined && !t->finished) set_thread_core_mask(t->thread, t->core_mask);
  return true;
}

uint64_t Task::affinity(uint64_t thread_id) {
  uint64_t mask = process_mask;
  if (thread_id != 0) {
    UserThread* t = find_thread(thread_id);
    if (t) mask = t->core_mask;
  }
  return mask & online_core_mask();
}

bool Task::cpu_time(uint64_t thread_id, uint64_t* hundred_ns) {
  const uint64_t hz = tick_rate() ? tick_rate() : 1;
  uint64_t ticks = 0;
  if (thread_id == 0) {
    ticks = retired_ticks;
    for (const UserThread* t = threads; t; t = t->next) if (t->spawned && !t->joined && t->thread) ticks += t->thread->cpu_ticks;
  } else {
    const UserThread* t = find_thread(thread_id);
    if (!t) return false;
    if (t->spawned && !t->joined && t->thread) ticks = t->thread->cpu_ticks;
  }
  *hundred_ns = ticks * (10000000ull / hz);
  return true;
}

uint32_t Task::current_processor() { return current_core(); }

bool Task::process_exists(uint64_t pid) {
  ScopedMutex registry(g_tasks_mutex);
  for (const Task* task = g_tasks; task; task = task->all_next) if (task->program.loaded() && task->program.process()->id() == pid && !task->program.process()->exited()) return true;
  return false;
}

uint32_t Task::list_processes(ProcessEntry* out, uint32_t capacity) {
  ProcessInfo info[64];
  // (a system call holds this program's lock, and the snapshot takes every program's lock, this one's too: let go of it meanwhile)
  mutex.unlock();
  const uint32_t n = process_snapshot(info, capacity < 64 ? capacity : 64);
  mutex.lock();
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = ProcessEntry();
    out[i].pid = info[i].id;
    out[i].parent = info[i].parent_id;
    out[i].threads = info[i].threads;
    out[i].memory_bytes = info[i].memory_bytes;
    out[i].create_time = info[i].create_time;
    __builtin_memcpy(out[i].name, info[i].name, sizeof out[i].name);
  }
  return n;
}

uint32_t Task::remote_operation(uint64_t pid, RemoteOp& op) {
  mutex.unlock();      // (the other program's lock is taken, and the lock of this one must not be held meanwhile: two programs working on each other would wait for each other)
  const uint32_t status = usermode::remote_operation(pid, op);
  mutex.lock();
  return status;
}

uint32_t Task::list_threads(uint64_t pid, ThreadEntry* out, uint32_t capacity) {
  mutex.unlock();      // (see list_processes)
  const uint32_t n = usermode::thread_snapshot(pid, out, capacity);
  mutex.lock();
  return n;
}

uint32_t Task::list_modules(uint64_t pid, ModuleEntry* out, uint32_t capacity) {
  mutex.unlock();
  const uint32_t n = usermode::module_snapshot(pid, out, capacity);
  mutex.lock();
  return n;
}

bool Task::sync_tables() {
  // The program's threads run side by side on every core, each on the page tables it was given. New tables are built beside the old ones; every thread is then given the new
  // ones (a thread running on another core takes them when that core's interrupt brings it into the kernel and back), and only after all those cores have answered are the old
  // tables and the memory of what was unmapped given back: until then another core may still be running on them.
  if (!tables.sync(program.address_space(), true)) return false;
  const uint64_t root = tables.root();
  CoreMask others;
  for (UserThread* t = threads; t; t = t->next) {
    if (!t->spawned || !t->thread || t->joined || t->finished || t->thread->user_cr3 == root) continue;
    const int32_t core = set_thread_user_cr3(t->thread, root);
    if (core >= 0) others.add(static_cast<uint32_t>(core));
  }
  bool answered = true;
  if (!others.empty()) answered = wait_for_kernel_entry(others);
  if (answered) {
    tables.release_previous();
    program.address_space().reclaim();
  } else {
    console::puts("user program: a core did not give up the old page tables\n");      // (the old tables and memory are kept until the next change)
  }
  return true;
}

bool Task::start_thread(const process::ThreadStart& start, bool suspended) {
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

// A program started by CreateProcess: its own Task, run on its own CPU threads, sharing the files, the libraries and the console of the program that started it.
struct Task::Child {
  UniquePointer<process::UserProgram> program;
  UniquePointer<Task> task;
  uint64_t pid = 0;
  bool started = true;
  bool busy = false;      // (orphans) one reaper is looking at it
  allocators::IAllocator* owner = nullptr;      // where the record came from (an orphan is freed by whoever reaps it)
  Child* next = nullptr;
};

// The programs whose parent has ended. They go on (as on Windows): whoever starts a program next frees the ones that have ended since, and anyone may still terminate or
// resume them by id.
namespace {
Mutex g_orphans_mutex;
Task::Child* g_orphans = nullptr;
}  // namespace

static void destroy_child(Task::Child* c) {
  allocators::IAllocator* owner = c->owner;
  c->~Child();
  owner->deallocate(c);
}

static void reap_orphans() {
  constexpr uint32_t kBatch = 16;
  Task::Child* batch[kBatch];
  uint32_t count = 0;
  {
    ScopedMutex hold(g_orphans_mutex);
    for (Task::Child* o = g_orphans; o && count < kBatch; o = o->next) if (!o->busy) { o->busy = true; batch[count++] = o; }
  }
  for (uint32_t i = 0; i < count; ++i) {
    Task::Child* c = batch[i];
    uint32_t code = 0;
    const bool finished = c->started ? usermode::wait(*c->task, &code, 1) : c->task->program.process()->exited();
    {
      ScopedMutex hold(g_orphans_mutex);
      if (!finished) c->busy = false;
      else for (Task::Child** link = &g_orphans; *link; link = &(*link)->next) if (*link == c) { *link = c->next; break; }
    }
    if (finished) {
      if (process::ObjectManager* objects = process::system_objects()) objects->process_exited(c->pid, c->task->program.process()->exit_code());
      destroy_child(c);
    }
  }
}

uint64_t Task::allocate_process_id() {
  static uint64_t next = 0x40000;
  static Mutex ids;
  ScopedMutex hold(ids);
  const uint64_t id = next++;
  if (next >= 0x100000) next = 0x40000;      // (a thread id carries its process's id in the low 20 bits)
  return id;
}

uint32_t Task::spawn_process(const process::ChildSpec& spec) {
  constexpr uint32_t kNoMemory = 0xC0000017u, kInvalidImage = 0xC000007Bu, kNoResources = 0xC000009Au;
  reap_children(false);
  reap_orphans();
  const process::SyscallAbi abi = process::abi_for_executable(exe::identify_executable(spec.image.at_most(exe::kIdentifyBytes)));
  if (abi == process::SyscallAbi::None) return kInvalidImage;
  void* memory = allocator_.allocate(sizeof(Child));
  if (!memory) return kNoMemory;
  Child* child = new (memory) Child();
  child->owner = &allocator_;
  auto discard = [&]() { child->~Child(); allocator_.deallocate(child); };
  child->pid = spec.pid;
  child->program = UniquePointer<process::UserProgram>::make(allocator_, allocator_, spec.pid, program.host());
  if (!child->program) { discard(); return kNoMemory; }
  process::UserProgram& p = *child->program;
  p.address_space().set_reserved(reserved_ranges());
  p.set_file_system(program.file_system());
  if (abi == process::SyscallAbi::Windows) p.set_library_provider(program.library_provider());
  p.set_working_directory(spec.working_directory);
  p.set_processor_count(processor_count());
  p.set_command_line(spec.command_line);
  p.set_startup(spec.startup);
  // argv: the program's name, then (for a Linux program) the words of the command line
  StringView argv[32];
  uint32_t argc = 0;
  argv[argc++] = spec.image_path;
  if (abi != process::SyscallAbi::Windows) {
    StringView rest = spec.command_line;
    bool first_word = true;
    while (argc < 32) {
      while (!rest.empty() && rest[0] == ' ') rest = rest.without_prefix(1);
      if (rest.empty()) break;
      uint64_t end = rest.find(' ');
      if (end == StringView::npos) end = rest.size();
      if (!first_word) argv[argc++] = rest.first(end);
      first_word = false;
      rest = rest.without_prefix(end);
    }
  }
  uint8_t random[16];
  uint64_t t = hardware::read_tsc() ^ (spec.pid * 0x9E3779B97F4A7C15ull);
  for (uint8_t& b : random) {
    t = t * 6364136223846793005ull + 1442695040888963407ull;
    b = static_cast<uint8_t>(t >> 56);
  }
  const process::LoadStatus loaded = p.load(spec.image, ArrayView<const StringView>(argv, argc), spec.environment, ConstByteView(random, sizeof(random)));
  if (loaded != process::LoadStatus::Ok) {
    discard();
    return loaded == process::LoadStatus::OutOfMemory ? kNoMemory : kInvalidImage;
  }
  if (spec.inherit_handles) {
    auto count_use = [](process::DescriptorKind kind, uint64_t token) {      // the child's table holds one more use of what the parent's does
      if (process::ObjectManager* objects = process::system_objects()) {
        if (kind == process::DescriptorKind::Pipe) process::windowsabi::pipe_token_add(token);
      else if (kind == process::DescriptorKind::Socket) process::windowsabi::socket_token_add(token);
        else if (kind == process::DescriptorKind::Mailslot) process::windowsabi::mailslot_token_add(token);
        else if (kind == process::DescriptorKind::Key) process::windowsabi::key_token_add(token);
        else if (kind == process::DescriptorKind::Event || kind == process::DescriptorKind::Mutex || kind == process::DescriptorKind::Semaphore || kind == process::DescriptorKind::Process || kind == process::DescriptorKind::Timer || kind == process::DescriptorKind::Section || kind == process::DescriptorKind::Job || kind == process::DescriptorKind::ChangeNotification || kind == process::DescriptorKind::DebugObject) objects->add_ref(uint32_t(token));
      }
    };
    p.process()->files().inherit_from(program.process()->files(), count_use);
  }
  if (spec.use_std_handles) p.set_standard_handles(spec.std_handles);
  if (p.process()) p.process()->set_parent_id(program.process()->id());
  child->task = usermode::spawn(p, allocator_, "program", spec.suspended);
  if (!child->task) { discard(); return kNoResources; }
  child->started = !spec.suspended;
  child->task->parent_id = program.process()->id();
  {      // the name the process list shows: the program's file name
    StringView base = spec.image_path;
    for (uint64_t i = 0; i < base.size(); ++i) if (base[i] == '\\' || base[i] == '/') { base = base.without_prefix(i + 1); i = static_cast<uint64_t>(-1); }
    child->task->set_name(base);
  }
  child->next = children;
  children = child;
  return 0;
}

static bool terminate_child(Task::Child* c, uint32_t code) {
  if (c->task->program.process()->exited()) return false;
  if (!c->started) {      // it never ran: there is no thread to stop
    c->task->kill_code = code;
    c->task->killed = true;
    c->task->program.process()->terminate(code);
    c->task->program.process()->release_handles();
    return true;
  }
  request_kill(*c->task, code);
  c->program->host().wake_blocked();
  return true;
}

static bool resume_child(Task::Child* c) {
  if (c->started) return false;
  c->started = true;
  return c->task->start_first_thread();
}

// Does `action` to the child with this id wherever it is: among the programs this one started, those whose parent has ended, or those another program started (the
// program may be asked about by anyone who knows its id). The bool says whether there was one.
template <typename Action>
static bool with_child(Task& self, uint64_t pid, Action action) {
  for (Task::Child* c = self.children; c; c = c->next) if (c->pid == pid) return action(c);
  {
    ScopedMutex hold(g_orphans_mutex);
    for (Task::Child* c = g_orphans; c; c = c->next) if (c->pid == pid) return action(c);
  }
  bool found = false, result = false;
  self.mutex.unlock();      // (another program's lock is taken, and no lock of this one may be held meanwhile: see remote_operation)
  {
    ScopedMutex registry(g_tasks_mutex);
    for (Task* t = g_tasks; t && !found; t = t->all_next) {
      if (t == &self) continue;
      ScopedMutex hold(t->mutex);
      for (Task::Child* c = t->children; c; c = c->next) if (c->pid == pid) { found = true; result = action(c); break; }
    }
  }
  self.mutex.lock();
  return found && result;
}

bool Task::terminate_process(uint64_t pid, uint32_t code) {
  return with_child(*this, pid, [&](Child* c) { return terminate_child(c, code); });
}

bool Task::resume_process(uint64_t pid) {
  return with_child(*this, pid, [&](Child* c) { return resume_child(c); });
}

void Task::reap_children(bool all) {
  Child** link = &children;
  while (*link) {
    Child* c = *link;
    Task& t = *c->task;
    uint32_t code = 0;
    if (all) {
      if (!c->task->program.process()->exited()) {      // the program goes on without its parent
        *link = c->next;
        ScopedMutex hold(g_orphans_mutex);
        c->next = g_orphans;
        g_orphans = c;
        continue;
      }
      if (c->started) usermode::wait(t, &code);
    } else if (!c->started || !usermode::wait(t, &code, 1)) {
      link = &c->next;
      continue;
    }
    if (process::ObjectManager* objects = process::system_objects()) objects->process_exited(c->pid, c->task->program.process()->exit_code());
    *link = c->next;
    c->~Child();
    allocator_.deallocate(c);
  }
}

UniquePointer<Task> spawn(process::UserProgram& program, allocators::IAllocator& allocator, const char* name, bool suspended) {
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
  program.address_space().set_deferred_free(true);      // (see Task::sync_tables)
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
  if (suspended) {      // it waits for Task::start_first_thread()
    task->first_thread = t;
  } else {
    __atomic_add_fetch(&task->running_threads, 1, __ATOMIC_ACQ_REL);      // (before the thread can run and end)
    t->thread = kernel::spawn(user_main, t, name);
    t->spawned = t->thread != nullptr;
    if (!t->spawned) return UniquePointer<Task>();
  }
  task->set_name(StringView(name));
  {
    ScopedMutex hold(g_tasks_mutex);
    task->all_next = g_tasks;
    g_tasks = task.get();
    task->registered = true;
  }
  return task;
}

uint32_t remote_operation(uint64_t pid, process::IProcessRuntime::RemoteOp& op) {
  constexpr uint32_t kInvalidCid = 0xC000000Bu;
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task; task = task->all_next) {
    if (!task->program.loaded() || task->program.process()->id() != pid) continue;
    ScopedMutex hold(task->mutex);
    const uint32_t status = process::windowsabi::run_remote(*task->program.process(), op);
    // The layout may have changed: the page tables are rebuilt and given to the threads of that program (on whatever cores they run), as at the end of one of its own system calls.
    using Op = process::IProcessRuntime::RemoteOp;
    const bool layout = op.kind == Op::Allocate || op.kind == Op::Free || op.kind == Op::Protect || op.kind == Op::MapView || op.kind == Op::Unmap;      // (a view that came or went changes the layout too)
    if (layout && status == 0 && !task->sync_tables()) return 0xC0000017u;
    return status;
  }
  return kInvalidCid;
}

uint32_t thread_snapshot(uint64_t pid, process::IProcessRuntime::ThreadEntry* out, uint32_t capacity) {
  uint64_t ids[process::Process::kMaxThreads], starts[process::Process::kMaxThreads];
  int32_t priorities[process::Process::kMaxThreads];
  uint32_t n = 0;
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task; task = task->all_next) {
    if (!task->program.loaded() || task->program.process()->id() != pid) continue;
    ScopedMutex hold(task->mutex);
    n = task->program.process()->thread_snapshot(ids, starts, priorities, process::Process::kMaxThreads);
    break;
  }
  if (n > capacity) n = capacity;
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = process::IProcessRuntime::ThreadEntry();
    out[i].id = ids[i]; out[i].start_address = starts[i]; out[i].priority = priorities[i];
  }
  return n;
}

uint32_t module_snapshot(uint64_t pid, process::IProcessRuntime::ModuleEntry* out, uint32_t capacity) {
  uint32_t n = 0;
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task; task = task->all_next) {
    if (!task->program.loaded() || task->program.process()->id() != pid) continue;
    ScopedMutex hold(task->mutex);
    const process::ModuleSet* modules = task->program.modules();
    if (!modules) break;
    for (uint64_t i = 0; i < modules->count() && n < capacity; ++i) {
      const process::Module& m = modules->at(i);
      process::IProcessRuntime::ModuleEntry& e = out[n++];
      e = process::IProcessRuntime::ModuleEntry();
      e.base = m.base; e.size = m.size; e.entry = m.entry; e.program = m.main;
      uint32_t k = 0;
      for (; k + 1 < sizeof e.name && k < m.name.size(); ++k) e.name[k] = m.name.view()[k];
      e.name[k] = 0;
    }
    break;
  }
  return n;
}

uint32_t process_snapshot(ProcessInfo* out, uint32_t capacity) {
  uint32_t n = 0;
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task && n < capacity; task = task->all_next) {
    if (!task->program.loaded()) continue;
    ProcessInfo& info = out[n++];
    info = ProcessInfo();
    info.id = task->program.process()->id();
    info.parent_id = task->parent_id;
    info.create_time = task->program.process()->create_time;
    __builtin_memcpy(info.name, task->name, sizeof info.name);
    info.windows = task->program.process()->abi() == process::SyscallAbi::Windows;
    info.memory_bytes = task->program.address_space().mapped_bytes();
    ScopedMutex hold(task->mutex);
    info.cpu_ticks = task->retired_ticks;
    bool ready = false;
    for (const UserThread* t = task->threads; t; t = t->next) {
      if (t->joined) continue;    // (a joined thread's memory is gone)
      if (!t->spawned || !t->thread) { if (!t->finished) ++info.threads; continue; }      // (a thread that was made suspended is a thread already)
      ++info.threads;
      info.cpu_ticks += t->thread->cpu_ticks;
      if (t->thread->state == ThreadState::Running) info.state = ThreadState::Running;
      else if (t->thread->state == ThreadState::Ready) ready = true;
    }
    if (info.state != ThreadState::Running && ready) info.state = ThreadState::Ready;
  }
  return n;
}

bool Task::start_first_thread() {
  UserThread* t = first_thread;
  if (!t || t->spawned) return false;
  first_thread = nullptr;
  __atomic_add_fetch(&running_threads, 1, __ATOMIC_ACQ_REL);
  t->thread = kernel::spawn(user_main, t, "program");
  t->spawned = t->thread != nullptr;
  if (!t->spawned) __atomic_sub_fetch(&running_threads, 1, __ATOMIC_ACQ_REL);
  return t->spawned;
}

void request_kill(Task& task, uint32_t exit_code) {
  task.kill_code = exit_code;
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
        // (A thread that ends sets `finished` and wakes us without holding the lock, so the wake-up can fall between our look and our sleep: never sleep long.)
        if (left == 0 || left > 2000) left = 2000;
        task.changed.wait(task.mutex, left);
        continue;
      }
    }
    if (!to_join) break;
    join(to_join, 0);      // (it only has to finish leaving)
  }
  task.reap_children(true);      // (the programs it started do not outlive it)
  *exit_code = task.program.process()->exit_code();
  return true;
}

}  // namespace usermode
}  // namespace kernel

#else

namespace kernel {
namespace usermode {
uint32_t process_snapshot(ProcessInfo*, uint32_t) { return 0; }      // (programs only run on x86-64 so far)
uint32_t thread_snapshot(uint64_t, process::IProcessRuntime::ThreadEntry*, uint32_t) { return 0; }
uint32_t remote_operation(uint64_t, process::IProcessRuntime::RemoteOp&) { return 0xC0000002u; }
uint32_t module_snapshot(uint64_t, process::IProcessRuntime::ModuleEntry*, uint32_t) { return 0; }
}  // namespace usermode
}  // namespace kernel

#endif
