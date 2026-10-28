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
#include "Utility/ByteBuffer.hpp"

// The NT services for other processes (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

// NtCreateUserProcess(PHANDLE Process, PHANDLE Thread, PUNICODE_STRING Image, PUNICODE_STRING CommandLine, PUNICODE_STRING CurrentDirectory, PVOID Environment,
//                     ULONG Flags, PULONG ProcessId, PULONG ThreadId, HANDLE StdInput, HANDLE StdOutput, HANDLE StdError): a new process runs the program in the file `Image` (a Win32 path). `Environment` is a block of
// NUL-terminated UTF-16 "NAME=value" strings ended by an empty one (0: none). Flags bit 0: do not start it until its thread is resumed; bit 1: it inherits the handles that are inheritable; bit 2: its standard handles are StdInput, StdOutput
// and StdError (handles of the caller that it inherits, or the console's). Both handles can be waited for;
// the thread handle can be resumed; the process handle answers NtQueryInformationProcess and NtTerminateProcess.
uint32_t nt_create_user_process(Process& p, const SyscallRequest& r);
// NtQueryInformationProcess(Process, Class, Buffer, Length, ReturnLength) and NtSetInformationProcess(Process, Class, Buffer, Length). For the calling process (-1): basic
// information (0), I/O counters (2), memory counters (3), times (4), debug port (7), error mode (12, also set), priority class (18, also set), handle count (20), affinity (21,
// also set), session (24), WOW64 (26), image file name (27), debug object (30) and flags (31), cookie (36). For a process handle from nt_create_user_process: basic
// information (the exit status).
uint32_t nt_query_information_process(Process& p, const SyscallRequest& r);
uint32_t nt_set_information_process(Process& p, const SyscallRequest& r);
uint32_t nt_open_process(Process& p, const SyscallRequest& r);
// The whole file at a Win32 path of the program's world, into `out`; on failure an NTSTATUS in *status.
bool read_program_file(Process& p, StringView path, ByteBuffer* out, uint32_t* status);
// A handle to a process created by this system (or 0 if the handle names something else), and what it is.
bool process_handle_info(Process& p, uint64_t handle, bool* exited, uint32_t* code, uint64_t* pid);
// True for the pseudo handle (-1) and for a handle opened on this process's own id.
bool names_own_process(Process& p, uint64_t handle);
// NtTerminateProcess / NtResumeThread on a handle from nt_create_user_process; `handled` is false if the handle is not one.
uint32_t terminate_process_handle(Process& p, uint64_t handle, uint32_t code, bool* handled);
// The kernel scheduler's absolute priority (0..31) for a thread whose process has the Win32 priority class `win32_class` (one of the *_PRIORITY_CLASS constants) and whose
// own relative priority (SetThreadPriority's argument, -16..16) is `relative`. Used both when a thread's own priority changes and, since every thread's absolute priority
// depends on its process's class, when the class itself does.
int32_t absolute_thread_priority(uint32_t win32_class, int32_t relative);
// Pushes every one of this process's existing threads' absolute priority down to the kernel scheduler again (the process's priority class just changed).
void repriority_threads(Process& p);
uint32_t resume_process_handle(Process& p, uint64_t handle, uint32_t* previous, bool* handled);

}  // namespace process::windowsabi
