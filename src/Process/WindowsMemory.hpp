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
#pragma once

#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsException.hpp"

namespace process::windowsabi {

// The access a PAGE_* constant grants, as the checks on sections and views see it (bit 1 read, 2 write, 4 execute): copy-on-write pages read, a page that can only execute
// reads as well, no access and anything unknown grant nothing.
constexpr uint8_t page_access(uint32_t page) {
  switch (page & 0xFF) {      // (the modifiers PAGE_GUARD, PAGE_NOCACHE and PAGE_WRITECOMBINE do not change the access)
    case 0x02: case 0x08: return 1;
    case 0x04: return 3;
    case 0x10: case 0x20: case 0x80: return 5;
    case 0x40: return 7;
    default: return 0;
  }
}

// The virtual memory services. Each takes a process handle: -1 (this process), or a handle to another running program (from CreateProcess or OpenProcess), whose memory is
// then reached through IProcessRuntime::remote_operation().
// NtAllocateVirtualMemory(Process, PVOID* Base, ULONG_PTR ZeroBits, PSIZE_T Size, ULONG Type, ULONG Protect), NtFreeVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG FreeType),
// NtProtectVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG NewProtect, PULONG OldProtect), NtQueryVirtualMemory(Process, BaseAddress, Class 0, Buffer, Length, ReturnLength),
// NtReadVirtualMemory(Process, BaseAddress, Buffer, Size, PSIZE_T Read) and NtWriteVirtualMemory(Process, BaseAddress, Buffer, Size, PSIZE_T Written). Reading and writing
// stop at the first page that is not committed and answer STATUS_PARTIAL_COPY (0x8000000D) with the count.
uint32_t nt_allocate_virtual_memory(Process& p, const SyscallRequest& r);
uint32_t nt_free_virtual_memory(Process& p, const SyscallRequest& r);
uint32_t nt_protect_virtual_memory(Process& p, const SyscallRequest& r);
uint32_t nt_query_virtual_memory(Process& p, const SyscallRequest& r);
uint32_t nt_read_virtual_memory(Process& p, const SyscallRequest& r);
uint32_t nt_write_virtual_memory(Process& p, const SyscallRequest& r);

// A page fault on a PAGE_GUARD page: the page gets its protection back. In a thread's stack (`teb` says where it is) the stack grows by a page and the guard moves down (Retry);
// elsewhere, and when the stack is used up, an exception is raised (STATUS_GUARD_PAGE_VIOLATION, STATUS_STACK_OVERFLOW) described by `info`.
enum class GuardResult { NotGuard, Retry, Exception };
GuardResult guard_fault(Process& process, uint64_t teb, uint64_t address, bool write, ExceptionInfo* info);

// What a remote operation does, on the process it was asked of (its lock is held by the caller).
uint32_t run_remote(Process& target, IProcessRuntime::RemoteOp& op);

}  // namespace process::windowsabi
