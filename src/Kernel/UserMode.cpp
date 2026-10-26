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

struct State {
  allocators::IAllocator* allocator = nullptr;
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

Task* current_task() {
  Thread* t = current_thread();
  return t ? static_cast<Task*>(t->user_context) : nullptr;
}

// SYSCALL from user mode (the stub made it look like an interrupt): the registers are in the frame.
Frame* on_syscall(Frame* frame, void*) {
  Task* task = current_task();
  if (!task) panic("system call from a thread that is not a user program", frame);
  process::Process* process = task->program.process();
  irq_enable();      // the frame is saved: the call may take as long as it needs and be preempted
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
  // The call may have changed the memory layout: rebuild the page tables and hand the new ones to this thread and core.
  if (alive) {
    if (!task->tables.sync(task->program.address_space())) {
      console::puts("user program: out of memory for page tables\n");
      process->terminate(0xFF);
      alive = false;
    } else if (task->tables.root() != current_thread()->user_cr3) {
      set_current_user_context(task->tables.root(), task);
    }
  }
  if (!alive) exit_thread();
  irq_disable();      // the way back is not re-entrant
  return frame;
}

// An exception in user mode ends that program only.
Frame* on_user_fault(Frame* frame) {
  Task* task = current_task();
  if (!task) panic("fault in user mode on a thread that is not a user program", frame);
  task->faulted = true;
  task->fault_vector = frame->vector & 0xFF;
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
  irq_enable();      // the frame is saved; ending the thread has to be able to switch away
  exit_thread();
  return frame;
}

// The thread that becomes the program: sets up its context and "returns" into ring 3 through a frame of the kind an interrupt leaves behind.
void user_main(void* argument) {
  Task* task = static_cast<Task*>(argument);
  const process::ProgramStart& start = task->program.start();
  Thread* me = current_thread();
  me->user_fs = start.fs_base;
  me->user_gs = start.gs_base;
  set_current_user_context(task->tables.root(), task);
  Frame frame;      // on this stack, which stays untouched until the CPU is in ring 3 (the next entry starts at the top of the stack)
  __asm__ volatile("fxsave %0" : "=m"(frame.fpu));
  frame.r15 = frame.r14 = frame.r13 = frame.r12 = frame.r11 = frame.r10 = frame.r9 = frame.r8 = 0;
  frame.rdi = frame.rsi = frame.rbp = frame.rbx = frame.rdx = frame.rax = 0;
  frame.rcx = start.argument;
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
    gate.type_attributes = 0x8E;      // present, ring 0 only, interrupt gate
    gate.offset_middle = static_cast<uint16_t>(stub >> 16);
    gate.offset_high = static_cast<uint32_t>(stub >> 32);
    gate.reserved = 0;
  }
  g.ranges[g.range_count++] = {address_of(g.code), g.code_bytes};
  g.ranges[g.range_count++] = {address_of(g.idt), kPageSize};
  set_interrupt_handler(kVectorSyscall, on_syscall, nullptr);
  set_user_fault_handler(on_user_fault);

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
  task->thread = kernel::spawn(user_main, task.get(), name);
  if (!task->thread) return UniquePointer<Task>();
  return task;
}

bool wait(Task& task, uint32_t* exit_code, uint64_t timeout_us) {
  if (!task.thread) return false;
  if (!join(task.thread, timeout_us)) return false;
  task.thread = nullptr;
  *exit_code = task.program.process()->exit_code();
  return true;
}

}  // namespace usermode
}  // namespace kernel

#endif
