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

}  // namespace process::windowsabi
