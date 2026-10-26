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

bool put32(Process& p, uint64_t address, uint32_t v) {
  const uint8_t raw[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  return p.write_user(address, ConstByteView(raw, 4));
}

}  // namespace

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
  if (r.args[3] != ~uint64_t{0}) return kInvalidHandle;      // threads in other processes are not reachable
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
  if (ObjectManager* objects = system_objects()) objects->abandon_thread(record.id);
  if (p.live_threads() == 0) p.terminate(code);
  if (p.runtime()) p.runtime()->notify();
  if (ObjectManager* objects = system_objects()) objects->notify();
}

// NtTerminateThread(HANDLE Thread, NTSTATUS ExitStatus): the calling thread ends at once; another one is told to end (it does at its next system call or timer tick).
uint32_t nt_terminate_thread(Process& p, const SyscallRequest& r) {
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

// NtQueryInformationThread(Thread, Class, Buffer, Length, ReturnLength): ThreadBasicInformation (0) only.
uint32_t nt_query_information_thread(Process& p, const SyscallRequest& r) {
  uint64_t returned = 0;
  if (!windowsabi::argument(p, r, 4, &returned)) return kAccessViolation;
  Process::ThreadRecord* record = thread_of(p, r.args[0]);
  if (!record) return kInvalidHandle;
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;
  if (uint32_t(r.args[3]) < 48) return kInfoLengthMismatch;
  uint8_t out[48] = {};
  auto put64 = [&](uint32_t at, uint64_t v) { for (uint32_t i = 0; i < 8; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  put64(0, record->finished ? record->exit_code : kStillActive);      // ExitStatus
  put64(8, record->teb);                                               // TebBaseAddress
  put64(16, p.id());                                                   // ClientId.UniqueProcess
  put64(24, record->id);                                               // ClientId.UniqueThread
  put64(32, 1);                                                        // AffinityMask
  if (!p.write_user(r.args[2], ConstByteView(out, 48))) return kAccessViolation;
  if (returned != 0 && !put32(p, returned, 48)) return kAccessViolation;
  return kSuccess;
}

}  // namespace process::windowsabi
