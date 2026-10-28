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
#include "Process/WindowsThreads.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsException.hpp"
#include "Process/WindowsStartup.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kAlerted = 0x101, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kAccessViolation = 0xC0000005u,
                   kNotImplemented = 0xC0000002u, kObjectTypeMismatch = 0xC0000024u, kInsufficientResources = 0xC000009Au, kInfoLengthMismatch = 0xC0000004u,
                   kInvalidInfoClass = 0xC0000003u;
constexpr uint64_t kCurrentThread = ~uint64_t{1};      // NtCurrentThread() = (HANDLE)-2
constexpr uint32_t kCreateSuspended = 1;
constexpr uint32_t kStillActive = 259;

// The thread a handle names: the pseudo handle of the calling thread, or a thread descriptor. Null if it is neither.
Process::ThreadRecord* thread_of(Process& p, uint64_t handle) {
  if (handle == kCurrentThread || handle == 0) return p.runtime() ? p.find_thread(p.runtime()->current_thread_id()) : p.find_thread(p.id());
  if (handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return nullptr;
  const uint64_t slot = handle / 4 - 1;
  if (p.files().kind(slot) != DescriptorKind::Thread) return nullptr;
  return p.find_thread(p.files().token(slot));
}

constexpr uint64_t kRemoteThreadIdLimit = uint64_t{1} << 20;      // thread ids are (counter << 20) | process id: a process object whose owner is bigger stands for a thread

bool put32(Process& p, uint64_t address, uint32_t v) {
  const uint8_t raw[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  return p.write_user(address, ConstByteView(raw, 4));
}

}  // namespace

bool remote_thread_handle(Process& p, uint64_t handle, uint64_t* thread_id) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  const uint64_t slot = handle / 4 - 1;
  ObjectManager* objects = system_objects();
  if (!objects || p.files().kind(slot) != DescriptorKind::Process) return false;
  bool exited = false;
  uint32_t code = 0;
  uint64_t owner = 0;
  if (!objects->query_process(uint32_t(p.files().token(slot)), &exited, &code, &owner) || owner < kRemoteThreadIdLimit) return false;
  *thread_id = owner;
  return true;
}

uint32_t create_thread_in(Process& target, uint64_t start, uint64_t argument_value, bool suspended, uint64_t stack_size, uint64_t* thread_id) {
  IProcessRuntime* runtime = target.runtime();
  AddressSpace* space = target.address_space();
  const Process::ThreadSupport& support = target.thread_support();
  if (!runtime || !space || support.start_thunk == 0) return kNotImplemented;      // not a Windows program with threads
  if (start == 0 || start >= 0x0000800000000000ull) return kInvalidParameter;
  Process::ThreadRecord* record = target.add_thread(target.next_thread_id());
  if (!record) return kInsufficientResources;
  const uint64_t id = record->id;
  WindowsThreadInput in;
  in.peb = support.peb;
  in.process_id = target.id();
  in.thread_id = id;
  in.stack_bytes = stack_size ? stack_size : support.stack_bytes;
  for (uint32_t i = 0; i < support.tls_count; ++i) in.tls[i] = support.tls[i];
  in.tls_count = support.tls_count;
  WindowsThread thread;
  if (build_windows_thread(*space, in, &thread) != LoadStatus::Ok) {
    record->used = false;
    return kInsufficientResources;
  }
  record->teb = thread.teb;
  record->start_address = start;
  record->region_base = thread.region_base;
  record->region_size = thread.region_size;
  ThreadStart ts;
  ts.entry = support.start_thunk;
  ts.argument = start;
  ts.argument2 = argument_value;
  ts.stack_pointer = thread.stack_pointer;
  ts.gs_base = thread.teb;
  ts.id = id;
  if (!runtime->start_thread(ts, suspended)) {
    space->unmap(thread.region_base, thread.region_size);
    record->used = false;
    return kInsufficientResources;
  }
  record->started = !suspended;
  record->suspend_count = suspended ? 1 : 0;
  *thread_id = id;
  return kSuccess;
}

uint32_t thread_status_in(Process& target, uint64_t thread_id, uint64_t* exit_code) {
  Process::ThreadRecord* record = target.find_thread(thread_id);
  if (!record) { *exit_code = kStillActive + 1; return kInvalidHandle; }
  *exit_code = record->finished ? record->exit_code : kStillActive;
  return kSuccess;
}

// The registers of a thread of `target` that has not run yet (or is parked), for GetContext/SetContext.
uint32_t context_in(Process& target, uint64_t thread_id, bool set, X86Context* context) {
  Process::ThreadRecord* record = target.find_thread(thread_id);
  IProcessRuntime* runtime = target.runtime();
  if (!record || !runtime || !context) return kInvalidHandle;
  if (record->finished) return 0xC000004Bu;      // STATUS_THREAD_IS_TERMINATING
  if (record->suspend_count == 0) return 0xC000000Du;
  if (record->started && !runtime->is_parked(record->id)) return 0xC0000001u;
  if (set) return runtime->set_context(record->id, *context) ? kSuccess : 0xC0000001u;
  return runtime->get_context(record->id, context) ? kSuccess : 0xC0000001u;
}

uint32_t terminate_thread_in(Process& target, uint64_t thread_id, uint32_t code) {
  Process::ThreadRecord* record = target.find_thread(thread_id);
  if (!record) return kInvalidHandle;
  if (record->finished) return kSuccess;
  if (!record->started) { end_thread(target, *record, code); return kSuccess; }
  if (!target.runtime()) return kNotImplemented;
  target.runtime()->request_terminate(record->id, code);
  if (ObjectManager* objects = system_objects()) objects->notify();
  return kSuccess;
}

uint32_t resume_thread_in(Process& target, uint64_t thread_id, uint64_t* previous_count) {
  Process::ThreadRecord* record = target.find_thread(thread_id);
  if (!record || !target.runtime()) return kInvalidHandle;
  *previous_count = uint32_t(record->suspend_count);
  if (record->suspend_count > 0 && !record->finished) {
    if (--record->suspend_count == 0) {
      if (!record->started) {
        if (!target.runtime()->resume_thread(record->id)) { ++record->suspend_count; return kInsufficientResources; }
        record->started = true;
      } else {
        target.runtime()->request_resume(record->id);
      }
    }
  }
  return kSuccess;
}

// NtCreateThreadEx(PHANDLE Thread, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE Process, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits,
//                  SIZE_T StackSize, SIZE_T MaximumStackSize, PVOID AttributeList)
uint32_t nt_create_thread_ex(Process& p, const SyscallRequest& r) {
  uint64_t start = 0, argument_value = 0, flags = 0, stack_size = 0, max_stack = 0;
  if (!windowsabi::argument(p, r, 4, &start) || !windowsabi::argument(p, r, 5, &argument_value) || !windowsabi::argument(p, r, 6, &flags) || !windowsabi::argument(p, r, 8, &stack_size) ||
      !windowsabi::argument(p, r, 9, &max_stack))
    return kAccessViolation;
  IProcessRuntime* runtime = p.runtime();
  AddressSpace* space = p.address_space();
  const Process::ThreadSupport& support = p.thread_support();
  if (!runtime || !space || support.start_thunk == 0) return kNotImplemented;      // a process of one thread
  if (p.threads_terminated() && r.args[3] == ~uint64_t{0}) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING
  if (r.args[3] != ~uint64_t{0}) {      // a thread in another program: the handle names it by its id and can be waited for
    bool exited = false;
    uint32_t code = 0;
    uint64_t pid = 0;
    ObjectManager* objects = system_objects();
    if (!objects || !process_handle_info(p, r.args[3], &exited, &code, &pid)) return kInvalidHandle;
    if (pid == p.id() && p.threads_terminated()) return 0xC0000022u;      // STATUS_ACCESS_DENIED (the program is ending)
    if (pid != p.id()) {
      if (exited) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING
      IProcessRuntime::RemoteOp op;
      op.kind = IProcessRuntime::RemoteOp::CreateThread;
      op.address = start; op.argument = argument_value; op.suspended = (flags & kCreateSuspended) != 0; op.stack_size = stack_size ? stack_size : max_stack;
      const uint32_t status = runtime->remote_operation(pid, op);
      if (status != kSuccess) return status;
      const ObjectManager::Created c = objects->create_process(op.thread_id);      // (an object that is signalled when the thread ends; see ObjectManager::process_exited)
      if (c.id == 0) return c.status;
      const int64_t slot = p.files().open_token(DescriptorKind::Process, c.id);
      if (slot < 0) { objects->release(c.id); return kInsufficientResources; }
      if (!p.write_user_u64(r.args[0], (uint64_t(slot) + 1) * 4)) { p.close_descriptor(uint64_t(slot)); return kAccessViolation; }
      IProcessRuntime::RemoteOp check;      // (it may have ended before the object existed)
      check.kind = IProcessRuntime::RemoteOp::ThreadStatus;
      check.thread_id = op.thread_id;
      if (runtime->remote_operation(pid, check) == kSuccess && check.size != kStillActive) objects->process_exited(op.thread_id, uint32_t(check.size));
      return kSuccess;
    }
  }
  if (start == 0 || start >= 0x0000800000000000ull) return kInvalidParameter;
  Process::ThreadRecord* record = p.add_thread(p.next_thread_id());
  if (!record) return kInsufficientResources;
  const uint64_t id = record->id;
  WindowsThreadInput in;
  in.peb = support.peb;
  in.process_id = p.id();
  in.thread_id = id;
  in.stack_bytes = stack_size ? stack_size : max_stack ? max_stack : support.stack_bytes;
  for (uint32_t i = 0; i < support.tls_count; ++i) in.tls[i] = support.tls[i];
  in.tls_count = support.tls_count;
  WindowsThread thread;
  if (build_windows_thread(*space, in, &thread) != LoadStatus::Ok) {
    record->used = false;
    return kInsufficientResources;
  }
  const int64_t slot = p.files().open_token(DescriptorKind::Thread, id);
  const uint64_t handle = slot < 0 ? 0 : (uint64_t(slot) + 1) * 4;
  if (slot < 0 || !p.write_user_u64(r.args[0], handle)) {
    if (slot >= 0) p.files().close(uint64_t(slot));
    space->unmap(thread.region_base, thread.region_size);
    record->used = false;
    return slot < 0 ? kInsufficientResources : kAccessViolation;
  }
  record->teb = thread.teb;
  record->start_address = start;
  record->region_base = thread.region_base;
  record->region_size = thread.region_size;
  ThreadStart ts;
  ts.entry = support.start_thunk;
  ts.argument = start;
  ts.argument2 = argument_value;
  ts.stack_pointer = thread.stack_pointer;
  ts.gs_base = thread.teb;
  ts.id = id;
  const bool suspended = (flags & kCreateSuspended) != 0;
  if (!runtime->start_thread(ts, suspended)) {
    p.files().close(uint64_t(slot));
    space->unmap(thread.region_base, thread.region_size);
    record->used = false;
    return kInsufficientResources;
  }
  record->started = !suspended;
  record->suspend_count = suspended ? 1 : 0;
  return kSuccess;
}

// A thread has ended (itself, or because it was told to): its stack, TEB and TLS block go, its mutexes are abandoned, whoever waits for it looks again, and the last thread
// takes the process with it.
void end_thread(Process& p, Process::ThreadRecord& record, uint32_t code) {
  record.finished = true;
  record.exit_code = code;
  if (record.region_size && p.address_space()) {
    p.address_space()->unmap(record.region_base, record.region_size);
    record.region_size = 0;
  }
  for (uint32_t i = 0; i < kMaxTlsModules; ++i) {      // thread-local blocks of libraries loaded after the thread began
    if (record.extra_tls[i] != 0 && p.address_space()) p.address_space()->unmap(record.extra_tls[i], record.extra_tls_size[i]);
    record.extra_tls[i] = 0;
    record.extra_tls_size[i] = 0;
  }
  if (ObjectManager* objects = system_objects()) { objects->abandon_thread(record.id); objects->process_exited(record.id, code); }      // (the second: a handle to the thread from another program)
  if (p.live_threads() == 0) p.terminate(code);
  if (p.runtime()) p.runtime()->notify();
  if (ObjectManager* objects = system_objects()) objects->notify();
}

// NtTerminateThread(HANDLE Thread, NTSTATUS ExitStatus): the calling thread ends at once; another one is told to end (it does at its next system call or timer tick).
uint32_t nt_terminate_thread(Process& p, const SyscallRequest& r) {
  uint64_t remote_id = 0;
  if (remote_thread_handle(p, r.args[0], &remote_id)) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::TerminateThread;
    op.thread_id = remote_id; op.size = uint32_t(r.args[1]);
    return p.runtime() ? p.runtime()->remote_operation(remote_id & (kRemoteThreadIdLimit - 1), op) : kNotImplemented;
  }
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  const uint64_t self = p.runtime() ? p.runtime()->current_thread_id() : p.id();
  const uint32_t code = uint32_t(r.args[1]);
  if (record->id == self) {
    end_thread(p, *record, code);
    p.request_thread_exit(code);      // (the thread never returns to user mode)
    return kSuccess;
  }
  if (record->finished) return kSuccess;
  if (!record->started) {      // it never ran: nothing to stop
    end_thread(p, *record, code);
    return kSuccess;
  }
  if (!p.runtime()) return kNotImplemented;
  p.runtime()->request_terminate(record->id, code);
  if (ObjectManager* objects = system_objects()) objects->notify();      // (a thread waiting inside the kernel looks again and finds it is to end)
  return kSuccess;
}

uint32_t terminate_other_threads(Process& p, uint32_t code) {
  if (!p.runtime()) return kNotImplemented;
  const uint64_t self = p.runtime()->current_thread_id();
  p.set_threads_terminated();
  for (uint32_t i = 0; i < Process::kMaxThreads; ++i) {
    Process::ThreadRecord* record = p.thread_at(i);
    if (!record || !record->used || record->finished || record->id == self) continue;
    if (!record->started) { end_thread(p, *record, code); continue; }
    p.runtime()->request_terminate(record->id, code);
  }
  if (ObjectManager* objects = system_objects()) objects->notify();
  return kSuccess;
}

// NtSuspendThread(HANDLE Thread, PULONG PreviousSuspendCount)
uint32_t nt_suspend_thread(Process& p, const SyscallRequest& r) {
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  if (record->finished) return 0xC000004Bu;      // STATUS_THREAD_IS_TERMINATING
  if (record->suspend_count >= 127) return 0xC000004Cu;      // STATUS_SUSPEND_COUNT_EXCEEDED
  const uint32_t previous = uint32_t(record->suspend_count++);
  if (previous == 0 && record->started && p.runtime()) {
    p.runtime()->request_suspend(record->id);
    if (ObjectManager* objects = system_objects()) objects->notify();
  }
  if (r.args[1] != 0 && !put32(p, r.args[1], previous)) return kAccessViolation;
  return kSuccess;
}

// NtResumeThread(HANDLE Thread, PULONG PreviousSuspendCount)
uint32_t nt_resume_thread(Process& p, const SyscallRequest& r) {
  uint64_t remote_id = 0;
  if (remote_thread_handle(p, r.args[0], &remote_id)) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::ResumeThread;
    op.thread_id = remote_id;
    const uint32_t status = p.runtime() ? p.runtime()->remote_operation(remote_id & (kRemoteThreadIdLimit - 1), op) : kNotImplemented;
    if (status == kSuccess && r.args[1] != 0 && !put32(p, r.args[1], uint32_t(op.size))) return kAccessViolation;
    return status;
  }
  {      // the thread handle CreateProcess gave out
    bool other = false;
    uint32_t previous = 0;
    const uint32_t status = resume_process_handle(p, r.args[0], &previous, &other);
    if (other) {
      if (status == kSuccess && r.args[1] != 0 && !put32(p, r.args[1], previous)) return kAccessViolation;
      return status;
    }
  }
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  const uint32_t previous = uint32_t(record->suspend_count);
  if (record->suspend_count > 0 && !record->finished && p.runtime()) {
    if (--record->suspend_count == 0) {
      if (!record->started) {      // created suspended: it starts now
        if (!p.runtime()->resume_thread(record->id)) { ++record->suspend_count; return kInsufficientResources; }
        record->started = true;
      } else {
        p.runtime()->request_resume(record->id);
      }
    }
  }
  if (r.args[1] != 0 && !put32(p, r.args[1], previous)) return kAccessViolation;
  return kSuccess;
}

// NtGetContextThread(HANDLE Thread, PCONTEXT) and NtSetContextThread(HANDLE Thread, PCONTEXT): the thread must be suspended (the call waits a moment for it to stop).
uint32_t context_call(Process& p, const SyscallRequest& r, bool set) {
  uint64_t remote_id = 0;
  bool exited = false;
  uint32_t exit_code = 0;
  uint64_t other_pid = 0;
  if (remote_thread_handle(p, r.args[0], &remote_id) && p.runtime()) {      // a thread made by CreateRemoteThread in another program
    X86Context context;
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::GetContext;
    op.thread_id = remote_id; op.context = &context;
    const uint64_t pid = remote_id & (kRemoteThreadIdLimit - 1);
    uint32_t status = p.runtime()->remote_operation(pid, op);
    if (status == kSuccess && set) {
      if (!apply_context(p, r.args[1], &context)) return kAccessViolation;
      op.kind = IProcessRuntime::RemoteOp::SetContext;
      status = p.runtime()->remote_operation(pid, op);
    } else if (status == kSuccess) {
      return store_context(p, r.args[1], context) ? kSuccess : kAccessViolation;
    }
    return status == 0xC000010Au || status == kInvalidHandle ? 0xC000004Bu : status;      // (the program is gone: the thread is terminating)
  }
  if (process_handle_info(p, r.args[0], &exited, &exit_code, &other_pid) && exited && other_pid != p.id()) return 0xC000004Bu;
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  IProcessRuntime* runtime = p.runtime();
  if (!runtime) return kNotImplemented;
  if (record->finished) return 0xC000004Bu;
  if (record->id == runtime->current_thread_id() || record->suspend_count == 0) return 0xC000000Du;      // a thread's own registers, or a thread that runs, are not to be had
  for (uint32_t waited = 0; record->started && !runtime->is_parked(record->id); ++waited) {      // it stops at its next system call or timer tick
    if (waited > 5000 || p.exited()) return 0xC0000001u;
    p.block_sleep(1000000);
    if (record->finished) return 0xC000004Bu;
  }
  X86Context context;
  if (!runtime->get_context(record->id, &context)) return 0xC0000001u;
  if (!set) return store_context(p, r.args[1], context) ? kSuccess : kAccessViolation;
  if (!apply_context(p, r.args[1], &context)) return kAccessViolation;
  return runtime->set_context(record->id, context) ? kSuccess : 0xC0000001u;
}
uint32_t nt_get_context_thread(Process& p, const SyscallRequest& r) { return context_call(p, r, false); }
uint32_t nt_set_context_thread(Process& p, const SyscallRequest& r) { return context_call(p, r, true); }

// NtQueueApcThread(Thread, ApcRoutine, Arg1, Arg2, Arg3): the routine will be called with the arguments in that thread when it waits alertably (SleepEx, WaitForSingleObjectEx, ...)
// or tests for it (NtTestAlert); an alertable wait that is going on ends at once.
uint32_t nt_queue_apc_thread(Process& p, const SyscallRequest& r) {
  uint64_t arg3 = 0;
  if (!windowsabi::argument(p, r, 4, &arg3)) return kAccessViolation;
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  if (record->finished) return 0xC000004Bu;      // STATUS_THREAD_IS_TERMINATING
  if (r.args[1] == 0) return kInvalidParameter;
  if (!Process::queue_apc(*record, Process::ThreadRecord::Apc{r.args[1], r.args[2], r.args[3], arg3})) return kInsufficientResources;
  if (p.runtime()) p.runtime()->notify();
  if (ObjectManager* objects = system_objects()) objects->notify();
  return kSuccess;
}

// NtDequeueApc(PVOID Out): takes the oldest APC queued for the calling thread: four 64-bit numbers (routine, argument 1, 2, 3). STATUS_NO_MORE_ENTRIES if there is none. ntdll
// builds NtTestAlert and the delivery after an alertable wait on it.
uint32_t nt_dequeue_apc(Process& p, const SyscallRequest& r) {
  Process::ThreadRecord* record = thread_of(p, kCurrentThread);
  if (!record) return kInvalidHandle;
  Process::ThreadRecord::Apc apc;
  if (!Process::take_apc(*record, &apc)) return 0x8000001Au;
  uint8_t out[32];
  const uint64_t values[4] = {apc.routine, apc.a1, apc.a2, apc.a3};
  for (uint32_t v = 0; v < 4; ++v) for (uint32_t b = 0; b < 8; ++b) out[v * 8 + b] = uint8_t(values[v] >> (8 * b));
  if (!p.write_user(r.args[0], ConstByteView(out, 32))) {
    Process::queue_apc(*record, apc);      // (it stays queued)
    return kAccessViolation;
  }
  return kSuccess;
}

// NtOpenThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID): a thread of this process by its id.
uint32_t nt_open_thread(Process& p, const SyscallRequest& r) {
  uint8_t id[16];
  if (r.args[3] == 0 || !p.read_user(r.args[3], ByteView(id, 16))) return kAccessViolation;
  uint64_t process_id = 0, thread_id = 0;
  for (uint32_t i = 0; i < 8; ++i) { process_id |= uint64_t{id[i]} << (8 * i); thread_id |= uint64_t{id[8 + i]} << (8 * i); }
  if (process_id != 0 && process_id != p.id()) return kInvalidHandle;      // (threads of other processes are not reachable)
  Process::ThreadRecord* record = p.find_thread(thread_id);
  if (!record || record->finished) return 0xC000000Bu;      // STATUS_INVALID_CID
  const int64_t slot = p.files().open_token(DescriptorKind::Thread, thread_id);
  if (slot < 0) return kInsufficientResources;
  uint8_t attributes[4] = {};
  if (r.args[2] != 0) p.read_user(r.args[2] + 24, ByteView(attributes, 4));
  p.files().set_handle_flags(uint64_t(slot), (attributes[0] & 2) != 0, false);
  if (!p.write_user_u64(r.args[0], (uint64_t(slot) + 1) * 4)) { p.files().close(uint64_t(slot)); return kAccessViolation; }
  return kSuccess;
}

// NtQueryInformationThread(Thread, Class, Buffer, Length, ReturnLength): basic information (0), times (1), the start address (9), whether it is the last thread (12),
// and the suspend count (35).
uint32_t nt_query_information_thread(Process& p, const SyscallRequest& r) {
  uint64_t returned = 0;
  if (!windowsabi::argument(p, r, 4, &returned)) return kAccessViolation;
  uint64_t remote_id = 0;
  if (remote_thread_handle(p, r.args[0], &remote_id)) {      // a thread of another program: what the object knows (basic information only)
    if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;
    if (uint32_t(r.args[3]) < 48) return kInfoLengthMismatch;
    bool exited = false;
    uint32_t code = 0;
    uint64_t owner = 0;
    uint64_t slot = r.args[0] / 4 - 1;
    if (!system_objects()->query_process(uint32_t(p.files().token(slot)), &exited, &code, &owner)) return kInvalidHandle;
    uint8_t info[48] = {};
    const uint32_t status_value = exited ? code : kStillActive;
    for (uint32_t i = 0; i < 4; ++i) info[i] = uint8_t(status_value >> (8 * i));
    for (uint32_t i = 0; i < 8; ++i) { info[16 + i] = uint8_t((owner & (kRemoteThreadIdLimit - 1)) >> (8 * i)); info[24 + i] = uint8_t(owner >> (8 * i)); }
    if (returned != 0 && !p.write_user_u32(returned, 48)) return kAccessViolation;
    return p.write_user(r.args[2], ConstByteView(info, 48)) ? kSuccess : kAccessViolation;
  }
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);
  uint8_t out[48] = {};
  auto put = [&](uint32_t at, uint64_t v, uint32_t size) { for (uint32_t i = 0; i < size; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  uint32_t size = 0;
  switch (info_class) {
    case 0: {      // THREAD_BASIC_INFORMATION
      IProcessRuntime* runtime = p.runtime();
      put(0, record->finished ? record->exit_code : kStillActive, 4);      // ExitStatus
      put(8, record->teb, 8);                                               // TebBaseAddress
      put(16, p.id(), 8);                                                   // ClientId.UniqueProcess
      put(24, record->id, 8);                                               // ClientId.UniqueThread
      put(32, runtime ? runtime->affinity(record->id) : 1, 8);              // AffinityMask
      put(40, uint32_t(record->priority), 4);                               // Priority
      put(44, uint32_t(record->priority), 4);                               // BasePriority
      size = 48;
      break;
    }
    case 1: {      // KERNEL_USER_TIMES
      uint64_t used = 0;
      if (p.runtime()) p.runtime()->cpu_time(record->id, &used);
      put(24, used, 8);      // UserTime (the time is not split into kernel and user)
      size = 32;
      break;
    }
    case 9: put(0, record->start_address, 8); size = 8; break;      // ThreadQuerySetWin32StartAddress
    case 12: put(0, p.live_threads() <= 1 ? 1 : 0, 4); size = 4; break;      // ThreadAmILastThread
    case 35: put(0, uint32_t(record->suspend_count), 4); size = 4; break;      // ThreadSuspendCount
    default: return kInvalidInfoClass;
  }
  if (returned != 0 && !put32(p, returned, size)) return kAccessViolation;
  if (length < size) return kInfoLengthMismatch;
  return p.write_user(r.args[2], ConstByteView(out, size)) ? kSuccess : kAccessViolation;
}

// NtSetInformationThread(Thread, Class, Buffer, Length): priority (2, 3: the value is kept for the program to read back), affinity (4), ideal processor (13, which
// only has to name a core that exists), and hiding from a debugger (17: nothing to hide from).
uint32_t nt_set_information_thread(Process& p, const SyscallRequest& r) {
  uint64_t remote_id = 0;
  if (remote_thread_handle(p, r.args[0], &remote_id)) return kSuccess;      // (the scheduling of another program's thread is that program's own: accepted, not applied)
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);
  uint8_t in[8] = {};
  auto need = [&](uint32_t bytes) -> uint32_t {
    if (length != bytes) return kInfoLengthMismatch;
    return p.read_user(r.args[2], ByteView(in, bytes)) ? kSuccess : kAccessViolation;
  };
  switch (info_class) {
    case 2:
    case 3: {      // ThreadPriority, ThreadBasePriority
      const uint32_t status = need(4);
      if (status != kSuccess) return status;
      const int32_t value = int32_t(in[0] | (uint32_t{in[1]} << 8) | (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 24));
      if (value < -16 || value > 16) return kInvalidParameter;
      record->priority = value;
      if (p.runtime()) p.runtime()->set_priority(record->id, absolute_thread_priority(p.priority_class, value));
      return kSuccess;
    }
    case 4: {      // ThreadAffinityMask
      const uint32_t status = need(8);
      if (status != kSuccess) return status;
      uint64_t mask = 0;
      for (uint32_t i = 0; i < 8; ++i) mask |= uint64_t{in[i]} << (8 * i);
      return p.runtime() && p.runtime()->set_affinity(record->id, mask) ? kSuccess : kInvalidParameter;
    }
    case 13: {     // ThreadIdealProcessor
      const uint32_t status = need(4);
      if (status != kSuccess) return status;
      const uint32_t core = in[0] | (uint32_t{in[1]} << 8) | (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 24);
      return p.runtime() && core < p.runtime()->processor_count() ? kSuccess : kInvalidParameter;
    }
    case 17: return length == 0 ? kSuccess : kInfoLengthMismatch;
    default: return kInvalidInfoClass;
  }
}

}  // namespace process::windowsabi
