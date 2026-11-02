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
#include "Process/IoRequest.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// Asynchronous I/O of the NT services: what NtReadFile / NtWriteFile / NtFsControlFile do with their event, APC routine and context; the requests of calls that wait in the kernel
// (IoRequest.hpp), applied to the memory of the process that made them; cancelling; and the I/O completion ports.
namespace process::windowsabi {

// What a call was given besides its buffers.
struct IoCall {
  uint64_t event_handle = 0, apc = 0, context = 0, status_block = 0;
  uint32_t event_object = 0;      // the event, once io_call_begin has found it
  uint64_t thread = 0;            // the calling thread
  bool wait = false;              // the library asked for the call to wait whatever the handle was opened for (an event handle of 1)
  bool saved = false;             // the IO_STATUS_BLOCK as it was, kept for a call that fails
  uint8_t status_block_before[16] = {};
};

// The arguments of a call (event, APC routine, APC context, IO_STATUS_BLOCK are the 2nd to 5th of NtReadFile, NtWriteFile and NtFsControlFile). NTSTATUS: a call whose handle is
// right but whose event is not fails before it does anything (and leaves everything as it was).
uint32_t io_call_begin(Process& p, uint64_t slot, const SyscallRequest& r, IoCall* call);
// The end of a call that did not wait (`status` is its result): a call that failed leaves the IO_STATUS_BLOCK as it was; one that went through signals the event, queues the
// APC and tells the port. STATUS_PENDING is passed through. Returns `status`.
uint32_t io_call_end(Process& p, uint64_t slot, const IoCall& call, uint32_t status);
// The completion port a handle is tied to (NtSetInformationFile FileCompletionInformation), with the key and the FILE_SKIP_* flags.
bool io_handle_port(Process& p, uint64_t slot, uint32_t* port, uint64_t* key, uint32_t* flags);
// The call ended before it returned (`status` is a success or a warning): the event is set, the APC queued for the calling thread, the port told.
void io_call_completed(Process& p, uint64_t slot, const IoCall& call, uint32_t status, uint64_t information);
// A request for a call that has to wait, to be queued on the object with ObjectManager::io_add_locked. `buffer` is where a read's data go.
io::IoRequest io_new_request(Process& p, uint64_t slot, const IoCall& call, uint64_t buffer, uint32_t buffer_length);
// Applies the requests of this process that finished since: the IO_STATUS_BLOCK and the buffer of a read are written, the event is set, the APC is queued for the thread that
// made the call, the port gets its packet. Called with the process locked, at the start and the end of every system call and whenever a thread of the process wakes from a wait.
void io_reap(Process& p);
inline void io_reap_if_needed(Process& p);

// NtCancelIoFile(FileHandle, PIO_STATUS_BLOCK): the calls of the calling thread on the file object end with STATUS_CANCELLED. NtCancelIoFileEx(FileHandle, PIO_STATUS_BLOCK ToCancel,
// PIO_STATUS_BLOCK): the calls of any thread of the process (only the one with that IO_STATUS_BLOCK if given); STATUS_NOT_FOUND if none.
uint32_t nt_cancel_io_file(Process& p, const SyscallRequest& r);
uint32_t nt_cancel_io_file_ex(Process& p, const SyscallRequest& r);

// Completion ports. NtSetIoCompletion(Port, Key, ApcContext, IoStatus, IoStatusInformation), NtSetIoCompletionEx(Port, Reserve, Key, ApcContext, IoStatus, Information),
// NtRemoveIoCompletion(Port, PVOID* Key, PVOID* ApcContext, PIO_STATUS_BLOCK, PLARGE_INTEGER Timeout), NtRemoveIoCompletionEx(Port, FILE_IO_COMPLETION_INFORMATION*, Count, PULONG
// Removed, PLARGE_INTEGER Timeout, BOOLEAN Alertable), NtQueryIoCompletion(Port, Class, Buffer, Length, PULONG Returned).
uint32_t nt_set_io_completion(Process& p, const SyscallRequest& r);
uint32_t nt_set_io_completion_ex(Process& p, const SyscallRequest& r);
uint32_t nt_remove_io_completion(Process& p, const SyscallRequest& r);
uint32_t nt_remove_io_completion_ex(Process& p, const SyscallRequest& r);
uint32_t nt_query_io_completion(Process& p, const SyscallRequest& r);
// NtSetInformationFile classes FileCompletionInformation (30) and FileIoCompletionNotificationInformation (41) and the query of the latter: false if the class is not one of these.
bool io_set_file_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length, uint32_t* status);
bool io_query_file_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length, uint32_t* status, uint64_t* size);

inline void io_reap_if_needed(Process& p) {
  ObjectManager* objects = system_objects();
  if (objects && objects->io_ready() != 0) io_reap(p);
}

}  // namespace process::windowsabi
