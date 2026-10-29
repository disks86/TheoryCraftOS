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

// Mailslots: CreateMailslot (the one reader) and CreateFile(\\.\mailslot\name) (a writer; an ordinary file service, like a pipe's client end). A mailslot handle is a
// descriptor of kind Mailslot; reading, writing, and info classes 26/27 go through the usual file services, which hand the Mailslot kind over to these functions.
namespace process::windowsabi {

// NtCreateMailslotFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG CreateOptions, ULONG MailslotQuota, ULONG MaximumMessageSize,
// PLARGE_INTEGER ReadTimeout): the one reader. MaximumMessageSize 0: no cap beyond this OS's own fixed per-mailslot buffer. ReadTimeout: a relative, negative 100ns
// value like every other timeout here (not real mailslots' own millisecond convention) - null waits forever, pointing at 0 does not wait at all.
uint32_t nt_create_mailslot_file(Process& p, const SyscallRequest& r);
// A path like \??\mailslot\name or \Device\Mailslot\name opens a writer: true if the name was one (*status and *handle are the answer).
bool mailslot_open_by_name(Process& p, uint64_t object_attributes, uint32_t access, uint32_t* status, uint64_t* handle);
// NtReadFile / NtWriteFile on a mailslot handle.
uint32_t mailslot_io(Process& p, uint64_t slot, bool write, uint64_t status_block, uint64_t buffer, uint64_t length);
// NtQueryInformationFile / NtSetInformationFile classes 26 (FileMailslotQueryInformation) and 27 (FileMailslotSetInformation) on a mailslot handle.
uint32_t mailslot_query_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t status_block, uint64_t buffer, uint32_t length);
uint32_t mailslot_set_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length);
// What closing, duplicating or inheriting a mailslot handle does: the reader's own handle closing destroys the mailslot; either end's token is reference-counted.
// An asynchronous reader (CreateOptions without FILE_SYNCHRONOUS_IO) whose NtReadFile found no message gets STATUS_PENDING; the read is finished when the owner waits on the
// handle: ready (the system lock held) says whether a message has come for the read pending on this handle, complete takes it into the buffer and the status block.
bool mailslot_read_ready_locked(Process& p, uint64_t slot);
uint32_t mailslot_complete_read(Process& p, uint64_t slot);
void mailslot_token_add(uint64_t token);
void mailslot_token_release(uint64_t token);

}  // namespace process::windowsabi
