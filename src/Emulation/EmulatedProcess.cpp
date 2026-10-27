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
#include "Process/Process.hpp"
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

 private:
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
      sync_.unlock();
      control_.yield();
      sync_.lock();
      continue;
    }
    if (reason == StopReason::Fault) {
      // A Windows program gets the fault as an exception on its own stack, to be handled by SEH or C++ code (or to end up in ntdll's last resort); only when that cannot be
      // done does it end.
      if (deliver_exception(cpu)) continue;
      record_fault(cpu.fault());
      break;
    }
    process::SyscallRequest request;
    uint64_t result = ~uint64_t{37};      // -ENOSYS
    if (cpu.pending_syscall(process_->abi(), &request)) result = process_->syscall(request);
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
    cpu.finish_syscall(result);
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
  sync_.lock();
  slots_[0].token = threads_ ? threads_->self() : 0;
  slots_[0].cpu->reset(program_.start());
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
