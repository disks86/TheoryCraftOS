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

// The NT services for threads (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

uint32_t nt_create_thread_ex(Process& p, const SyscallRequest& r);
uint32_t nt_terminate_thread(Process& p, const SyscallRequest& r);
// NtTerminateProcess(NULL, code): every thread of the process but the caller ends with `code`, and no thread can be made afterwards.
uint32_t terminate_other_threads(Process& p, uint32_t code);
uint32_t nt_resume_thread(Process& p, const SyscallRequest& r);
uint32_t nt_suspend_thread(Process& p, const SyscallRequest& r);
uint32_t nt_get_context_thread(Process& p, const SyscallRequest& r);
uint32_t nt_set_context_thread(Process& p, const SyscallRequest& r);
// The thread has ended (for whatever reason): releases what it had and wakes whoever waits (see the .cpp).
void end_thread(Process& p, Process::ThreadRecord& record, uint32_t code);
uint32_t nt_query_information_thread(Process& p, const SyscallRequest& r);
uint32_t nt_queue_apc_thread(Process& p, const SyscallRequest& r);
uint32_t nt_dequeue_apc(Process& p, const SyscallRequest& r);
uint32_t nt_open_thread(Process& p, const SyscallRequest& r);
uint32_t nt_set_information_thread(Process& p, const SyscallRequest& r);
// Threads of another program (CreateRemoteThread): called on that program, with its lock held (see IProcessRuntime::remote_operation). The thread is started (or left suspended); the
// caller gets a handle to it that can be waited for.
uint32_t create_thread_in(Process& target, uint64_t start, uint64_t argument, bool suspended, uint64_t stack_size, uint64_t* thread_id);
uint32_t thread_status_in(Process& target, uint64_t thread_id, uint64_t* exit_code);      // 259 (STILL_ACTIVE) if it runs
uint32_t terminate_thread_in(Process& target, uint64_t thread_id, uint32_t code);
uint32_t context_in(Process& target, uint64_t thread_id, bool set, X86Context* context);      // the registers of a thread that has not run or is parked
uint32_t resume_thread_in(Process& target, uint64_t thread_id, uint64_t* previous_count);
// A handle to a thread of another program (what CreateRemoteThread gave out) names a thread id; false if the handle is something else.
bool remote_thread_handle(Process& p, uint64_t handle, uint64_t* thread_id);

}  // namespace process::windowsabi
