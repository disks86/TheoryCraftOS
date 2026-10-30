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

// Pipes: anonymous (CreatePipe) and named (\\.\pipe\name, CreateNamedPipe and CreateFile on it). A pipe handle is a descriptor of kind Pipe; reading, writing, FSCTLs and
// file information go through the usual file services, which hand the Pipe kind over to these functions. Each returns an NTSTATUS.
namespace process::windowsabi {

// NtCreatePipe(PHANDLE Read, PHANDLE Write, POBJECT_ATTRIBUTES (or 0; OBJ_INHERIT makes both handles inheritable), ULONG Size): an anonymous pipe. (Our own service: NT makes
// anonymous pipes out of a named pipe with a made-up name.)
uint32_t nt_create_pipe(Process& p, const SyscallRequest& r);
// NtCreateNamedPipeFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG ShareAccess, ULONG Disposition, ULONG Options, ULONG NamedPipeType,
// ULONG ReadMode, ULONG CompletionMode, ULONG MaximumInstances, ULONG InboundQuota, ULONG OutboundQuota, PLARGE_INTEGER DefaultTimeout): one instance of a named pipe;
// ShareAccess says the direction (FILE_SHARE_READ: the server writes, FILE_SHARE_WRITE: the server reads, both: duplex).
uint32_t nt_create_named_pipe_file(Process& p, const SyscallRequest& r);
// The name of the pipe a pipe handle is for, in UTF-8 and without the folder (empty for an anonymous pipe or the folder).
bool pipe_handle_name(Process& p, uint64_t slot, String* name);
// NtFsControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FsControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength): on a pipe,
// FSCTL_PIPE_LISTEN, _DISCONNECT, _PEEK, _WAIT and _TRANSCEIVE.
uint32_t nt_fs_control_file(Process& p, const SyscallRequest& r);

// For the file services:
// A path like \??\pipe\name or \Device\NamedPipe\name opens a pipe (as a client) or the pipe folder: true if the name was one (*status and *handle are the answer).
bool pipe_open_by_name(Process& p, uint64_t object_attributes, uint32_t access, uint32_t* status, uint64_t* handle);
// NtReadFile / NtWriteFile on a pipe handle.
uint32_t pipe_io(Process& p, uint64_t slot, bool write, uint64_t status_block, uint64_t buffer, uint64_t length);
// NtQueryInformationFile / NtSetInformationFile classes 23 (FilePipeInformation) and 24 (FilePipeLocalInformation) on a pipe handle.
uint32_t pipe_query_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t status_block, uint64_t buffer, uint32_t length);
uint32_t pipe_set_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length);
// NtFlushBuffersFile: waits until the other end has read everything.
uint32_t pipe_flush(Process& p, uint64_t slot);
// What closing, duplicating or inheriting a pipe handle does to the pipe's ends (the token is the descriptor's).
// A client end of a pipe whose server disconnected it is not signalled (the system lock held).
bool pipe_handle_gone_locked(Process& p, uint64_t slot);
void pipe_token_add(uint64_t token);
void pipe_token_release(uint64_t token);

}  // namespace process::windowsabi
