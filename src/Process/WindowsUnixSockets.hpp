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
#include "Process/UnixSockets.hpp"

// AF_UNIX sockets for Windows programs (ws2_32.dll's AF_UNIX, <afunix.h>): the socket NT services (WindowsSockets.hpp) hand a handle of kind UnixSocket, and
// NtCreateSocket with family AF_UNIX (1), to these. The sockets are the system's unix sockets (UnixSockets.hpp), the same ones Linux programs use: a path in
// sun_path is a Windows path ("C:\\tmp\\app.sock", or relative to the current directory), and names the same file a Linux program calls
// "/mnt/c/tmp/app.sock". Addresses are the program's struct sockaddr_un bytes (UnixSocketAddress.hpp). Each returns an NTSTATUS (see WindowsSockets.hpp for
// the would-block status).
//
// NtSocketControl on a unix socket handle: 0 Shutdown, 1 SetNonBlocking, 4 BytesReadable and 7 GetSocketInfo (family 1, the type) as for the others;
//   2 GetSockName / 3 GetPeerName write the sockaddr_un (cut to OutLength) and, when In is a ULONG (InLength 4), its full size there;
//  11 GetPeerPid (Out: ULONG, the process id of the other end: SIO_AF_UNIX_GETPEERPID)
//  12 SetTimeout (In: ULONG which - 0 receiving, 1 sending - then ULONG milliseconds, 0 for none: SO_RCVTIMEO / SO_SNDTIMEO)
//  13 TakeError (Out: ULONG, the NTSTATUS of the pending error or 0: SO_ERROR)
namespace process::windowsabi {

uint32_t unix_create_socket(Process& p, uint32_t type, uint64_t handle_at);
uint32_t unix_bind_socket(Process& p, uint32_t id, uint64_t address, uint64_t length);
uint32_t unix_connect_socket(Process& p, uint32_t id, uint64_t address, uint64_t length);
uint32_t unix_listen_socket(Process& p, uint32_t id, uint32_t backlog);
uint32_t unix_accept_socket(Process& p, uint32_t id, uint64_t handle_at, uint64_t peer_at, uint64_t peer_length_at);
uint32_t unix_send_socket(Process& p, uint32_t id, uint64_t buffer, uint32_t length, uint64_t sent_at, uint64_t to_at, uint64_t to_length);
uint32_t unix_receive_socket(Process& p, uint32_t id, uint64_t buffer, uint32_t capacity, uint64_t got_at, uint64_t from_at, uint64_t from_length_at,
                             uint64_t peer_closed_at);
uint32_t unix_socket_control(Process& p, uint32_t id, uint32_t operation, uint64_t in, uint32_t in_length, uint64_t out, uint32_t out_length);
// NtSelectSockets: the state of a unix socket (the lock of `sockets` held). `which`: 0 readable, 1 writable, 2 an error.
bool unix_select_ready_locked(UnixSocketManager& sockets, uint32_t id, uint32_t which);
// The NTSTATUS for a unix socket status (the ones ws2_32 turns into the matching WSA errors).
uint32_t unix_ntstatus(UnixStatus status);

}  // namespace process::windowsabi
