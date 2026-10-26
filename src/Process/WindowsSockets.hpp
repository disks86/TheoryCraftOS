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

// IPv4 sockets (ws2_32.dll's services), all this OS's own: real Windows has no NT services for sockets at all (Winsock talks to a device driver, afd.sys, through its own
// ioctl protocol). A socket handle is a descriptor of kind Socket, closed the usual way (NtClose); everything else about it goes through these. Each returns an NTSTATUS;
// 0xC0000103 (shared with "not a directory" elsewhere, picked because real Winsock's WSAEWOULDBLOCK is likewise "the same thing other APIs use for a different wait")
// marks "would block" on a socket made non-blocking - see kStatusWouldBlock in the .cpp.
namespace process::windowsabi {

// NtCreateSocket(ULONG Family, ULONG Type, ULONG Protocol, PHANDLE Result): AF_INET only; Type SOCK_STREAM (TCP) or SOCK_DGRAM (UDP).
uint32_t nt_create_socket(Process& p, const SyscallRequest& r);
// NtBindSocket(HANDLE, ULONG Address, ULONG Port): Address is ignored beyond is-it-INADDR_ANY (there is one address, this interface's own); Port and the rest of these
// services' "port" arguments are plain 16-bit values in the low half of the argument, in network byte order (as a sockaddr_in carries them: ws2_32 does not convert).
uint32_t nt_bind_socket(Process& p, const SyscallRequest& r);
// NtConnectSocket(HANDLE, ULONG Address, ULONG Port): TCP starts the handshake and returns WouldBlock until it finishes (the caller learns that through select() or
// NtSocketControl's GetPeerName, like a real non-blocking connect; a blocking socket's ws2_32 wrapper loops here itself). UDP just remembers the peer.
uint32_t nt_connect_socket(Process& p, const SyscallRequest& r);
// NtListenSocket(HANDLE, ULONG Backlog).
uint32_t nt_listen_socket(Process& p, const SyscallRequest& r);
// NtAcceptSocket(HANDLE Listener, PHANDLE Result, PULONG PeerAddress, PULONG PeerPort): WouldBlock (not failure) while nothing is waiting; ws2_32 loops for a
// blocking socket, or hands WouldBlock to the program as WSAEWOULDBLOCK for a non-blocking one.
uint32_t nt_accept_socket(Process& p, const SyscallRequest& r);
// NtSendSocket(HANDLE, PVOID Buffer, ULONG Length, PULONG Sent, ULONG Address, ULONG Port, BOOLEAN HasAddress): HasAddress true is sendto (UDP only); false is send
// (to the connected peer, which a UDP socket must have from NtConnectSocket).
uint32_t nt_send_socket(Process& p, const SyscallRequest& r);
// NtReceiveSocket(HANDLE, PVOID Buffer, ULONG Capacity, PULONG Got, PULONG FromAddress, PULONG FromPort, PBOOLEAN PeerClosed): FromAddress/FromPort may be 0 (recv,
// not recvfrom); for TCP they are filled in from the one peer regardless. *PeerClosed true (status Success, *Got 0) is an orderly close, like a real recv() returning 0.
uint32_t nt_receive_socket(Process& p, const SyscallRequest& r);
// NtSocketControl(HANDLE, ULONG Operation, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength). Operations: 0 Shutdown (In: ULONG How - 0 receive, 1 send, 2 both),
// 1 SetNonBlocking (In: BOOLEAN), 2 GetSockName (Out: ULONG Address, ULONG Port), 3 GetPeerName (Out: the same), 4 BytesReadable (Out: ULONG, for FIONREAD).
uint32_t nt_socket_control(Process& p, const SyscallRequest& r);
// NtSelectSockets(PULONG ReadCount, PULONG ReadHandles, PULONG WriteCount, PULONG WriteHandles, PULONG ExceptCount, PULONG ExceptHandles, PLARGE_INTEGER Timeout,
// PULONG ReadyCount): each *Count is read/write, like a real fd_set's own fd_count field - in, how many handles its *Handles array holds; out, how many of them are
// ready (the array itself is compacted in place to just those, the same way a real fd_set ends up after select()). *ReadyCount is the total across all three.
// A null Timeout waits forever; one of 0 does not wait at all.
uint32_t nt_select_sockets(Process& p, const SyscallRequest& r);
// What closing, duplicating or inheriting a socket handle does to its reference count.
void socket_token_add(uint64_t token);
void socket_token_release(uint64_t token);

}  // namespace process::windowsabi
