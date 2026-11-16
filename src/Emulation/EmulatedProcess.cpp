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
#include "Emulation/GuestCpu.hpp"
#include "Process/LinuxProcess.hpp"
#include "Process/Process.hpp"
#include "Process/UserProgram.hpp"
#include "Process/WindowsThreads.hpp"

namespace emulation {

namespace {

// One guest thread. The process lock is held while the guest runs and while its system calls run; it is let go between slices and while a call waits.
class EmulatedProcess final : public process::IProcessRuntime {
 public:
  static constexpr uint32_t kMaxSlots = 64;

  EmulatedProcess(process::UserProgram& program, IGuestCpu& cpu, IRunControl& control, process::ISystemSync& sync, IThreadSpawner* threads, allocators::IAllocator* allocator,
                  uint64_t slice)
      : program_(program), process_(program.process()), control_(control), sync_(sync), threads_(threads), allocator_(allocator), slice_(slice ? slice : 20000), memory_(program.address_space()) {
    slots_[0].used = true;
    slots_[0].cpu = &cpu;
    slots_[0].tid = process_->id();
    slots_[0].started = true;
    arch_ = cpu.arch();
  }

  EmulatedOutcome run();

  // process::IProcessRuntime
  void lock() override { sync_.lock(); }
  void unlock() override { sync_.unlock(); }
  bool wait(uint64_t timeout_ns) override { return sync_.wait(timeout_ns); }
  void notify() override { sync_.notify_all(); }
  bool interrupted() override { return control_.stop_requested(); }
  bool supports_linux_threads() override { return threads_ != nullptr && allocator_ != nullptr; }
  uint64_t current_thread_id() override {
    const uint64_t me = threads_ ? threads_->self() : 0;
    for (const Slot& s : slots_)
      if (s.used && !s.finished && s.token == me) return s.tid;
    return process_->id();
  }
  bool start_thread(const process::ThreadStart& start, bool suspended) override;
  bool resume_thread(uint64_t id) override;
  void request_terminate(uint64_t id, uint32_t code) override;
  uint32_t processor_count() override { return 1; }
  // fork(): the child is another emulated process, run on a thread of its own with a copy of the calling thread's CPU (see start_fork).
  bool supports_linux_fork() override { return threads_ != nullptr && allocator_ != nullptr; }
  uint64_t allocate_process_id() override {
    static uint32_t next = 0x5000;
    return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
  }
  bool query_child(uint64_t pid, bool* exited, uint32_t* code) override {
    for (const UniquePointer<ForkChild>& c : forks_) {
      if (!c || c->pid != pid) continue;
      *exited = c->program->process()->exited();
      *code = c->program->process()->exit_code();
      return true;
    }
    return false;
  }
  bool send_signal(uint64_t pid, uint32_t signal) override {      // (to a child of a fork, or from one to its parent)
    process::Process* target = nullptr;
    for (const UniquePointer<ForkChild>& c : forks_)
      if (c && c->pid == pid) target = c->program->process();
    if (!target && parent_ && parent_->process_->id() == pid) target = parent_->process_;
    if (!target || target->exited()) return false;
    target->post_linux_signal(signal);
    sync_.notify_all();
    return true;
  }
  uint32_t child_exit_signal(uint64_t pid) override {
    for (const UniquePointer<ForkChild>& c : forks_)
      if (c && c->pid == pid) return c->program->process()->linux_state().exit_signal;
    return 0;
  }
  bool process_exists(uint64_t pid) override {
    bool exited = false;
    uint32_t code = 0;
    if (parent_ && parent_->process_->id() == pid) return !parent_->process_->exited();      // (the program that forked this one)
    return pid == process_->id() || (query_child(pid, &exited, &code) && !exited);
  }
  bool terminate_process(uint64_t pid, uint32_t code) override {
    for (const UniquePointer<ForkChild>& c : forks_) {
      if (!c || c->pid != pid || c->program->process()->exited()) continue;
      c->program->process()->terminate(code);
      sync_.notify_all();
      return true;
    }
    return false;
  }

 private:
  // A process made by fork(): its program (a copy of this one), the CPU its one thread starts with, and the runtime that runs it on a thread of the host.
  struct ForkChild {
    UniquePointer<process::UserProgram> program;
    UniquePointer<IGuestCpu> cpu;
    UniquePointer<EmulatedProcess> runtime;
    void* handle = nullptr;
    uint64_t pid = 0;
    bool done = false;      // its run() has returned (set with the lock held)
  };
  static constexpr uint32_t kMaxForks = 32;
  static void fork_entry(void* child) {
    ForkChild* c = static_cast<ForkChild*>(child);
    c->runtime->run();
    c->runtime->sync_.lock();
    if (c->runtime->parent_) c->runtime->parent_->process_->post_linux_signal(17);      // SIGCHLD
    c->done = true;
    c->runtime->sync_.notify_all();
    c->runtime->sync_.unlock();
  }
  bool start_fork(const IGuestCpu& cpu, const process::LinuxProcessState::ForkRequest& request);
  // Does this runtime enter the handlers of the program's signals? (A Linux program's, on the CPUs whose frames linuxabi::deliver_signals knows.)
  bool enters_handlers() const {
    return process_->abi() == process::SyscallAbi::Linux &&
           (arch_ == process::UserArch::X86_64 || arch_ == process::UserArch::AArch64 || arch_ == process::UserArch::RiscV64);
  }
  // The registers of a guest thread in the form the signal code wants for its CPU, and back.
  struct SignalRegisters {
    bool x86 = false;
    process::X86Context context;
    process::linuxabi::LinuxSignalContext other;
  };
  bool get_registers(const IGuestCpu& cpu, SignalRegisters* out) const {
    out->x86 = arch_ == process::UserArch::X86_64;
    return out->x86 ? cpu.get_x86_context(&out->context) : cpu.get_signal_context(&out->other);
  }
  static bool set_registers(IGuestCpu& cpu, const SignalRegisters& r) { return r.x86 ? cpu.set_x86_context(r.context) : cpu.set_signal_context(r.other); }
  // The guest thread on `cpu` is about to run on: the signals that wait for it are acted on, which may point the CPU at a handler. `call`: the system call it
  // returns from, or null (between two slices).
  void linux_signals(IGuestCpu& cpu, const process::linuxabi::InterruptedCall* call) {
    if (!process::linuxabi::signals_wanted(*process_)) return;
    SignalRegisters r;
    if (!get_registers(cpu, &r)) return;
    const bool entered = r.x86 ? process::linuxabi::deliver_signals(*process_, &r.context, call) : process::linuxabi::deliver_signals(*process_, &r.other, call);
    if (entered) set_registers(cpu, r);
    if (process_->exited()) sync_.notify_all();
  }
  // rt_sigreturn: the registers the handler interrupted come back from its frame. False if the frame is gone (the program then ends by SIGSEGV).
  bool linux_signal_return(IGuestCpu& cpu) {
    SignalRegisters r;
    if (!get_registers(cpu, &r)) return false;
    const bool restored = r.x86 ? process::linuxabi::signal_return(*process_, &r.context) : process::linuxabi::signal_return(*process_, &r.other);
    return restored && set_registers(cpu, r);
  }
  // A fault of a Linux program as a signal: true if its handler was entered (the CPU is there now).
  bool deliver_signal_fault(IGuestCpu& cpu) {
    if (!enters_handlers()) return false;
    SignalRegisters r;
    if (!get_registers(cpu, &r)) return false;
    const GuestFault& fault = cpu.fault();
    bool entered = false;
    if (r.x86) {
      r.context.rip = fault.vector == 3 ? fault.pc + 1 : fault.pc;      // (an x86 breakpoint is reported after the INT3)
      entered = process::linuxabi::deliver_fault(*process_, &r.context, fault.vector, fault.error, fault.address);
    } else {
      r.other.pc = fault.pc;
      entered = process::linuxabi::deliver_fault(*process_, &r.other, fault.vector, fault.error, fault.address);
    }
    return entered && set_registers(cpu, r);
  }
  // Ends the children that still run and waits for every child's thread. Called without the lock.
  void finish_forks();

  struct Slot {
    bool used = false, started = false, finished = false, terminate = false;
    uint32_t terminate_code = 0;
    uint64_t tid = 0, token = 0;
    void* handle = nullptr;
    IGuestCpu* cpu = nullptr;
    UniquePointer<IGuestCpu> owned;
    EmulatedProcess* owner = nullptr;
  };

  static void thread_entry(void* slot) {
    Slot* s = static_cast<Slot*>(slot);
    s->owner->thread_loop(*s, false);
  }
  Slot* free_slot() {
    for (uint32_t i = 1; i < kMaxSlots; ++i) {      // (slot 0 is the first thread)
      Slot& s = slots_[i];
      if (s.used && s.finished) {      // an ended thread: let go of its system thread, and the slot can be used again
        if (s.handle && threads_) threads_->join(s.handle);
        s = Slot();
      }
    }
    for (Slot& s : slots_)
      if (!s.used) return &s;
    return nullptr;
  }
  bool launch(Slot& slot);
  void thread_loop(Slot& slot, bool main);
  // Turns the fault the CPU just reported into a Windows exception: the CONTEXT and EXCEPTION_RECORD go on the thread's stack and it continues in ntdll's
  // KiUserExceptionDispatcher. False if this is not a Windows program with a dispatcher, or the exception cannot be delivered (no stack left).
  bool deliver_exception(IGuestCpu& cpu) {
    if (process_->abi() != process::SyscallAbi::Windows || process_->exception_dispatcher() == 0) return false;
    process::X86Context at, entry;
    if (!cpu.get_x86_context(&at)) return false;
    const GuestFault& fault = cpu.fault();
    if (fault.vector == 3) at.rip = fault.pc + 1;      // (the breakpoint is reported after the INT3, as the CPU does; fault_to_exception moves it back)
    process::ExceptionInfo info;
    if (!process::fault_to_exception(fault.vector, fault.error, fault.address, &at, &info)) return false;
    if (!process::dispatch_exception(*process_, at, info, process_->exception_dispatcher(), &entry)) return false;
    return cpu.set_x86_context(entry);
  }
  void record_fault(const GuestFault& fault) {
    if (!outcome_.faulted) {
      outcome_.faulted = true;
      outcome_.fault = fault;
    }
    process_->terminate(128u + fault.vector);
    sync_.notify_all();
  }

  process::UserProgram& program_;
  process::Process* process_;
  IRunControl& control_;
  process::ISystemSync& sync_;
  IThreadSpawner* threads_;
  allocators::IAllocator* allocator_;
  uint64_t slice_;
  GuestMemory memory_;
  process::UserArch arch_ = process::UserArch::None;
  Slot slots_[kMaxSlots];
  UniquePointer<ForkChild> forks_[kMaxForks];
  EmulatedProcess* parent_ = nullptr;      // the runtime of the program that forked this one (it outlives its children: finish_forks)
  bool forked_ = false;      // this is the child of a fork: its first thread's CPU is a copy of its parent's thread, not a program at its entry
  uint32_t live_ = 0;
  EmulatedOutcome outcome_;
};

bool EmulatedProcess::launch(Slot& slot) {
  slot.owner = this;
  slot.started = true;
  ++live_;
  slot.handle = threads_ ? threads_->spawn(&EmulatedProcess::thread_entry, &slot, "guest-thread") : nullptr;
  if (!slot.handle) {
    --live_;
    slot.started = false;
    return false;
  }
  return true;
}

bool EmulatedProcess::start_fork(const IGuestCpu& cpu, const process::LinuxProcessState::ForkRequest& request) {
  if (!threads_ || !allocator_) return false;
  UniquePointer<ForkChild>* place = nullptr;
  for (UniquePointer<ForkChild>& c : forks_) {
    // (a child that has ended and been waited for is still asked about by nobody: its place stays taken until the program ends, which bounds a program to kMaxForks forks)
    if (!c && !place) place = &c;
  }
  if (!place) return false;
  UniquePointer<ForkChild> child = UniquePointer<ForkChild>::make(*allocator_);
  if (!child) return false;
  child->pid = request.pid;
  child->program = UniquePointer<process::UserProgram>::make(*allocator_, *allocator_, request.pid, program_.host());
  if (!child->program || !child->program->fork_from(program_, request, process_->fs_base())) return false;
  child->cpu = cpu.clone_for_thread(*allocator_, request.stack, request.set_tls, request.tls);      // (the result register is 0 in the copy)
  if (!child->cpu) return false;
  child->runtime = UniquePointer<EmulatedProcess>::make(*allocator_, *child->program, *child->cpu, control_, sync_, threads_, allocator_, slice_);
  if (!child->runtime) return false;
  child->runtime->forked_ = true;
  child->runtime->parent_ = this;
  child->handle = threads_->spawn(&EmulatedProcess::fork_entry, child.get(), "guest-fork");
  if (!child->handle) return false;
  *place = static_cast<UniquePointer<ForkChild>&&>(child);
  return true;
}

void EmulatedProcess::finish_forks() {
  for (UniquePointer<ForkChild>& c : forks_) {
    if (!c) continue;
    sync_.lock();
    if (!c->program->process()->exited()) c->program->process()->terminate(137);      // (a child does not outlive the emulated program that made it)
    sync_.notify_all();
    sync_.unlock();
    threads_->join(c->handle);
    c = UniquePointer<ForkChild>();
  }
}

bool EmulatedProcess::start_thread(const process::ThreadStart& start, bool suspended) {
  if (!threads_ || !allocator_) return false;
  Slot* slot = free_slot();
  if (!slot) return false;
  UniquePointer<IGuestCpu> cpu = create_guest_cpu(arch_, *allocator_);
  if (!cpu) return false;
  process::ProgramStart ps;
  ps.arch = arch_;
  ps.entry = start.entry;
  ps.stack_pointer = start.stack_pointer;
  ps.argument = start.argument;
  ps.argument2 = start.argument2;
  ps.fs_base = start.fs_base;
  ps.gs_base = start.gs_base;
  cpu->reset(ps);
  *slot = Slot();
  slot->used = true;
  slot->tid = start.id;
  slot->owned = static_cast<UniquePointer<IGuestCpu>&&>(cpu);
  slot->cpu = slot->owned.get();
  if (!suspended && !launch(*slot)) {
    *slot = Slot();
    return false;
  }
  return true;
}

bool EmulatedProcess::resume_thread(uint64_t id) {
  for (Slot& s : slots_) {
    if (!s.used || s.tid != id) continue;
    return s.started || launch(s);
  }
  return false;
}

void EmulatedProcess::request_terminate(uint64_t id, uint32_t code) {
  for (Slot& s : slots_) {
    if (s.used && !s.finished && s.tid == id) {
      s.terminate = true;
      s.terminate_code = code;
    }
  }
  sync_.notify_all();
}

// Runs one guest thread to its end. Called with the lock not held; returns the same way.
void EmulatedProcess::thread_loop(Slot& slot, bool main) {
  (void)main;
  sync_.lock();
  slot.token = threads_ ? threads_->self() : 0;
  IGuestCpu& cpu = *slot.cpu;
  while (!process_->exited()) {
    if (control_.stop_requested()) {
      outcome_.killed = true;
      process_->terminate(130);
      sync_.notify_all();
      break;
    }
    if (slot.terminate) {      // another thread asked this one to end (NtTerminateThread)
      if (process::Process::ThreadRecord* record = process_->find_thread(slot.tid)) process::windowsabi::end_thread(*process_, *record, slot.terminate_code);
      break;
    }
    const StopReason reason = cpu.run(memory_, slice_);
    if (reason == StopReason::Budget) {
      if (enters_handlers()) linux_signals(cpu, nullptr);      // (a timer that ran out, another process's kill: as a real kernel would at a timer tick)
      sync_.unlock();
      control_.yield();
      sync_.lock();
      continue;
    }
    if (reason == StopReason::Fault) {
      // A Windows program gets the fault as an exception on its own stack, to be handled by SEH or C++ code (or to end up in ntdll's last resort); only when that cannot be
      // done does it end.
      if (deliver_exception(cpu)) continue;
      if (deliver_signal_fault(cpu)) continue;      // (SIGSEGV and the like, to the program's own handler)
      record_fault(cpu.fault());
      break;
    }
    process::SyscallRequest request;
    uint64_t result = ~uint64_t{37};      // -ENOSYS
    if (cpu.pending_syscall(process_->abi(), &request)) {
      control_.on_syscall(slot.tid, request, false, 0);
      result = process_->syscall(request);
      control_.on_syscall(slot.tid, request, true, result);
    }
    process::Process::LinuxThreadState::CloneRequest clone;
    if (process_->take_clone(&clone)) {      // clone(): the child starts as a copy of this thread, right after the call, with 0 as the result
      Slot* child = (threads_ && allocator_) ? free_slot() : nullptr;
      UniquePointer<IGuestCpu> copy = child ? cpu.clone_for_thread(*allocator_, clone.stack, clone.set_tls, clone.tls) : UniquePointer<IGuestCpu>();
      bool started = false;
      if (child && copy) {
        *child = Slot();
        child->used = true;
        child->tid = clone.tid;
        child->owned = static_cast<UniquePointer<IGuestCpu>&&>(copy);
        child->cpu = child->owned.get();
        started = launch(*child);
        if (!started) *child = Slot();
      }
      if (!started) {      // no thread after all: give the table entry back and fail the call
        for (process::Process::LinuxThreadState::Thread& t : process_->linux_threads().threads)
          if (t.used && t.tid == clone.tid) { t.used = false; t.clear_child_tid = 0; }
        result = uint64_t(-int64_t{11});      // -EAGAIN
      }
    }
    process::LinuxProcessState::ForkRequest fork;
    if (process_->take_fork(&fork) && !start_fork(cpu, fork)) {      // fork(): the child is a copy of the program as it is at the end of the call
      process::linuxabi::fork_failed(*process_, fork.pid);
      result = uint64_t(-int64_t{11});      // -EAGAIN
    }
    cpu.finish_syscall(result);
    if (enters_handlers() && !process_->exited()) {
      // rt_sigreturn: the registers the handler interrupted come back from its frame. Then, for any call: the signals that wait for this thread.
      const bool returned = process_->take_signal_return();
      if (returned && !linux_signal_return(cpu)) {      // the frame is gone: SIGSEGV
        process_->linux_state().exit_signal = 11;
        process_->terminate(128 + 11);
        sync_.notify_all();
      }
      const process::linuxabi::InterruptedCall call{request.number, process::linuxabi::call_restarts(arch_, request), request.args[0]};
      if (!process_->exited()) linux_signals(cpu, returned ? nullptr : &call);
    }
    uint64_t fs = 0, gs = 0;
    const bool fs_changed = process_->take_fs_change(&fs), gs_changed = process_->take_gs_change(&gs);
    if (fs_changed || gs_changed) cpu.set_thread_pointers(fs_changed, fs, gs_changed, gs);      // arch_prctl
    uint32_t thread_code = 0;
    if (!process_->exited() && process_->take_thread_exit(&thread_code)) break;      // this thread is done (the exit call or NtTerminateThread dealt with the rest)
    uint64_t context_at = 0;
    if (!process_->exited() && process_->take_context_restore(&context_at)) {      // NtContinue: load the CONTEXT into the registers
      process::X86Context context;
      if (cpu.get_x86_context(&context) && process::apply_context(*process_, context_at, &context) && cpu.set_x86_context(context)) continue;
      process_->terminate(process::kStatusAccessViolation);
      sync_.notify_all();
      break;
    }
  }
  slot.finished = true;
  --live_;
  sync_.notify_all();
  sync_.unlock();
}

EmulatedOutcome EmulatedProcess::run() {
  if (!process_) {
    outcome_.faulted = true;
    outcome_.fault.vector = 6;
    return outcome_;
  }
  slots_[0].owner = this;
  slots_[0].used = true;
  live_ = 1;
  process_->attach_runtime(this);
  if (enters_handlers()) process_->linux_state().handlers_run = true;
  sync_.lock();
  slots_[0].token = threads_ ? threads_->self() : 0;
  if (!forked_) slots_[0].cpu->reset(program_.start());
  sync_.unlock();
  thread_loop(slots_[0], true);
  // The first thread is done; the others may still run. Wait for them, polling so that a stop request reaches them.
  sync_.lock();
  while (live_ > 0) {
    if (control_.stop_requested() && !process_->exited()) {
      outcome_.killed = true;
      process_->terminate(130);
    }
    sync_.wait(20000000);
  }
  if (!process_->exited()) process_->terminate(0);
  const uint32_t code = process_->exit_code();
  sync_.unlock();
  for (Slot& s : slots_) {
    if (s.handle && threads_) threads_->join(s.handle);
    s.handle = nullptr;
  }
  finish_forks();
  process_->attach_runtime(nullptr);
  process_->release_handles();
  outcome_.exited = !outcome_.killed && !outcome_.faulted;
  outcome_.exit_code = code;
  return outcome_;
}

// A process lock for a program that has one thread: nothing else could contend for it, and waiting is only sleeping.
class SingleThreadSync final : public process::ISystemSync {
 public:
  explicit SingleThreadSync(process::IProcessHost& host) : host_(host) {}
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t timeout_ns) override {
    host_.sleep_ns(timeout_ns < 1000000 ? timeout_ns : 1000000);      // (nobody can notify: sleep a little and let the caller look again)
    return true;
  }
  void notify_all() override {}

 private:
  process::IProcessHost& host_;
};

}  // namespace

EmulatedOutcome run_emulated_threads(process::UserProgram& program, IGuestCpu& cpu, IRunControl& control, process::ISystemSync& sync, IThreadSpawner& threads,
                                     allocators::IAllocator& allocator, uint64_t slice) {
  EmulatedProcess runtime(program, cpu, control, sync, &threads, &allocator, slice);
  return runtime.run();
}

EmulatedOutcome run_emulated(process::UserProgram& program, IGuestCpu& cpu, IRunControl& control, uint64_t slice) {
  SingleThreadSync sync(program.host());
  EmulatedProcess runtime(program, cpu, control, sync, nullptr, nullptr, slice);
  return runtime.run();
}

}  // namespace emulation
