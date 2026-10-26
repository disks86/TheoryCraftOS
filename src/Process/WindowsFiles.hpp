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

// The NT file services (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS. Arguments past the fourth are on the program's stack.
namespace process::windowsabi {

// The Nth argument (0-based) of a service: registers up to the fourth, the stack after that. False if the stack cannot be read.
bool argument(Process& p, const SyscallRequest& r, uint32_t index, uint64_t* out);

// Reads a UNICODE_STRING (Length, MaximumLength, Buffer) from the program into UTF-8. NTSTATUS.
uint32_t read_unicode_string_argument(Process& p, uint64_t address, String* out);

uint32_t nt_create_file(Process& p, const SyscallRequest& r);
uint32_t nt_open_file(Process& p, const SyscallRequest& r);
uint32_t nt_read_file(Process& p, const SyscallRequest& r);
uint32_t nt_write_file(Process& p, const SyscallRequest& r);
uint32_t nt_close(Process& p, const SyscallRequest& r);
uint32_t nt_query_information_file(Process& p, const SyscallRequest& r);
uint32_t nt_set_information_file(Process& p, const SyscallRequest& r);
uint32_t nt_query_directory_file(Process& p, const SyscallRequest& r);
uint32_t nt_query_attributes_file(Process& p, const SyscallRequest& r);
uint32_t nt_delete_file(Process& p, const SyscallRequest& r);
// NtCreateSymbolicLink(POBJECT_ATTRIBUTES LinkAttributes, PUNICODE_STRING Target, BOOLEAN TargetIsDirectory): this OS's own service, behind CreateSymbolicLinkW
// (CreateHardLinkW instead goes through NtSetInformationFile's FileLinkInformation, like MoveFileExW goes through FileRenameInformation).
uint32_t nt_create_symbolic_link(Process& p, const SyscallRequest& r);
uint32_t nt_flush_buffers_file(Process& p, const SyscallRequest& r);
uint32_t nt_query_volume_information_file(Process& p, const SyscallRequest& r);
// NtCreateChangeNotification(POBJECT_ATTRIBUTES DirectoryAttributes, ULONG WatchTree, ULONG NotifyFilter, PHANDLE Result): this OS's own service, behind
// FindFirstChangeNotification(W/A) - a handle that becomes signalled (like a manual-reset event) when a file or subdirectory is created, deleted, renamed or linked
// directly under the named directory, or anywhere under it if WatchTree is nonzero. FindNextChangeNotification is NtResetEvent on the same handle; FindCloseChangeNotification
// is NtClose. NotifyFilter is kept but not matched against: any of those changes signals the handle, regardless of which FILE_NOTIFY_CHANGE_* bits were asked for.
uint32_t nt_create_change_notification(Process& p, const SyscallRequest& r);
// NtNotifyChangeDirectoryFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
// ULONG CompletionFilter, BOOLEAN WatchTree): this OS's own real-named service, behind ReadDirectoryChangesW - unlike real Windows, synchronous (there is no real
// asynchronous I/O in this kernel): blocks until at least one change, fills Buffer with as many FILE_NOTIFY_INFORMATION records as fit and have already happened.
// FileHandle must already be an open directory handle; the watch (its directory, WatchTree, CompletionFilter) is made on the handle's first call and kept for its
// life. Event, if given, is set on success; ApcRoutine, if given, is queued as an ordinary APC (like ReadFileEx), run when the thread next waits alertably.
uint32_t nt_notify_change_directory_file(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
