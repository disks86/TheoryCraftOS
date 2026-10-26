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
// Sections: NtCreateSection, NtOpenSection, NtMapViewOfSection, NtUnmapViewOfSection, NtFlushVirtualMemory.
uint32_t nt_create_section(Process& p, const SyscallRequest& r);
uint32_t nt_open_section(Process& p, const SyscallRequest& r);
uint32_t nt_map_view_of_section(Process& p, const SyscallRequest& r);
uint32_t nt_unmap_view_of_section(Process& p, const SyscallRequest& r);
uint32_t nt_flush_virtual_memory(Process& p, const SyscallRequest& r);
// NtQueryObject(Handle, Class, Buffer, Length, ReturnLength) and NtSetInformationObject(Handle, Class, Buffer, Length): basic information, the type name and the
// handle flags (inherit, protect from close).
uint32_t nt_query_object(Process& p, const SyscallRequest& r);
uint32_t nt_set_information_object(Process& p, const SyscallRequest& r);
// NtSignalAndWaitForSingleObject(SignalHandle, WaitHandle, Alertable, Timeout).
uint32_t nt_signal_and_wait(Process& p, const SyscallRequest& r);
// Waitable timers: NtCreateTimer, NtOpenTimer, NtSetTimer (an APC routine runs in the thread that set the timer, while it waits alertably), NtCancelTimer, NtQueryTimer.
uint32_t nt_create_timer(Process& p, const SyscallRequest& r);
uint32_t nt_open_timer(Process& p, const SyscallRequest& r);
uint32_t nt_set_timer(Process& p, const SyscallRequest& r);
uint32_t nt_cancel_timer(Process& p, const SyscallRequest& r);
uint32_t nt_query_timer(Process& p, const SyscallRequest& r);
// NtDelayExecution of an alertable sleep: ends early with STATUS_USER_APC (0xC0) when an APC is queued for the thread.
uint32_t alertable_delay(Process& p, uint64_t nanoseconds);
// NtDuplicateObject(SourceProcess, SourceHandle, TargetProcess, PHANDLE Target, Access, Attributes, Options): within the calling process.
uint32_t nt_duplicate_object(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
