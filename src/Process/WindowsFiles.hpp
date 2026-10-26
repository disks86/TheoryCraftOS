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

}  // namespace process::windowsabi
