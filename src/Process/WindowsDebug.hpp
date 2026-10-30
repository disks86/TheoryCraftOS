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

// Debugging, as the NT debug object has it. A debug object keeps a queue of events (a process or thread starting or ending, a library loaded or unloaded, an exception); a program
// attached to it (NtDebugActiveProcess) posts its events there and, for the ones that need an answer (exceptions), waits until the debugger continues it. The debugger takes them one
// at a time with NtWaitForDebugEvent, which fills a DBGUI_WAIT_STATE_CHANGE, and answers with NtDebugContinue. All of it is guarded by the system lock of the object manager.
// NtCreateDebugObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG Flags) is made with the other named objects (WindowsObjects.cpp), which tell us about it with debug_note_created().
uint32_t nt_debug_active_process(Process& p, const SyscallRequest& r);      // (Process, DebugObject)
uint32_t nt_remove_process_debug(Process& p, const SyscallRequest& r);      // (Process, DebugObject)
uint32_t nt_wait_for_debug_event(Process& p, const SyscallRequest& r);      // (DebugObject, Alertable, PLARGE_INTEGER Timeout, PDBGUI_WAIT_STATE_CHANGE)
uint32_t nt_debug_continue(Process& p, const SyscallRequest& r);            // (DebugObject, PCLIENT_ID, NTSTATUS)
uint32_t nt_set_information_debug_object(Process& p, const SyscallRequest& r);      // (DebugObject, Class, Buffer, Length, PULONG ReturnLength): class 1, the flags
// This OS's own service: the program reports an exception (the record is in its memory) to the debugger it is attached to and waits for the answer: DBG_CONTINUE (0x00010002) or
// DBG_EXCEPTION_NOT_HANDLED (0x80010001, also when nobody debugs it). NtTcosDebugException(PEXCEPTION_RECORD, BOOLEAN FirstChance).
uint32_t nt_tcos_debug_exception(Process& p, const SyscallRequest& r);
// NtSuspendProcess(Process) and NtResumeProcess(Process): every thread of another program is suspended / resumed once.
uint32_t nt_suspend_resume_process(Process& p, const SyscallRequest& r, bool suspend);

void debug_note_created(uint32_t object, uint32_t flags);
void debug_object_freed_locked(uint32_t object);
// The debug object (id of the object manager's object) a process is attached to, 0 if none.
uint32_t debug_object_of(uint64_t pid);
// The CONTEXT (address in the program) of a thread that waits for the debugger at an exception.
bool debug_stopped_context(uint64_t pid, uint64_t tid, uint64_t* address);
// Events the program itself causes.
void debug_thread_created(Process& p, uint64_t thread_id, uint64_t start);
void debug_thread_exited(Process& p, uint64_t thread_id, uint32_t code);
void debug_module_loaded(Process& p, uint64_t base);
void debug_module_unloaded(Process& p, uint64_t base);
void debug_process_exited(uint64_t pid, uint32_t code);
// A program that is debugged (with its children) starts another: does the child have to be attached (and so started suspended)? Then attach it.
bool debug_wants_child(uint64_t parent_pid);
void debug_set_inherit(uint64_t pid, bool inherit);
uint32_t debug_attach_child(Process& parent, uint64_t child_pid);

}  // namespace process::windowsabi
