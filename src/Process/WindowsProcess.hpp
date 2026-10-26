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

// The NT services for other processes (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

// NtCreateUserProcess(PHANDLE Process, PHANDLE Thread, PUNICODE_STRING Image, PUNICODE_STRING CommandLine, PUNICODE_STRING CurrentDirectory, PVOID Environment,
//                     ULONG Flags, PULONG ProcessId, PULONG ThreadId): a new process runs the program in the file `Image` (a Win32 path). `Environment` is a block of
// NUL-terminated UTF-16 "NAME=value" strings ended by an empty one (0: none). Flags bit 0: do not start it until its thread is resumed. Both handles can be waited for;
// the thread handle can be resumed; the process handle answers NtQueryInformationProcess and NtTerminateProcess.
uint32_t nt_create_user_process(Process& p, const SyscallRequest& r);
// A handle to a process created by this system (or 0 if the handle names something else), and what it is.
bool process_handle_info(Process& p, uint64_t handle, bool* exited, uint32_t* code, uint64_t* pid);
// NtTerminateProcess / NtResumeThread on a handle from nt_create_user_process; `handled` is false if the handle is not one.
uint32_t terminate_process_handle(Process& p, uint64_t handle, uint32_t code, bool* handled);
uint32_t resume_process_handle(Process& p, uint64_t handle, uint32_t* previous, bool* handled);

}  // namespace process::windowsabi
