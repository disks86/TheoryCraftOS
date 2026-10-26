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
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

// The Windows system-call interface: the NT native services a PE program reaches through ntdll.dll. A PE program never executes SYSCALL itself; the
// ntdll we load into it holds the stubs ("mov r10, rcx; mov eax, number; syscall; ret"), so the numbers below are this OS's own (see
// WindowsSyscallNumber), not those of any one Windows build, which change from release to release. Results are NTSTATUS values.
namespace process {

namespace {

constexpr uint32_t kStatusSuccess = 0, kStatusNoYield = 0x40000024u, kStatusNotImplemented = 0xC0000002u, kStatusInvalidHandle = 0xC0000008u,
                   kStatusInvalidParameter = 0xC000000Du, kStatusAccessViolation = 0xC0000005u, kStatusEndOfFile = 0xC0000011u, kStatusAccessDenied = 0xC0000022u,
                   kStatusNoMemory = 0xC0000017u, kStatusConflictingAddresses = 0xC0000018u, kStatusUnableToFreeVm = 0xC000001Au, kStatusInvalidPageProtection = 0xC0000045u;
constexpr uint64_t kCurrentProcess = ~uint64_t{0};      // NtCurrentProcess()
constexpr uint64_t kStackArgumentBase = 0x28;           // return address + 32 bytes of home space
constexpr uint64_t kUnixToNtSeconds = 11644473600ull;   // 1601-01-01 to 1970-01-01
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u;

bool argument(Process& process, const SyscallRequest& r, uint32_t index, uint64_t* out) { return windowsabi::argument(process, r, index, out); }

uint64_t nt_time_now(Process& process) { return (process.host().realtime_ns() / 100) + kUnixToNtSeconds * 10000000ull; }      // 100 ns units since 1601

uint32_t nt_query_system_time(Process& process, const SyscallRequest& r) {
  return process.write_user_u64(r.args[0], nt_time_now(process)) ? kStatusSuccess : kStatusAccessViolation;
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
  process.host().sleep_ns(units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100);
  return kStatusSuccess;
}

constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000;
constexpr uint32_t kPageNoAccess = 0x01, kPageReadOnly = 0x02, kPageReadWrite = 0x04, kPageWriteCopy = 0x08, kPageExecute = 0x10, kPageExecuteRead = 0x20,
                   kPageExecuteReadWrite = 0x40, kPageExecuteWriteCopy = 0x80;
constexpr uint64_t kAllocationGranularity = 0x10000;

// PAGE_* -> our permission bits. Modifier flags (guard, no-cache, write-combine) are not supported.
bool page_protection(uint32_t page, uint8_t* out) {
  switch (page) {
    case kPageNoAccess: *out = 0; return true;
    case kPageReadOnly: *out = kProtRead; return true;
    case kPageReadWrite:
    case kPageWriteCopy: *out = kProtRead | kProtWrite; return true;
    case kPageExecute: *out = kProtExecute; return true;
    case kPageExecuteRead: *out = kProtRead | kProtExecute; return true;
    case kPageExecuteReadWrite:
    case kPageExecuteWriteCopy: *out = kProtRead | kProtWrite | kProtExecute; return true;
    default: return false;
  }
}
uint32_t page_constant(uint8_t protection) {
  switch (protection) {
    case 0: return kPageNoAccess;
    case kProtRead: return kPageReadOnly;
    case kProtRead | kProtWrite: return kPageReadWrite;
    case kProtWrite: return kPageReadWrite;
    case kProtExecute: return kPageExecute;
    case kProtRead | kProtExecute: return kPageExecuteRead;
    default: return kPageExecuteReadWrite;
  }
}

bool current_process_handle(uint64_t handle) { return handle == kCurrentProcess; }

// NtAllocateVirtualMemory(Process, PVOID* Base, ULONG_PTR ZeroBits, PSIZE_T Size, ULONG Type, ULONG Protect)
uint32_t nt_allocate_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t type = 0, protect = 0;
  if (!argument(process, r, 4, &type) || !argument(process, r, 5, &protect)) return kStatusAccessViolation;
  AddressSpace* space = process.address_space();
  if (!current_process_handle(r.args[0])) return kStatusInvalidHandle;
  uint64_t base = 0, size = 0;
  if (!process.read_user_u64(r.args[1], &base) || !process.read_user_u64(r.args[3], &size)) return kStatusAccessViolation;
  uint8_t prot = 0;
  type &= 0xFFFFFFFFu;
  if (!(type & (kMemCommit | kMemReserve)) || (type & ~uint64_t{kMemCommit | kMemReserve | 0x100000 /*MEM_TOP_DOWN: the search direction is ours to choose*/})) return kStatusInvalidParameter;
  if (!page_protection(uint32_t(protect), &prot)) return kStatusInvalidPageProtection;
  if (!space) return kStatusNoMemory;
  if (size == 0) return kStatusInvalidParameter;
  if (!(type & kMemCommit)) prot = 0;      // reserved memory cannot be touched until it is committed
  uint64_t rounded = 0;
  if (!AddressSpace::round_up(size, &rounded)) return kStatusInvalidParameter;
  if (base != 0) {
    base = AddressSpace::round_down(base);
    if (space->check(base, rounded, 0)) {
      // Committing pages that were reserved earlier.
      if (!(type & kMemCommit) || !space->protect(base, rounded, prot)) return kStatusConflictingAddresses;
    } else if (!space->map(base, rounded, prot)) {
      return kStatusConflictingAddresses;
    }
  } else {
    uint64_t at = 0;
    const uint64_t hint = (process.mmap_cursor() + kAllocationGranularity - 1) & ~(kAllocationGranularity - 1);
    if (!space->find_free(rounded, hint, &at)) return kStatusNoMemory;
    at = (at + kAllocationGranularity - 1) & ~(kAllocationGranularity - 1);      // reservations begin on the allocation granularity
    if (!space->map(at, rounded, prot)) return kStatusNoMemory;
    process.set_mmap_cursor(at + rounded);
    base = at;
  }
  if (!process.write_user_u64(r.args[1], base) || !process.write_user_u64(r.args[3], rounded)) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtFreeVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG FreeType)
uint32_t nt_free_virtual_memory(Process& process, const SyscallRequest& r) {
  if (!current_process_handle(r.args[0])) return kStatusInvalidHandle;
  AddressSpace* space = process.address_space();
  uint64_t base = 0, size = 0;
  if (!process.read_user_u64(r.args[1], &base) || !process.read_user_u64(r.args[2], &size)) return kStatusAccessViolation;
  const uint32_t type = uint32_t(r.args[3]);
  if (!space) return kStatusNoMemory;
  if (type == kMemRelease) {
    // Releases the whole allocation `base` is the start of (as NtAllocateVirtualMemory returned it); the size must be 0.
    uint64_t region_base = 0, region_size = 0;
    if (size != 0) return kStatusInvalidParameter;
    if (!space->allocation_extent(base, &region_base, &region_size) || region_base != base) return kStatusUnableToFreeVm;
    if (!space->unmap(region_base, region_size)) return kStatusUnableToFreeVm;
    return process.write_user_u64(r.args[1], region_base) && process.write_user_u64(r.args[2], region_size) ? kStatusSuccess : kStatusAccessViolation;
  }
  if (type == kMemDecommit) {
    uint64_t rounded = 0;
    base = AddressSpace::round_down(base);
    if (size == 0 || !AddressSpace::round_up(size, &rounded)) return kStatusInvalidParameter;
    if (!space->protect(base, rounded, 0)) return kStatusUnableToFreeVm;      // decommitted pages become inaccessible (their contents are not cleared)
    return process.write_user_u64(r.args[1], base) && process.write_user_u64(r.args[2], rounded) ? kStatusSuccess : kStatusAccessViolation;
  }
  return kStatusInvalidParameter;
}

// NtProtectVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG NewProtect, PULONG OldProtect)
uint32_t nt_protect_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t old_pointer = 0;
  if (!argument(process, r, 4, &old_pointer)) return kStatusAccessViolation;
  if (!current_process_handle(r.args[0])) return kStatusInvalidHandle;
  AddressSpace* space = process.address_space();
  uint64_t base = 0, size = 0;
  if (!process.read_user_u64(r.args[1], &base) || !process.read_user_u64(r.args[2], &size)) return kStatusAccessViolation;
  uint8_t prot = 0;
  if (!page_protection(uint32_t(r.args[3]), &prot)) return kStatusInvalidPageProtection;
  if (!space) return kStatusNoMemory;
  uint64_t rounded = 0;
  base = AddressSpace::round_down(base);
  if (size == 0 || !AddressSpace::round_up(size, &rounded)) return kStatusInvalidParameter;
  uint8_t old = 0;
  if (!space->protection_at(base, &old)) return kStatusConflictingAddresses;
  uint8_t probe[4] = {};
  const uint32_t old_page = page_constant(old);
  for (uint32_t i = 0; i < 4; ++i) probe[i] = uint8_t(old_page >> (8 * i));
  if (!process.write_user(old_pointer, ConstByteView(probe, 4))) return kStatusAccessViolation;
  if (!space->protect(base, rounded, prot)) return kStatusConflictingAddresses;
  return process.write_user_u64(r.args[1], base) && process.write_user_u64(r.args[2], rounded) ? kStatusSuccess : kStatusAccessViolation;
}

// NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency): 100 ns ticks, so the frequency is 10 MHz.
uint32_t nt_query_performance_counter(Process& process, const SyscallRequest& r) {
  if (r.args[0] == 0 || !process.write_user_u64(r.args[0], process.host().monotonic_ns() / 100)) return kStatusAccessViolation;
  if (r.args[1] != 0 && !process.write_user_u64(r.args[1], 10000000ull)) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtQueryVirtualMemory(Process, BaseAddress, MemoryBasicInformation = 0, Buffer, Length, ReturnLength): MEMORY_BASIC_INFORMATION (48 bytes) for the page.
uint32_t nt_query_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t length = 0, returned = 0;
  if (!argument(process, r, 4, &length) || !argument(process, r, 5, &returned)) return kStatusAccessViolation;
  if (!current_process_handle(r.args[0])) return kStatusInvalidHandle;
  if ((r.args[2] & 0xFFFFFFFFu) != 0) return 0xC0000003u;      // STATUS_INVALID_INFO_CLASS (a ULONG: mask the undefined upper half)
  if (length < 48) return 0xC0000004u;         // STATUS_INFO_LENGTH_MISMATCH
  AddressSpace* space = process.address_space();
  if (!space) return kStatusNoMemory;
  const uint64_t page = AddressSpace::round_down(r.args[1]);
  if (page >= AddressSpace::kUserTop) return kStatusInvalidParameter;
  uint8_t out[48] = {};
  auto put64 = [&](uint32_t at, uint64_t v) { for (uint32_t i = 0; i < 8; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  auto put32 = [&](uint32_t at, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  uint64_t base = 0, size = 0;
  uint8_t protection = 0;
  if (space->region_extent(page, &base, &size) && space->protection_at(page, &protection)) {
    uint64_t allocation = 0, allocation_size = 0;
    if (!space->allocation_extent(page, &allocation, &allocation_size)) allocation = base;
    put64(0, page);
    put64(8, allocation);
    put32(16, page_constant(protection));      // AllocationProtect
    put64(24, base + size - page);             // RegionSize: to the end of this run of pages
    put32(32, protection == 0 ? 0x2000 : 0x1000);      // State: MEM_RESERVE for pages nobody may touch, else MEM_COMMIT
    put32(36, page_constant(protection));      // Protect
    put32(40, 0x20000);                        // Type: MEM_PRIVATE
  } else {
    // Free: up to the next mapped page.
    uint64_t next = AddressSpace::kUserTop;
    AddressSpace::RegionInfo region;
    for (uint64_t i = 0; space->region_at(i, &region); ++i) {
      if (region.base > page) { next = region.base; break; }
    }
    put64(0, page);
    put64(24, next - page);
    put32(32, 0x10000);      // MEM_FREE
    put32(36, 0x01);         // PAGE_NOACCESS
  }
  if (!process.write_user(r.args[3], ConstByteView(out, 48))) return kStatusAccessViolation;
  if (returned != 0 && !process.write_user_u64(returned, 48)) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtQueryInformationProcess(Process, Class, Buffer, Length, ReturnLength): ProcessBasicInformation (0) and ProcessWow64Information (26).
uint32_t nt_query_information_process(Process& process, const SyscallRequest& r) {
  uint64_t returned = 0;
  if (!argument(process, r, 4, &returned)) return kStatusAccessViolation;
  if (!current_process_handle(r.args[0])) return kStatusInvalidHandle;
  uint64_t size = 0;
  uint8_t out[48] = {};
  auto put64 = [&](uint32_t at, uint64_t v) { for (uint32_t i = 0; i < 8; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  const uint32_t info_class = uint32_t(r.args[1]);
  const uint32_t buffer_length = uint32_t(r.args[3]);      // (ULONGs: the upper halves of the arguments are not defined)
  if (info_class == 0) {
    uint64_t peb = 0;
    process.read_user_u64(process.gs_base() + 0x60, &peb);
    put64(8, peb);            // PebBaseAddress
    put64(16, 1);             // AffinityMask
    put64(24, 8);             // BasePriority
    put64(32, process.id());  // UniqueProcessId
    size = 48;
  } else if (info_class == 26) {
    size = 8;                 // not a 32-bit process on a 64-bit system
  } else {
    return 0xC0000003u;       // STATUS_INVALID_INFO_CLASS
  }
  if (buffer_length < size) return 0xC0000004u;
  if (!process.write_user(r.args[2], ConstByteView(out, size))) return kStatusAccessViolation;
  if (returned != 0 && !process.write_user_u64(returned, size)) return kStatusAccessViolation;
  return kStatusSuccess;
}

}  // namespace

class WindowsSyscalls final : public ISyscallInterface {
 public:
  SyscallAbi abi() const override { return SyscallAbi::Windows; }
  UserArch arch() const override { return UserArch::X86_64; }

  uint64_t dispatch(Process& process, const SyscallRequest& r) const override {
    if (r.number > 0xFFFFFFFFu) return kStatusInvalidSystemService;
    switch (static_cast<WindowsSyscallNumber>(r.number)) {
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
      case WindowsSyscallNumber::NtQueryVirtualMemory: return nt_query_virtual_memory(process, r);
      case WindowsSyscallNumber::NtQueryInformationProcess: return nt_query_information_process(process, r);
      case WindowsSyscallNumber::NtTerminateProcess:
        if (r.args[0] != kCurrentProcess) return kStatusInvalidHandle;      // other processes are not reachable yet
        process.terminate(uint32_t(r.args[1]));
        return kStatusSuccess;
      case WindowsSyscallNumber::NtYieldExecution: process.host().yield(); return kStatusSuccess;
      case WindowsSyscallNumber::NtDelayExecution: return nt_delay_execution(process, r);
      case WindowsSyscallNumber::NtQuerySystemTime: return nt_query_system_time(process, r);
      case WindowsSyscallNumber::NtAllocateVirtualMemory: return nt_allocate_virtual_memory(process, r);
      case WindowsSyscallNumber::NtFreeVirtualMemory: return nt_free_virtual_memory(process, r);
      case WindowsSyscallNumber::NtProtectVirtualMemory: return nt_protect_virtual_memory(process, r);
      default: process.host().unhandled_syscall(SyscallAbi::Windows, r.number); return kStatusInvalidSystemService;
    }
  }
};

namespace {
constexpr WindowsSyscalls kInstance;
}  // namespace

const ISyscallInterface& windows_interface() { return kInstance; }

}  // namespace process
