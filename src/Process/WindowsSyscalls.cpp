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
#include "Process/AddressSpace.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsIo.hpp"
#include "Process/WindowsObjects.hpp"
#include "Process/WindowsThreads.hpp"
#include "Process/WindowsLocks.hpp"
#include "Process/WindowsJobs.hpp"
#include "Process/WindowsDebug.hpp"
#include "Process/WindowsMailslots.hpp"
#include "Process/WindowsRegistry.hpp"
#include "Process/WindowsMemory.hpp"
#include "Process/WindowsMisc.hpp"
#include "Process/WindowsPorts.hpp"
#include "Process/WindowsSockets.hpp"
#include "Process/WindowsModules.hpp"
#include "Process/WindowsPipes.hpp"
#include "Process/WindowsProcess.hpp"
#include "Process/WindowsStartup.hpp"
#include "Process/WindowsSystem.hpp"
#include "Process/WindowsTokens.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

// The Windows system-call interface: the NT native services a PE program reaches through ntdll.dll. A PE program never executes SYSCALL itself; the
// ntdll we load into it holds the stubs ("mov r10, rcx; mov eax, number; syscall; ret"), so the numbers below are this OS's own (see
// WindowsSyscallNumber), not those of any one Windows build, which change from release to release. Results are NTSTATUS values.
namespace process {

namespace {

constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034u;
constexpr uint32_t kStatusSuccess = 0, kStatusNoYield = 0x40000024u, kStatusNotImplemented = 0xC0000002u, kStatusInvalidHandle = 0xC0000008u,
                   kStatusInvalidParameter = 0xC000000Du, kStatusAccessViolation = 0xC0000005u, kStatusEndOfFile = 0xC0000011u, kStatusAccessDenied = 0xC0000022u,
                   kStatusNoMemory = 0xC0000017u, kStatusConflictingAddresses = 0xC0000018u, kStatusUnableToFreeVm = 0xC000001Au, kStatusInvalidPageProtection = 0xC0000045u;
constexpr uint64_t kCurrentProcess = ~uint64_t{0};      // NtCurrentProcess()
constexpr uint64_t kStackArgumentBase = 0x28;           // return address + 32 bytes of home space
constexpr uint64_t kUnixToNtSeconds = 11644473600ull;   // 1601-01-01 to 1970-01-01
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u;

uint64_t nt_time_now(Process& process) { return (process.host().realtime_ns() / 100) + kUnixToNtSeconds * 10000000ull; }      // 100 ns units since 1601

uint32_t nt_query_system_time(Process& process, const SyscallRequest& r) {
  return process.write_user_u64(r.args[0], nt_time_now(process)) ? kStatusSuccess : kStatusAccessViolation;
}

// NtContinue(PCONTEXT Context, BOOLEAN TestAlert): the thread goes on with the registers of the CONTEXT (loaded into the CPU by whoever runs the program, see
// Process::take_context_restore()). Used by ntdll to resume after a handler dealt with an exception.
uint32_t nt_continue(Process& process, const SyscallRequest& r) {
  uint8_t probe[8];
  if (!process.read_user(r.args[0], ByteView(probe))) return kStatusAccessViolation;
  process.request_context_restore(r.args[0]);
  return kStatusSuccess;
}

// NtRaiseException(PEXCEPTION_RECORD, PCONTEXT, BOOLEAN FirstChance) as the kernel sees it: ntdll looks for handlers itself, so a call that gets here is the last chance. The
// process ends with the exception code as its exit code.
uint32_t nt_raise_exception_final(Process& process, const SyscallRequest& r) {
  uint8_t code[4];
  if (!process.read_user(r.args[0], ByteView(code))) return kStatusAccessViolation;
  process.terminate(uint32_t(code[0]) | uint32_t(code[1]) << 8 | uint32_t(code[2]) << 16 | uint32_t(code[3]) << 24);
  return kStatusSuccess;
}

// NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval): negative = relative, in 100 ns units; positive = an absolute system time.
uint32_t nt_delay_execution(Process& process, const SyscallRequest& r) {
  uint64_t raw = 0;
  if (!process.read_user_u64(r.args[1], &raw)) return kStatusAccessViolation;
  uint64_t units = 0;
  if (int64_t(raw) < 0) {
    units = uint64_t(0) - raw;
  } else {
    const uint64_t now = nt_time_now(process);
    units = raw > now ? raw - now : 0;
  }
  const uint64_t nanoseconds = units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100;
  if ((r.args[0] & 0xFF) != 0 && process.runtime()) return windowsabi::alertable_delay(process, nanoseconds);      // an APC may end it
  process.block_sleep(nanoseconds);
  return kStatusSuccess;
}

// NtQueryTimerResolution(PULONG Minimum, PULONG Maximum, PULONG Current) and NtSetTimerResolution(ULONG Desired, BOOLEAN Set, PULONG Current): the system's clock ticks
// are a 10 ms scheduler tick with a finer clock. A program may ask for a period between the maximum (finest) and the minimum (coarsest) resolution; the current
// resolution is the finest of what was asked for (one request per program, replaced by the next, taken back with Set = FALSE) and the system's own 10 ms.
constexpr uint32_t kTimerMinimum = 156250, kTimerMaximum = 10000, kTimerInitial = 10000, kStatusTimerResolutionNotSet = 0xC0000245u;
struct TimerRequests {
  static constexpr uint32_t kSlots = 32;
  uint64_t pid[kSlots] = {};
  uint32_t value[kSlots] = {};
  uint8_t lock = 0;
};
TimerRequests g_timer_requests;
struct TimerRequestsLock {      // (a few instructions long; it spins without masking interrupts)
  TimerRequestsLock() { while (__atomic_exchange_n(&g_timer_requests.lock, 1, __ATOMIC_ACQUIRE)) {} }
  ~TimerRequestsLock() { __atomic_store_n(&g_timer_requests.lock, 0, __ATOMIC_RELEASE); }
};
uint32_t current_timer_resolution() {      // (with the lock held)
  uint32_t best = kTimerInitial;
  for (uint32_t i = 0; i < TimerRequests::kSlots; ++i) if (g_timer_requests.value[i] != 0 && g_timer_requests.value[i] < best) best = g_timer_requests.value[i];
  return best;
}

uint32_t nt_query_timer_resolution(Process& process, const SyscallRequest& r) {
  uint32_t current = 0;
  { TimerRequestsLock lock; current = current_timer_resolution(); }
  const uint32_t values[3] = {kTimerMinimum, kTimerMaximum, current};
  for (uint32_t i = 0; i < 3; ++i) if (r.args[i] == 0) return kStatusAccessViolation;
  for (uint32_t i = 0; i < 3; ++i) {
    uint8_t raw[4] = {uint8_t(values[i]), uint8_t(values[i] >> 8), uint8_t(values[i] >> 16), uint8_t(values[i] >> 24)};
    if (!process.write_user(r.args[i], ConstByteView(raw, 4))) return kStatusAccessViolation;
  }
  return kStatusSuccess;
}
uint32_t nt_set_timer_resolution(Process& process, const SyscallRequest& r) {
  if (r.args[2] == 0) return kStatusAccessViolation;
  uint32_t desired = uint32_t(r.args[0]), current = 0, status = kStatusSuccess;
  const bool set = (r.args[1] & 0xFF) != 0;
  {
    TimerRequestsLock lock;
    uint32_t slot = TimerRequests::kSlots, free_slot = TimerRequests::kSlots;
    for (uint32_t i = 0; i < TimerRequests::kSlots; ++i) {
      if (g_timer_requests.value[i] != 0 && g_timer_requests.pid[i] == process.id()) slot = i;
      if (g_timer_requests.value[i] == 0 && free_slot == TimerRequests::kSlots) free_slot = i;
    }
    if (set) {
      if (desired < kTimerMaximum) desired = kTimerMaximum;
      if (desired > kTimerMinimum) desired = kTimerMinimum;
      if (slot == TimerRequests::kSlots) slot = free_slot;
      if (slot != TimerRequests::kSlots) { g_timer_requests.pid[slot] = process.id(); g_timer_requests.value[slot] = desired; }
    } else if (slot == TimerRequests::kSlots) {
      status = kStatusTimerResolutionNotSet;
    } else {
      g_timer_requests.value[slot] = 0;
    }
    current = current_timer_resolution();
  }
  return process.write_user_u32(r.args[2], current) ? status : kStatusAccessViolation;
}

// NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency): 100 ns ticks, so the frequency is 10 MHz.
uint32_t nt_query_performance_counter(Process& process, const SyscallRequest& r) {
  if (r.args[0] == 0 || !process.write_user_u64(r.args[0], process.host().monotonic_ns() / 100)) return kStatusAccessViolation;
  if (r.args[1] != 0 && !process.write_user_u64(r.args[1], 10000000ull)) return kStatusAccessViolation;
  return kStatusSuccess;
}

bool is_service(uint64_t number) { return number < kWindowsServiceCount || (number >= kWin32kServiceBase && number < kWin32kServiceEnd); }

}  // namespace

class WindowsSyscalls final : public ISyscallInterface {
 public:
  SyscallAbi abi() const override { return SyscallAbi::Windows; }
  UserArch arch() const override { return UserArch::X86_64; }

  // The calls that waited in the kernel and finished since (the answer of another thread, another program, the end of a handle) are applied to this program's memory before
  // the call begins and once it is over, so that whatever the program looks at next is there.
  uint64_t dispatch(Process& process, const SyscallRequest& r) const override {
    windowsabi::io_reap_if_needed(process);
    const uint64_t result = dispatch_service(process, r);
    windowsabi::io_reap_if_needed(process);
    return result;
  }

  uint64_t dispatch_service(Process& process, const SyscallRequest& r) const {
    if (r.number > 0xFFFFFFFFu) return kStatusInvalidSystemService;
    refresh_shared_user_data(process.host());
    switch (static_cast<WindowsSyscallNumber>(r.number)) {
      case WindowsSyscallNumber::NtCancelIoFile: return windowsabi::nt_cancel_io_file(process, r);
      case WindowsSyscallNumber::NtCancelIoFileEx: return windowsabi::nt_cancel_io_file_ex(process, r);
      case WindowsSyscallNumber::NtSetIoCompletion: return windowsabi::nt_set_io_completion(process, r);
      case WindowsSyscallNumber::NtSetIoCompletionEx: return windowsabi::nt_set_io_completion_ex(process, r);
      case WindowsSyscallNumber::NtRemoveIoCompletion: return windowsabi::nt_remove_io_completion(process, r);
      case WindowsSyscallNumber::NtRemoveIoCompletionEx: return windowsabi::nt_remove_io_completion_ex(process, r);
      case WindowsSyscallNumber::NtQueryIoCompletion: return windowsabi::nt_query_io_completion(process, r);
      case WindowsSyscallNumber::NtReadFile: return windowsabi::nt_read_file(process, r);
      case WindowsSyscallNumber::NtWriteFile: return windowsabi::nt_write_file(process, r);
      case WindowsSyscallNumber::NtClose: return windowsabi::nt_close(process, r);
      case WindowsSyscallNumber::NtCreateFile: return windowsabi::nt_create_file(process, r);
      case WindowsSyscallNumber::NtOpenFile: return windowsabi::nt_open_file(process, r);
      case WindowsSyscallNumber::NtQueryInformationFile: return windowsabi::nt_query_information_file(process, r);
      case WindowsSyscallNumber::NtSetInformationFile: return windowsabi::nt_set_information_file(process, r);
      case WindowsSyscallNumber::NtQueryDirectoryFile: return windowsabi::nt_query_directory_file(process, r);
      case WindowsSyscallNumber::NtQueryAttributesFile: return windowsabi::nt_query_attributes_file(process, r);
      case WindowsSyscallNumber::NtDeleteFile: return windowsabi::nt_delete_file(process, r);
      case WindowsSyscallNumber::NtFlushBuffersFile: return windowsabi::nt_flush_buffers_file(process, r);
      case WindowsSyscallNumber::NtQueryVolumeInformationFile: return windowsabi::nt_query_volume_information_file(process, r);
      case WindowsSyscallNumber::NtQueryPerformanceCounter: return nt_query_performance_counter(process, r);
      case WindowsSyscallNumber::NtQueryVirtualMemory: return windowsabi::nt_query_virtual_memory(process, r);
      case WindowsSyscallNumber::NtQueryInformationProcess: return windowsabi::nt_query_information_process(process, r);
      case WindowsSyscallNumber::NtGetCurrentProcessorNumber: return process.runtime() ? process.runtime()->current_processor() : 0;
      case WindowsSyscallNumber::NtLoadModule: return windowsabi::nt_load_module(process, r);
      case WindowsSyscallNumber::NtQueueApcThread: return windowsabi::nt_queue_apc_thread(process, r);
      case WindowsSyscallNumber::NtDequeueApc: return windowsabi::nt_dequeue_apc(process, r);
      case WindowsSyscallNumber::NtSignalAndWaitForSingleObject: return windowsabi::nt_signal_and_wait(process, r);
      case WindowsSyscallNumber::NtCreateTimer: return windowsabi::nt_create_timer(process, r);
      case WindowsSyscallNumber::NtOpenTimer: return windowsabi::nt_open_timer(process, r);
      case WindowsSyscallNumber::NtSetTimer: return windowsabi::nt_set_timer(process, r);
      case WindowsSyscallNumber::NtCancelTimer: return windowsabi::nt_cancel_timer(process, r);
      case WindowsSyscallNumber::NtQueryTimer: return windowsabi::nt_query_timer(process, r);
      case WindowsSyscallNumber::NtQueryTimerResolution: return nt_query_timer_resolution(process, r);
      case WindowsSyscallNumber::NtSetTimerResolution: return nt_set_timer_resolution(process, r);
      case WindowsSyscallNumber::NtOpenThread: return windowsabi::nt_open_thread(process, r);
      case WindowsSyscallNumber::NtOpenProcess: return windowsabi::nt_open_process(process, r);
      case WindowsSyscallNumber::NtCreateSection: return windowsabi::nt_create_section(process, r);
      case WindowsSyscallNumber::NtQuerySection: return windowsabi::nt_query_section(process, r);
      case WindowsSyscallNumber::NtOpenSection: return windowsabi::nt_open_section(process, r);
      case WindowsSyscallNumber::NtMapViewOfSection: return windowsabi::nt_map_view_of_section(process, r);
      case WindowsSyscallNumber::NtUnmapViewOfSection: return windowsabi::nt_unmap_view_of_section(process, r);
      case WindowsSyscallNumber::NtAreMappedFilesTheSame: return windowsabi::nt_are_mapped_files_the_same(process, r);
      case WindowsSyscallNumber::NtFlushProcessWriteBuffers: if (process.runtime()) process.runtime()->flush_write_buffers(); return 0;
      case WindowsSyscallNumber::NtFlushVirtualMemory: return windowsabi::nt_flush_virtual_memory(process, r);
      case WindowsSyscallNumber::NtLockFile: return windowsabi::nt_lock_file(process, r);
      case WindowsSyscallNumber::NtUnlockFile: return windowsabi::nt_unlock_file(process, r);
      case WindowsSyscallNumber::NtCreateJobObject: return windowsabi::nt_create_job_object(process, r);
      case WindowsSyscallNumber::NtOpenJobObject: return windowsabi::nt_open_job_object(process, r);
      case WindowsSyscallNumber::NtAllocateReserveObject: return windowsabi::nt_allocate_reserve_object(process, r);
      case WindowsSyscallNumber::NtQueueApcThreadEx: return windowsabi::nt_queue_apc_thread_ex(process, r, false);
      case WindowsSyscallNumber::NtQueueApcThreadEx2: return windowsabi::nt_queue_apc_thread_ex(process, r, true);
      case WindowsSyscallNumber::NtCreateKeyedEvent: return windowsabi::nt_create_keyed_event(process, r);
      case WindowsSyscallNumber::NtOpenKeyedEvent: return windowsabi::nt_open_keyed_event(process, r);
      case WindowsSyscallNumber::NtCreateIoCompletion: return windowsabi::nt_create_io_completion(process, r);
      case WindowsSyscallNumber::NtCreateDirectoryObject: return windowsabi::nt_create_directory_object(process, r);
      case WindowsSyscallNumber::NtOpenDirectoryObject: return windowsabi::nt_open_directory_object(process, r);
      case WindowsSyscallNumber::NtQueryDirectoryObject: return windowsabi::nt_query_directory_object(process, r);
      case WindowsSyscallNumber::NtCreateSymbolicLinkObject: return windowsabi::nt_create_symbolic_link_object(process, r);
      case WindowsSyscallNumber::NtOpenSymbolicLinkObject: return windowsabi::nt_open_symbolic_link_object(process, r);
      case WindowsSyscallNumber::NtQuerySymbolicLinkObject: return windowsabi::nt_query_symbolic_link_object(process, r);
      case WindowsSyscallNumber::NtOpenIoCompletion: return windowsabi::nt_open_io_completion(process, r);
      case WindowsSyscallNumber::NtAssignProcessToJobObject: return windowsabi::nt_assign_process_to_job_object(process, r);
      case WindowsSyscallNumber::NtTerminateJobObject: return windowsabi::nt_terminate_job_object(process, r);
      case WindowsSyscallNumber::NtQueryInformationJobObject: return windowsabi::nt_query_information_job_object(process, r);
      case WindowsSyscallNumber::NtSetInformationJobObject: return windowsabi::nt_set_information_job_object(process, r);
      case WindowsSyscallNumber::NtIsProcessInJob: return windowsabi::nt_is_process_in_job(process, r);
      case WindowsSyscallNumber::NtCreatePipe: return windowsabi::nt_create_pipe(process, r);
      case WindowsSyscallNumber::NtCreateNamedPipeFile: return windowsabi::nt_create_named_pipe_file(process, r);
      case WindowsSyscallNumber::NtDeviceIoControlFile: return windowsabi::nt_device_io_control_file(process, r);
      case WindowsSyscallNumber::NtDebugActiveProcess: return windowsabi::nt_debug_active_process(process, r);
      case WindowsSyscallNumber::NtRemoveProcessDebug: return windowsabi::nt_remove_process_debug(process, r);
      case WindowsSyscallNumber::NtWaitForDebugEvent: return windowsabi::nt_wait_for_debug_event(process, r);
      case WindowsSyscallNumber::NtDebugContinue: return windowsabi::nt_debug_continue(process, r);
      case WindowsSyscallNumber::NtSetInformationDebugObject: return windowsabi::nt_set_information_debug_object(process, r);
      case WindowsSyscallNumber::NtCreateDebugObject: return windowsabi::nt_create_debug_object(process, r);
      case WindowsSyscallNumber::NtSuspendProcess: return windowsabi::nt_suspend_resume_process(process, r, true);
      case WindowsSyscallNumber::NtResumeProcess: return windowsabi::nt_suspend_resume_process(process, r, false);
      case WindowsSyscallNumber::NtTcosDebugException: return windowsabi::nt_tcos_debug_exception(process, r);
      case WindowsSyscallNumber::NtTcosLinkModules: return windowsabi::nt_link_modules(process, r);
      case WindowsSyscallNumber::NtTcosToken: return windowsabi::nt_tcos_token(process, r);
      case WindowsSyscallNumber::NtTcosAccessCheck: return windowsabi::nt_tcos_access_check(process, r);
      case WindowsSyscallNumber::NtCancelSynchronousIoFile: return windowsabi::nt_cancel_synchronous_io_file(process, r);
      case WindowsSyscallNumber::NtFsControlFile: return windowsabi::nt_fs_control_file(process, r);
      case WindowsSyscallNumber::NtQueryObject: return windowsabi::nt_query_object(process, r);
      case WindowsSyscallNumber::NtSetInformationObject: return windowsabi::nt_set_information_object(process, r);
      case WindowsSyscallNumber::NtCompareObjects: return windowsabi::nt_compare_objects(process, r);
      case WindowsSyscallNumber::NtReleaseModule: return windowsabi::nt_release_module(process, r);
      case WindowsSyscallNumber::NtUnloadModule: return windowsabi::nt_unload_module(process, r);
      case WindowsSyscallNumber::NtSetInformationProcess: return windowsabi::nt_set_information_process(process, r);
      case WindowsSyscallNumber::NtSetInformationThread: return windowsabi::nt_set_information_thread(process, r);
      case WindowsSyscallNumber::NtQuerySystemInformation: return windowsabi::nt_query_system_information(process, r);
      case WindowsSyscallNumber::NtQuerySystemInformationEx: return windowsabi::nt_query_system_information_ex(process, r);
      case WindowsSyscallNumber::NtContinue: return nt_continue(process, r);
      case WindowsSyscallNumber::NtCreateThreadEx: return windowsabi::nt_create_thread_ex(process, r);
      case WindowsSyscallNumber::NtTerminateThread: return windowsabi::nt_terminate_thread(process, r);
      case WindowsSyscallNumber::NtWaitForSingleObject: return windowsabi::nt_wait_for_single_object(process, r);
      case WindowsSyscallNumber::NtResumeThread: return windowsabi::nt_resume_thread(process, r);
      case WindowsSyscallNumber::NtQueryInformationThread: return windowsabi::nt_query_information_thread(process, r);
      case WindowsSyscallNumber::NtCreateEvent: return windowsabi::nt_create_event(process, r);
      case WindowsSyscallNumber::NtOpenEvent: return windowsabi::nt_open_event(process, r);
      case WindowsSyscallNumber::NtSetEvent: return windowsabi::nt_set_event(process, r);
      case WindowsSyscallNumber::NtResetEvent: return windowsabi::nt_reset_event(process, r);
      case WindowsSyscallNumber::NtPulseEvent: return windowsabi::nt_pulse_event(process, r);
      case WindowsSyscallNumber::NtCreateMutant: return windowsabi::nt_create_mutant(process, r);
      case WindowsSyscallNumber::NtOpenMutant: return windowsabi::nt_open_mutant(process, r);
      case WindowsSyscallNumber::NtReleaseMutant: return windowsabi::nt_release_mutant(process, r);
      case WindowsSyscallNumber::NtCreateSemaphore: return windowsabi::nt_create_semaphore(process, r);
      case WindowsSyscallNumber::NtOpenSemaphore: return windowsabi::nt_open_semaphore(process, r);
      case WindowsSyscallNumber::NtReleaseSemaphore: return windowsabi::nt_release_semaphore(process, r);
      case WindowsSyscallNumber::NtWaitForMultipleObjects: return windowsabi::nt_wait_for_multiple_objects(process, r);
      case WindowsSyscallNumber::NtDuplicateObject: return windowsabi::nt_duplicate_object(process, r);
      case WindowsSyscallNumber::NtSuspendThread: return windowsabi::nt_suspend_thread(process, r);
      case WindowsSyscallNumber::NtGetContextThread: return windowsabi::nt_get_context_thread(process, r);
      case WindowsSyscallNumber::NtCreateUserProcess: return windowsabi::nt_create_user_process(process, r);
      case WindowsSyscallNumber::NtSetContextThread: return windowsabi::nt_set_context_thread(process, r);
      case WindowsSyscallNumber::NtRaiseException: return nt_raise_exception_final(process, r);
      case WindowsSyscallNumber::NtTerminateProcess:
        if (r.args[0] == 0) return windowsabi::terminate_other_threads(process, uint32_t(r.args[1]));
        if (r.args[0] != kCurrentProcess) {      // a process this one created
          bool handled = false;
          const uint32_t status = windowsabi::terminate_process_handle(process, r.args[0], uint32_t(r.args[1]), &handled);
          return status;
        }
        process.terminate(uint32_t(r.args[1]));
        return kStatusSuccess;
      case WindowsSyscallNumber::NtYieldExecution: process.block_yield(); return kStatusSuccess;
      case WindowsSyscallNumber::NtDelayExecution: return nt_delay_execution(process, r);
      case WindowsSyscallNumber::NtQuerySystemTime: return nt_query_system_time(process, r);
      case WindowsSyscallNumber::NtConvertBetweenAuxiliaryCounterAndPerformanceCounter: return (r.args[1] == 0 || r.args[2] == 0) ? 0xC0000005u : 0xC00000BBu;      // no auxiliary counter: STATUS_NOT_SUPPORTED
      case WindowsSyscallNumber::NtAllocateVirtualMemory: return windowsabi::nt_allocate_virtual_memory(process, r);
      case WindowsSyscallNumber::NtFreeVirtualMemory: return windowsabi::nt_free_virtual_memory(process, r);
      case WindowsSyscallNumber::NtProtectVirtualMemory: return windowsabi::nt_protect_virtual_memory(process, r);
      case WindowsSyscallNumber::NtReadVirtualMemory: return windowsabi::nt_read_virtual_memory(process, r);
      case WindowsSyscallNumber::NtWriteVirtualMemory: return windowsabi::nt_write_virtual_memory(process, r);
      case WindowsSyscallNumber::NtCreateSymbolicLink: return windowsabi::nt_create_symbolic_link(process, r);
      case WindowsSyscallNumber::NtCreateSocket: return windowsabi::nt_create_socket(process, r);
      case WindowsSyscallNumber::NtBindSocket: return windowsabi::nt_bind_socket(process, r);
      case WindowsSyscallNumber::NtConnectSocket: return windowsabi::nt_connect_socket(process, r);
      case WindowsSyscallNumber::NtListenSocket: return windowsabi::nt_listen_socket(process, r);
      case WindowsSyscallNumber::NtAcceptSocket: return windowsabi::nt_accept_socket(process, r);
      case WindowsSyscallNumber::NtSendSocket: return windowsabi::nt_send_socket(process, r);
      case WindowsSyscallNumber::NtReceiveSocket: return windowsabi::nt_receive_socket(process, r);
      case WindowsSyscallNumber::NtSocketControl: return windowsabi::nt_socket_control(process, r);
      case WindowsSyscallNumber::NtSelectSockets: return windowsabi::nt_select_sockets(process, r);
      case WindowsSyscallNumber::NtCreateChangeNotification: return windowsabi::nt_create_change_notification(process, r);
      case WindowsSyscallNumber::NtCreateMailslotFile: return windowsabi::nt_create_mailslot_file(process, r);
      case WindowsSyscallNumber::NtCreateKey: return windowsabi::nt_create_key(process, r);
      case WindowsSyscallNumber::NtOpenKey: return windowsabi::nt_open_key(process, r);
      case WindowsSyscallNumber::NtOpenKeyEx: return windowsabi::nt_open_key_ex(process, r);
      case WindowsSyscallNumber::NtQueryKey: return windowsabi::nt_query_key(process, r);
      case WindowsSyscallNumber::NtEnumerateKey: return windowsabi::nt_enumerate_key(process, r);
      case WindowsSyscallNumber::NtEnumerateValueKey: return windowsabi::nt_enumerate_value_key(process, r);
      case WindowsSyscallNumber::NtQueryValueKey: return windowsabi::nt_query_value_key(process, r);
      case WindowsSyscallNumber::NtSetValueKey: return windowsabi::nt_set_value_key(process, r);
      case WindowsSyscallNumber::NtDeleteKey: return windowsabi::nt_delete_key(process, r);
      case WindowsSyscallNumber::NtDeleteValueKey: return windowsabi::nt_delete_value_key(process, r);
      case WindowsSyscallNumber::NtFlushKey: return windowsabi::nt_flush_key(process, r);
      case WindowsSyscallNumber::NtAlertThreadByThreadId: return windowsabi::nt_alert_thread_by_thread_id(process, r);
      case WindowsSyscallNumber::NtWaitForAlertByThreadId: return windowsabi::nt_wait_for_alert_by_thread_id(process, r);
      case WindowsSyscallNumber::NtAlertThread: return windowsabi::nt_alert_thread(process, r);
      case WindowsSyscallNumber::NtAlertResumeThread: return windowsabi::nt_alert_resume_thread(process, r);
      case WindowsSyscallNumber::NtWaitForKeyedEvent: return windowsabi::nt_wait_for_keyed_event(process, r);
      case WindowsSyscallNumber::NtReleaseKeyedEvent: return windowsabi::nt_release_keyed_event(process, r);
      case WindowsSyscallNumber::NtClearEvent: return windowsabi::nt_clear_event(process, r);
      case WindowsSyscallNumber::NtQueryEvent: return windowsabi::nt_query_event(process, r);
      case WindowsSyscallNumber::NtQueryMutant: return windowsabi::nt_query_mutant(process, r);
      case WindowsSyscallNumber::NtQuerySemaphore: return windowsabi::nt_query_semaphore(process, r);
      case WindowsSyscallNumber::NtSetEventBoostPriority: return windowsabi::nt_set_event_boost_priority(process, r);
      case WindowsSyscallNumber::NtGetCurrentProcessorNumberEx: return windowsabi::nt_get_current_processor_number_ex(process, r);
      case WindowsSyscallNumber::NtQueryDefaultLocale: return windowsabi::nt_query_default_locale(process, r);
      case WindowsSyscallNumber::NtSetDefaultLocale: return windowsabi::nt_set_default_locale(process, r);
      case WindowsSyscallNumber::NtQueryDefaultUILanguage: return windowsabi::nt_query_default_ui_language(process, r);
      case WindowsSyscallNumber::NtSetDefaultUILanguage: return windowsabi::nt_set_default_ui_language(process, r);
      case WindowsSyscallNumber::NtQueryInstallUILanguage: return windowsabi::nt_query_install_ui_language(process, r);
      case WindowsSyscallNumber::NtIsUILanguageComitted: case WindowsSyscallNumber::NtFlushInstallUILanguage: case WindowsSyscallNumber::NtFlushInstructionCache:
      case WindowsSyscallNumber::NtFlushWriteBuffer: case WindowsSyscallNumber::NtSetDebugFilterState: case WindowsSyscallNumber::NtSetIntervalProfile:
      case WindowsSyscallNumber::NtLockVirtualMemory: case WindowsSyscallNumber::NtUnlockVirtualMemory: case WindowsSyscallNumber::NtCloseObjectAuditAlarm:
      case WindowsSyscallNumber::NtDeleteObjectAuditAlarm: case WindowsSyscallNumber::NtOpenObjectAuditAlarm: case WindowsSyscallNumber::NtPrivilegeObjectAuditAlarm:
      case WindowsSyscallNumber::NtPrivilegedServiceAuditAlarm: case WindowsSyscallNumber::NtDisplayString: case WindowsSyscallNumber::NtSetDefaultHardErrorPort:
      case WindowsSyscallNumber::NtTraceEvent: case WindowsSyscallNumber::NtDisableLastKnownGood: case WindowsSyscallNumber::NtEnableLastKnownGood:
        return windowsabi::nt_succeed(process, r);
      case WindowsSyscallNumber::NtQueryDebugFilterState: case WindowsSyscallNumber::NtQueryIntervalProfile: return windowsabi::nt_query_interval_profile(process, r);
      case WindowsSyscallNumber::NtAllocateUuids: return windowsabi::nt_allocate_uuids(process, r);
      case WindowsSyscallNumber::NtSetUuidSeed: return windowsabi::nt_set_uuid_seed(process, r);
      case WindowsSyscallNumber::NtPowerInformation: return windowsabi::nt_power_information(process, r);
      case WindowsSyscallNumber::NtSetThreadExecutionState: return windowsabi::nt_set_thread_execution_state(process, r);
      case WindowsSyscallNumber::NtRaiseHardError: return windowsabi::nt_raise_hard_error(process, r);
      case WindowsSyscallNumber::NtMakeTemporaryObject: return windowsabi::nt_make_object_temporary(process, r);
      case WindowsSyscallNumber::NtMakePermanentObject: return windowsabi::nt_make_object_permanent(process, r);
      case WindowsSyscallNumber::NtSetSystemTime: case WindowsSyscallNumber::NtSetSystemInformation: case WindowsSyscallNumber::NtLoadDriver: case WindowsSyscallNumber::NtUnloadDriver:
      case WindowsSyscallNumber::NtShutdownSystem: case WindowsSyscallNumber::NtInitiatePowerAction: case WindowsSyscallNumber::NtSetSystemPowerState:
      case WindowsSyscallNumber::NtCreatePagingFile: case WindowsSyscallNumber::NtSystemDebugControl: case WindowsSyscallNumber::NtSetLdtEntries:
        return windowsabi::nt_privilege_not_held(process, r);
      case WindowsSyscallNumber::NtCreatePort: case WindowsSyscallNumber::NtCreateWaitablePort: return windowsabi::nt_create_port(process, r);
      case WindowsSyscallNumber::NtConnectPort: return windowsabi::nt_connect_port(process, r, false);
      case WindowsSyscallNumber::NtSecureConnectPort: return windowsabi::nt_connect_port(process, r, true);
      case WindowsSyscallNumber::NtListenPort: return windowsabi::nt_listen_port(process, r);
      case WindowsSyscallNumber::NtAcceptConnectPort: return windowsabi::nt_accept_connect_port(process, r);
      case WindowsSyscallNumber::NtCompleteConnectPort: return windowsabi::nt_complete_connect_port(process, r);
      case WindowsSyscallNumber::NtRequestPort: return windowsabi::nt_request_port(process, r);
      case WindowsSyscallNumber::NtRequestWaitReplyPort: return windowsabi::nt_request_wait_reply_port(process, r);
      case WindowsSyscallNumber::NtReplyPort: return windowsabi::nt_reply_port(process, r);
      case WindowsSyscallNumber::NtReplyWaitReceivePort: return windowsabi::nt_reply_wait_receive_port(process, r, false);
      case WindowsSyscallNumber::NtReplyWaitReceivePortEx: return windowsabi::nt_reply_wait_receive_port(process, r, true);
      case WindowsSyscallNumber::NtReplyWaitReplyPort: return windowsabi::nt_reply_wait_reply_port(process, r);
      case WindowsSyscallNumber::NtQueryInformationPort: return windowsabi::nt_query_information_port(process, r);
      case WindowsSyscallNumber::NtRegisterThreadTerminatePort: return windowsabi::nt_alpc_validate_port(process, r);
      case WindowsSyscallNumber::NtAlpcCreatePort: return windowsabi::nt_alpc_create_port(process, r);
      case WindowsSyscallNumber::NtAlpcConnectPort: return windowsabi::nt_alpc_connect_port(process, r);
      case WindowsSyscallNumber::NtAlpcAcceptConnectPort: return windowsabi::nt_alpc_accept_connect_port(process, r);
      case WindowsSyscallNumber::NtAlpcSendWaitReceivePort: return windowsabi::nt_alpc_send_wait_receive_port(process, r);
      case WindowsSyscallNumber::NtAlpcDisconnectPort: return windowsabi::nt_alpc_disconnect_port(process, r);
      case WindowsSyscallNumber::NtAlpcQueryInformation: return windowsabi::nt_alpc_query_information(process, r);
      case WindowsSyscallNumber::NtAlpcSetInformation: return windowsabi::nt_alpc_set_information(process, r);
      case WindowsSyscallNumber::NtAlpcCancelMessage: case WindowsSyscallNumber::NtAlpcCreateSecurityContext: case WindowsSyscallNumber::NtAlpcDeleteSecurityContext:
      case WindowsSyscallNumber::NtAlpcRevokeSecurityContext: return windowsabi::nt_alpc_validate_port(process, r);
      case WindowsSyscallNumber::NtGetNextThread: return windowsabi::nt_get_next_thread(process, r);
      case WindowsSyscallNumber::NtQueryMultipleValueKey: return windowsabi::nt_query_multiple_value_key(process, r);
      case WindowsSyscallNumber::NtRenameKey: return windowsabi::nt_rename_key(process, r);
      case WindowsSyscallNumber::NtSetInformationKey: return windowsabi::nt_set_information_key(process, r);
      case WindowsSyscallNumber::NtQueryOpenSubKeys: return windowsabi::nt_query_open_sub_keys(process, r);
      case WindowsSyscallNumber::NtNotifyChangeKey: return windowsabi::nt_notify_change_key(process, r);
      case WindowsSyscallNumber::NtQueryLicenseValue: return kStatusObjectNameNotFound;
      case WindowsSyscallNumber::NtQueryEaFile: case WindowsSyscallNumber::NtSetEaFile: return 0xC000004Fu;      // STATUS_EAS_NOT_SUPPORTED
      case WindowsSyscallNumber::NtQueryQuotaInformationFile: case WindowsSyscallNumber::NtSetQuotaInformationFile: return 0xC00000BBu;
      case WindowsSyscallNumber::NtSetVolumeInformationFile: case WindowsSyscallNumber::NtSetInformationSymbolicLink: return 0xC00000BBu;
      case WindowsSyscallNumber::NtLoadKey: case WindowsSyscallNumber::NtLoadKey2: case WindowsSyscallNumber::NtLoadKey3: case WindowsSyscallNumber::NtLoadKeyEx:
      case WindowsSyscallNumber::NtUnloadKey: case WindowsSyscallNumber::NtUnloadKey2: case WindowsSyscallNumber::NtUnloadKeyEx: case WindowsSyscallNumber::NtSaveKey:
      case WindowsSyscallNumber::NtSaveKeyEx: case WindowsSyscallNumber::NtSaveMergedKeys: case WindowsSyscallNumber::NtRestoreKey: case WindowsSyscallNumber::NtReplaceKey:
      case WindowsSyscallNumber::NtCompactKeys: case WindowsSyscallNumber::NtCompressKey: case WindowsSyscallNumber::NtLockRegistryKey: case WindowsSyscallNumber::NtFreezeRegistry:
      case WindowsSyscallNumber::NtThawRegistry: case WindowsSyscallNumber::NtInitializeRegistry:      // hives are not kept in files: nothing can be loaded, saved or replaced
        return windowsabi::nt_privilege_not_held(process, r);
      case WindowsSyscallNumber::NtNotifyChangeDirectoryFile: return windowsabi::nt_notify_change_directory_file(process, r);
      // A service of the list that has no code yet answers like a real kernel does for a call it does not support; a number outside the list is no service.
      default: process.host().unhandled_syscall(SyscallAbi::Windows, r.number); return is_service(r.number) ? kStatusNotImplemented : kStatusInvalidSystemService;
    }
  }
};

namespace {
constexpr WindowsSyscalls kInstance;
}  // namespace

const ISyscallInterface& windows_interface() { return kInstance; }

}  // namespace process
