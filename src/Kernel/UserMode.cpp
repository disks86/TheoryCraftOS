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
#include "Process/Win32k/Win32k.hpp"
#include "Kernel/UserMode.hpp"
#include "Process/ImageCache.hpp"
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
  // Interrupts save the x87/SSE state only; the AVX and AVX-512 registers (which the kernel never uses) go with their thread at each switch (see enable_extended_state),
  // so programs may use them. Without XSAVE support user mode is told there is none (CPUID reports OSXSAVE clear).
  if (!enable_extended_state()) {
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1u << 18)) __asm__ volatile("movq %0, %%cr4" ::"r"(cr4 & ~uint64_t{1u << 18}) : "memory");
  }
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
    u->tick_rip = frame->rip;
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
  self->call_service = frame->rax;
  self->call_rip = frame->rip;
  self->call_rsp = frame->rsp;
  ++self->calls;
  self->in_call = true;
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
  process::Process::LinuxThreadState::CloneRequest clone;
  if (process->take_clone(&clone) && !task->start_linux_thread(*frame, clone)) frame->rax = uint64_t(-int64_t{11});      // clone()/clone3(): -EAGAIN if no thread
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
  self->in_call = false;
  task->mutex.unlock();
  if (!alive || thread_ends) exit_thread();
  irq_disable();      // the way back is not re-entrant
  return frame;
}

// Each exception a processor fault raises in a Windows program goes to the debug console (the first 64 of each program): a program that handles them quietly
// but goes wrong afterwards can then be followed.
// (with the module and offset when the address is inside one; false if it is not)
bool put_code_address(const process::ModuleSet* modules, uint64_t address) {
  console::put_hex(address);
  for (uint64_t i = 0; modules && i < modules->count(); ++i) {
    const process::Module& m = modules->at(i);
    if (address < m.base || address - m.base >= m.size) continue;
    console::puts(" (");
    console::puts(m.name.view());
    console::puts("+");
    console::put_hex(address - m.base);
    console::puts(")");
    return true;
  }
  return false;
}

void log_exception(Task* task, uint32_t code, uint64_t rip, uint64_t address, uint64_t rsp) {
  if (task->exceptions_logged >= 64) return;
  ++task->exceptions_logged;
  const process::ModuleSet* modules = task->program.modules();
  console::puts("user program: exception ");
  console::put_hex(code);
  console::puts(" at ");
  put_code_address(modules, rip);
  console::puts(" address ");
  console::put_hex(address);
  process::AddressSpace& space = task->program.address_space();      // what is mapped there: the region and its neighbour below
  const uint64_t index = space.region_index(address);
  process::AddressSpace::RegionInfo region;
  for (uint64_t i = index > 0 ? index - 1 : 0; i <= index && space.region_at(i, &region); ++i) {
    console::puts(i < index || region.base > address ? "\n    region below/after " : "\n    region ");
    console::put_hex(region.base);
    console::puts("+");
    console::put_hex(region.size);
    console::puts(" prot ");
    console::put_dec(region.protection);
    console::puts(" protect ");
    console::put_hex(region.attributes.protect);
    console::puts(region.attributes.reserved ? " reserved" : "");
    console::puts(region.storage ? " shared" : " private");
    console::puts(region.attributes.type == process::PageAttributes::Mapped ? " view" : region.attributes.type == process::PageAttributes::Image ? " image" : "");
  }
  console::puts("\n");
  uint8_t raw[1024] = {};      // what may be return addresses: the words on its stack that point into a module
  if (!task->program.address_space().read_privileged(rsp, ByteView(raw, sizeof raw)) && !task->program.address_space().read_privileged(rsp, ByteView(raw, 128))) return;
  uint32_t shown = 0;
  for (uint32_t at = 0; at + 8 <= sizeof raw && shown < 10; at += 8) {
    uint64_t w = 0;
    for (uint32_t k = 0; k < 8; ++k) w |= uint64_t{raw[at + k]} << (8 * k);
    bool inside = false;
    for (uint64_t i = 0; modules && i < modules->count() && !inside; ++i) inside = w > modules->at(i).base && w - modules->at(i).base < modules->at(i).size;
    if (!inside) continue;
    console::puts("    ");
    put_code_address(modules, w);
    console::puts("\n");
    ++shown;
  }
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
  // A page the program may use but has no entry for yet: a private page touched for the first time (it gets its memory now), an entry the tables do not have yet
  // because another thread or process is changing this program's memory, or a write to a copy-on-write page of a Windows program (the page gets a copy of its own
  // first). The entry is made and the instruction runs again; a fault at the same place a few times in a row is real.
  if ((frame->vector & 0xFF) == 14 && !(frame->error & 8) && page_fault_address < process::AddressSpace::kUserTop) {
    process::AddressSpace* space = task->program.process()->address_space();
    UserThread* self = current_user_thread();
    if (space && self) {
      const bool write = (frame->error & 2) != 0;
      const uint8_t need = (frame->error & 0x10) ? process::kProtExecute : write ? process::kProtWrite : process::kProtRead;
      const uint64_t page = process::AddressSpace::round_down(page_fault_address);
      bool again = false;
      irq_enable();
      task->mutex.lock();
      if (write && task->program.process()->abi() == process::SyscallAbi::Windows && !space->check(page, 1, process::kProtWrite)) space->break_copy_on_write(page, 1);      // (nothing unless the page is copy-on-write)
      if (space->check(page, 1, need) && space->materialize(page)) {
        const uint64_t had = task->tables.entry_of(page);
        again = task->sync_tables() && task->fault_in(page);
        // (an entry that was already what it is now did not help: the same fault coming back a few times means the program really cannot do this)
        const bool progress = task->tables.entry_of(page) != had;
        if (progress || self->stale_rip != frame->rip || self->stale_address != page_fault_address) { self->stale_rip = frame->rip; self->stale_address = page_fault_address; self->stale_count = 0; }
        if (!progress && ++self->stale_count > 3) again = false;
      }
      task->mutex.unlock();
      irq_disable();
      if (again) return frame;
    }
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
      if (delivered) log_exception(task, info.code, frame->rip, address, frame->rsp);      // (here: reading its stack may have to wait, and its memory may not change meanwhile)
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
  if (ut->start_context_set) context_to_frame(ut->saved, &frame);      // (registers set before it ran)
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
    process::win32k::install();
  }
  if (!process::image_cache()) {      // the pages of DLLs at their preferred base, shared by every program that maps them
    if (void* cache_memory = allocator.allocate(sizeof(process::ImageCache))) process::set_image_cache(new (cache_memory) process::ImageCache(allocator));
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
// Lock order: a program's own lock comes before the registry's (a system call holds its program's lock, and ending a child takes the registry's), so nothing
// waits for a program's lock while it holds the registry's: the program is pinned, the registry let go, and then its lock taken.
Mutex g_tasks_mutex;
Task* g_tasks = nullptr;

void unpin(Task* task) { __atomic_sub_fetch(&task->pins, 1u, __ATOMIC_ACQ_REL); }

// Pins the programs `keep` accepts (under the registry's lock: they cannot be destroyed until unpinned), up to the capacity of `out`.
template <typename Keep>
uint32_t pin_tasks(ArrayView<Task*> out, Keep keep) {
  uint32_t n = 0;
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task && n < out.size(); task = task->all_next) {
    if (!keep(*task)) continue;
    __atomic_add_fetch(&task->pins, 1u, __ATOMIC_ACQ_REL);
    out[n++] = task;
  }
  return n;
}

// The program with this id, pinned (unpin it), or null.
Task* pin_task(uint64_t pid) {
  Task* found[1] = {};
  pin_tasks(ArrayView<Task*>(found, 1), [pid](Task& t) { return t.program.loaded() && t.program.process()->id() == pid; });
  return found[0];
}
}  // namespace

void Task::set_name(StringView text) {
  uint32_t i = 0;
  for (; i + 1 < sizeof name && i < text.size(); ++i) name[i] = text[i];
  name[i] = 0;
}

Task::~Task() {
  if (registered) {
    {
      ScopedMutex hold(g_tasks_mutex);
      for (Task** link = &g_tasks; *link; link = &(*link)->all_next)
        if (*link == this) {
          *link = all_next;
          break;
        }
    }
    while (__atomic_load_n(&pins, __ATOMIC_ACQUIRE) != 0) sleep_ms(1);      // (whoever found it before it left the list finishes with it)
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
bool Task::park_wanted() {
  UserThread* ut = current_user_thread();
  return ut && ut->task == this && ut->syscall_frame && __atomic_load_n(&ut->suspend_requested, __ATOMIC_ACQUIRE);
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
    if (t->start_context_set) { *out = t->saved; return true; }
    *out = process::X86Context();
    out->rflags = 0x200;
    out->fpu[0] = 0x7F; out->fpu[1] = 0x02;      // the x87 control word, 0x27F
    out->fpu[24] = 0x80; out->fpu[25] = 0x1F;    // MXCSR, 0x1F80
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
    t->saved = context;      // (all of them: the thread starts with exactly these)
    t->start_context_set = true;
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
  program.finish_loading();      // (a process created suspended gets its imports and loader list when its first thread starts)
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
  // The program's threads run side by side on every core, each on the page tables it was given. The tables are changed in place, only where the memory layout
  // changed; when an entry a core may have cached went away or lost a permission (or the tables were made anew), every core running a thread of the program is
  // interrupted, and only after all of them went through the kernel (which reloads CR3: the TLB goes with it) is the memory that was let go of given back.
  process::AddressSpace& space = program.address_space();
  const uint64_t before = tables.root();
  bool flush = false;
  if (!tables.sync(space, true, &flush)) return false;
  const uint64_t root = tables.root();
  if (root == before && !flush && !space.has_retired()) return true;
  CoreMask others;
  for (UserThread* t = threads; t; t = t->next) {
    if (!t->spawned || !t->thread || t->joined || t->finished) continue;
    const int32_t core = set_thread_user_cr3(t->thread, root);      // (also says where the thread runs right now)
    if (core >= 0) others.add(static_cast<uint32_t>(core));
  }
  bool answered = true;
  if (!others.empty()) answered = wait_for_kernel_entry(others);
  if (answered) {
    tables.release_previous();
    space.reclaim();
  } else {
    console::puts("user program: a core did not give up the old page tables\n");      // (the old tables and memory are kept until the next change)
  }
  return true;
}

bool Task::fault_in(uint64_t page) {
  bool flush = false;
  if (!tables.update(program.address_space(), page, page + process::AddressSpace::kPageSize, &flush)) return false;
  if (!flush) return true;
  CoreMask others;      // (an entry was there and allowed less: the cores that may have it cached forget it)
  for (UserThread* t = threads; t; t = t->next) {
    if (!t->spawned || !t->thread || t->joined || t->finished) continue;
    const int32_t core = set_thread_user_cr3(t->thread, tables.root());
    if (core >= 0) others.add(static_cast<uint32_t>(core));
  }
  if (!others.empty()) wait_for_kernel_entry(others);
  return true;
}

bool Task::start_thread(const process::ThreadStart& start, bool suspended) {
  UserThread* t = add_thread(start);
  if (!t) return false;
  if (!sync_tables()) return false;      // the new thread's stack and TEB are in the tables before it runs
  return suspended || launch(t, kUserThreadKernelStack);
}

bool Task::start_linux_thread(const Frame& frame, const process::Process::LinuxThreadState::CloneRequest& clone) {
  // The new thread is a copy of the calling one right after the call: every register (glibc's clone3 keeps the function and its argument in registers),
  // 0 as the call's result, the stack it was given, and its own thread pointer when CLONE_SETTLS asked for one.
  Thread* me = current_thread();
  process::ThreadStart start;
  start.id = clone.tid;
  start.entry = frame.rip;
  start.stack_pointer = clone.stack ? clone.stack : frame.rsp;
  start.fs_base = clone.set_tls ? clone.tls : (me ? me->user_fs : 0);
  start.gs_base = me ? me->user_gs : 0;
  UserThread* t = add_thread(start);
  bool started = false;
  if (t) {
    frame_to_context(frame, &t->saved);
    t->saved.gpr[process::X86Context::kRax] = 0;
    t->saved.gpr[process::X86Context::kRsp] = start.stack_pointer;
    t->start_context_set = true;
    started = sync_tables() && launch(t, kUserThreadKernelStack);
  }
  if (!started)      // no thread after all: the table entry the call made goes back
    for (process::Process::LinuxThreadState::Thread& lt : program.process()->linux_threads().threads)
      if (lt.used && lt.tid == clone.tid) { lt.used = false; lt.clear_child_tid = 0; }
  return started;
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
  p.defer_loading(spec.suspended);
  p.set_command_line(spec.command_line);
  p.set_startup(spec.startup);
  if (!spec.executable_path.empty()) p.set_executable_path(spec.executable_path);
  // argv: the program's name, then (for a Linux program) the words of the command line; or exactly what execve was given
  StringView argv[32];
  uint32_t argc = 0;
  argv[argc++] = spec.image_path;
  if (!spec.arguments.empty()) {
    argc = 0;
  } else if (abi != process::SyscallAbi::Windows) {
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
  const ArrayView<const StringView> arguments = spec.arguments.empty() ? ArrayView<const StringView>(argv, argc) : spec.arguments;
  const process::LoadStatus loaded = p.load(spec.image, arguments, spec.environment, ConstByteView(random, sizeof(random)));
  if (loaded != process::LoadStatus::Ok) {
    discard();
    return loaded == process::LoadStatus::OutOfMemory ? kNoMemory : kInvalidImage;
  }
  auto count_use = [](process::DescriptorKind kind, uint64_t token) { process::descriptor_token_add(kind, token); };      // the child's table holds one more use of what the parent's does

  if (spec.linux_exec) {
    p.process()->files().inherit_for_exec(program.process()->files(), count_use);
  } else if (spec.inherit_handles) {
    p.process()->files().inherit_from(program.process()->files(), count_use, spec.inherit_list, spec.inherit_list_count, spec.restrict_inherit);
  }
  for (uint32_t i = 0; i < 3; ++i) {      // the standard handles that are copied into the child (a console child whose parent did not let it inherit)
    if (!spec.copy_std[i] || spec.std_handles[i] == 0 || spec.std_handles[i] % 4 != 0) continue;
    const uint64_t slot = spec.std_handles[i] / 4 - 1;
    if (slot < process::ProcessFiles::kMaxDescriptors) p.process()->files().adopt_from(program.process()->files(), slot, count_use);
  }
  if (spec.use_std_handles) p.set_standard_handles(spec.std_handles);
  for (uint32_t i = 0; i < 3; ++i) if (spec.console_std[i]) p.process()->files().set_handle_flags(i, true, false);      // (a pseudo console's handles can be inherited)
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
  Task* pinned[256];
  const uint32_t n = pin_tasks(ArrayView<Task*>(pinned, 256), [&self](Task& t) { return &t != &self; });
  for (uint32_t i = 0; i < n; ++i) {
    if (!found) {
      ScopedMutex hold(pinned[i]->mutex);
      for (Task::Child* c = pinned[i]->children; c; c = c->next) if (c->pid == pid) { found = true; result = action(c); break; }
    }
    unpin(pinned[i]);
  }
  self.mutex.lock();
  return found && result;
}

uint32_t Task::spawn_process_for(uint64_t parent_pid, const process::ChildSpec& spec) {
  if (parent_pid == program.process()->id()) return spawn_process(spec);
  mutex.unlock();      // (another program's lock is taken, and no lock of this one may be held meanwhile: see remote_operation)
  Task* target = pin_task(parent_pid);
  uint32_t status = 0xC000000Bu;      // STATUS_INVALID_CID
  if (target) {
    if (target != this) {
      ScopedMutex hold(target->mutex);
      if (!target->program.process()->exited()) status = target->spawn_process(spec);
    }
    unpin(target);
  }
  mutex.lock();
  return status;
}

bool Task::terminate_process(uint64_t pid, uint32_t code) {
  return with_child(*this, pid, [&](Child* c) { return terminate_child(c, code); });
}

bool Task::resume_process(uint64_t pid) {
  return with_child(*this, pid, [&](Child* c) { return resume_child(c); });
}

bool Task::query_child(uint64_t pid, bool* exited, uint32_t* code) {
  for (Child* c = children; c; c = c->next) {
    if (c->pid != pid) continue;
    process::Process* child = c->task->program.process();
    *exited = child->exited();
    *code = child->exit_code();
    return true;
  }
  return false;
}

bool Task::process_not_started(uint64_t pid) {
  return with_child(*this, pid, [&](Child* c) { return !c->started; });
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
  Task* task = pin_task(pid);
  if (!task) return kInvalidCid;
  uint32_t status = 0;
  {
    ScopedMutex hold(task->mutex);
    status = process::windowsabi::run_remote(*task->program.process(), op);
    // The layout may have changed: the page tables are rebuilt and given to the threads of that program (on whatever cores they run), as at the end of one of its own system calls.
    using Op = process::IProcessRuntime::RemoteOp;
    const bool layout = op.kind == Op::Allocate || op.kind == Op::Free || op.kind == Op::Protect || op.kind == Op::MapView || op.kind == Op::Unmap;      // (a view that came or went changes the layout too)
    if (layout && status == 0 && !task->sync_tables()) status = 0xC0000017u;
  }
  unpin(task);
  return status;
}

void report() {
  ScopedMutex registry(g_tasks_mutex);
  for (Task* task = g_tasks; task; task = task->all_next) {
    if (!task->program.loaded()) continue;
    bool locked = false;
    for (int i = 0; i < 100 && !(locked = task->mutex.try_lock()); ++i) sleep_ms(10);
    console::puts("report: program ");
    console::put_dec(int64_t(task->program.process()->id()));
    console::puts(" ");
    console::puts(task->name);
    console::puts(locked ? "" : " (its lock is held: what follows may be changing)");
    console::puts(", memory ");
    console::put_dec(int64_t(task->program.address_space().resident_bytes() >> 20));
    console::puts(" MiB resident");
    if (task->program.process()->exited()) { console::puts(", ended with "); console::put_hex(task->program.process()->exit_code()); }
    console::puts("\n");
    const process::ModuleSet* modules = task->program.modules();
    auto where = [&](uint64_t address) { return put_code_address(modules, address); };
    for (uint64_t i = 0; modules && i < modules->count(); ++i) {      // where its modules are (a library that had to move is not where the others have it)
      console::puts("  module ");
      console::puts(modules->at(i).name.view());
      console::puts(" at ");
      console::put_hex(modules->at(i).base);
      console::puts("\n");
    }
    for (UserThread* t = task->threads; t; t = t->next) {
      console::puts("  thread ");
      console::put_dec(int64_t(t->id >> 20));
      if (t->finished) { console::puts(" finished\n"); continue; }
      console::puts(t->in_call ? " in call " : " running, last seen at ");
      if (t->in_call) {
        console::put_hex(t->call_service);
        console::puts(" from ");
        where(t->call_rip);
      } else {
        where(t->tick_rip);
      }
      console::puts(", calls ");
      console::put_dec(int64_t(t->calls));
      console::puts("\n");
      if (t->in_call && t->call_rsp) {      // return addresses on its stack that point into a module
        uint8_t raw[2048] = {};
        if (!task->program.address_space().read_privileged(t->call_rsp, ByteView(raw, sizeof raw)) && !task->program.address_space().read_privileged(t->call_rsp, ByteView(raw, 256))) continue;
        uint32_t shown = 0;
        for (uint32_t at = 0; at + 8 <= sizeof raw && shown < 12; at += 8) {
          uint64_t w = 0;
          for (uint32_t k = 0; k < 8; ++k) w |= uint64_t{raw[at + k]} << (8 * k);
          bool in_module = false;
          for (uint64_t i = 0; modules && i < modules->count() && !in_module; ++i) in_module = w >= modules->at(i).base && w - modules->at(i).base < modules->at(i).size;
          if (!in_module) continue;
          console::puts("      ");
          where(w);
          console::puts("\n");
          ++shown;
        }
      }
    }
    if (locked) task->mutex.unlock();
  }
}

uint32_t thread_snapshot(uint64_t pid, process::IProcessRuntime::ThreadEntry* out, uint32_t capacity) {
  uint64_t ids[process::Process::kMaxThreads], starts[process::Process::kMaxThreads];
  int32_t priorities[process::Process::kMaxThreads];
  uint32_t n = 0;
  if (Task* task = pin_task(pid)) {
    {
      ScopedMutex hold(task->mutex);
      n = task->program.process()->thread_snapshot(ids, starts, priorities, process::Process::kMaxThreads);
    }
    unpin(task);
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
  Task* task = pin_task(pid);
  if (!task) return 0;
  {
    ScopedMutex hold(task->mutex);
    const process::ModuleSet* modules = task->program.modules();
    for (uint64_t i = 0; modules && i < modules->count() && n < capacity; ++i) {
      const process::Module& m = modules->at(i);
      process::IProcessRuntime::ModuleEntry& e = out[n++];
      e = process::IProcessRuntime::ModuleEntry();
      e.base = m.base; e.size = m.size; e.entry = m.entry; e.program = m.main;
      uint32_t k = 0;
      for (; k + 1 < sizeof e.name && k < m.name.size(); ++k) e.name[k] = m.name.view()[k];
      e.name[k] = 0;
    }
  }
  unpin(task);
  return n;
}

uint32_t process_snapshot(ProcessInfo* out, uint32_t capacity) {
  uint32_t n = 0;
  Task* pinned[256];
  const uint32_t count = pin_tasks(ArrayView<Task*>(pinned, capacity < 256 ? capacity : 256), [](Task& t) {
    return t.program.loaded() && !t.program.process()->exited();      // (one that has ended is gone from the list, whatever still holds its record)
  });
  for (Task* task : ArrayView<Task*>(pinned, count)) {
    ProcessInfo& info = out[n++];
    info = ProcessInfo();
    info.id = task->program.process()->id();
    info.parent_id = task->parent_id;
    info.create_time = task->program.process()->create_time;
    __builtin_memcpy(info.name, task->name, sizeof info.name);
    info.windows = task->program.process()->abi() == process::SyscallAbi::Windows;
    info.memory_bytes = task->program.address_space().resident_bytes();
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
    unpin(task);
  }
  return n;
}

bool Task::start_first_thread() {
  UserThread* t = first_thread;
  if (!t || t->spawned) return false;
  program.finish_loading();
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
