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

// The NT services for synchronization objects, waiting and handle duplication (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

uint32_t nt_create_event(Process& p, const SyscallRequest& r);
uint32_t nt_open_event(Process& p, const SyscallRequest& r);
uint32_t nt_set_event(Process& p, const SyscallRequest& r);
uint32_t nt_reset_event(Process& p, const SyscallRequest& r);
uint32_t nt_pulse_event(Process& p, const SyscallRequest& r);
uint32_t nt_create_mutant(Process& p, const SyscallRequest& r);
uint32_t nt_open_mutant(Process& p, const SyscallRequest& r);
uint32_t nt_release_mutant(Process& p, const SyscallRequest& r);
uint32_t nt_create_semaphore(Process& p, const SyscallRequest& r);
uint32_t nt_open_semaphore(Process& p, const SyscallRequest& r);
uint32_t nt_release_semaphore(Process& p, const SyscallRequest& r);
// NtWaitForSingleObject(Handle, Alertable, PLARGE_INTEGER Timeout) and NtWaitForMultipleObjects(Count, Handles, WaitType, Alertable, Timeout): threads, events, mutexes and
// semaphores, of this process or shared with others.
uint32_t nt_wait_for_single_object(Process& p, const SyscallRequest& r);
uint32_t nt_wait_for_multiple_objects(Process& p, const SyscallRequest& r);
// NtDuplicateObject(SourceProcess, SourceHandle, TargetProcess, PHANDLE Target, Access, Attributes, Options): within the calling process.
uint32_t nt_duplicate_object(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
