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

namespace process::windowsabi {

// Job objects. NtCreateJobObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) and NtOpenJobObject are in WindowsObjects.cpp (they are like the other named objects).
// NtAssignProcessToJobObject(Job, Process), NtTerminateJobObject(Job, ExitStatus), NtIsProcessInJob(Process, Job or 0), NtQueryInformationJobObject(Job, Class, Buffer, Length,
// PULONG ReturnLength) and NtSetInformationJobObject(Job, Class, Buffer, Length). Classes: 1 basic accounting, 2 basic limits, 3 process id list, 4 basic UI restrictions,
// 8 basic and I/O accounting, 9 extended limits (query and set); 6 (end of job time) is accepted on set. A completion port (class 7) is not supported.
uint32_t nt_assign_process_to_job_object(Process& p, const SyscallRequest& r);
uint32_t nt_terminate_job_object(Process& p, const SyscallRequest& r);
uint32_t nt_is_process_in_job(Process& p, const SyscallRequest& r);
uint32_t nt_query_information_job_object(Process& p, const SyscallRequest& r);
uint32_t nt_set_information_job_object(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
