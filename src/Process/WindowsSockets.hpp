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

// IPv4 and IPv6 sockets (ws2_32.dll's services), all this OS's own: real Windows has no NT services for sockets at all (Winsock talks to a device driver, afd.sys, through
// its own ioctl protocol). A socket handle is a descriptor of kind Socket, closed the usual way (NtClose); everything else about it goes through these. Each returns an
// NTSTATUS; 0xC0000103 (shared with "not a directory" elsewhere, picked because real Winsock's WSAEWOULDBLOCK is likewise "the same thing other APIs use for a different
// wait") marks "would block" on a socket made non-blocking - see kStatusWouldBlock in the .cpp.
//
// Addresses cross as the program's own socket address bytes (SOCKADDR_IN or SOCKADDR_IN6, see SocketAddress.hpp), passed straight through by ws2_32. An address
// buffer the kernel fills is given with a PULONG length that holds its capacity going in and the size written coming out; one that is too small is
// STATUS_BUFFER_TOO_SMALL, found before any connection or datagram is taken.
namespace process::windowsabi {

// NtCreateSocket(ULONG Family, ULONG Type, ULONG Protocol, PHANDLE Result): Family AF_INET (2) or AF_INET6 (23; IPv6-only until SetV6Only turns that off); Type
// SOCK_STREAM (TCP) or SOCK_DGRAM (UDP).
uint32_t nt_create_socket(Process& p, const SyscallRequest& r);
// NtBindSocket(HANDLE, PVOID Address, ULONG AddressLength): the address itself is not used beyond its family (each interface has its address and a socket uses
// whichever it ends up on); the port is.
uint32_t nt_bind_socket(Process& p, const SyscallRequest& r);
// NtConnectSocket(HANDLE, PVOID Address, ULONG AddressLength): TCP starts the handshake and returns WouldBlock until it finishes (the caller learns that through
// select() or NtSocketControl's GetPeerName, like a real non-blocking connect; a blocking socket's ws2_32 wrapper loops here itself). UDP just remembers the peer.
uint32_t nt_connect_socket(Process& p, const SyscallRequest& r);
// NtListenSocket(HANDLE, ULONG Backlog).
uint32_t nt_listen_socket(Process& p, const SyscallRequest& r);
// NtAcceptSocket(HANDLE Listener, PHANDLE Result, PVOID PeerAddress, PULONG PeerAddressLength): WouldBlock (not failure) while nothing is waiting; ws2_32 loops for a
// blocking socket, or hands WouldBlock to the program as WSAEWOULDBLOCK for a non-blocking one. PeerAddress may be null.
uint32_t nt_accept_socket(Process& p, const SyscallRequest& r);
// NtSendSocket(HANDLE, PVOID Buffer, ULONG Length, PULONG Sent, PVOID To, ULONG ToLength): To non-null is sendto (UDP only); null is send (to the connected peer,
// which a UDP socket must have from NtConnectSocket).
uint32_t nt_send_socket(Process& p, const SyscallRequest& r);
// NtReceiveSocket(HANDLE, PVOID Buffer, ULONG Capacity, PULONG Got, PVOID From, PULONG FromLength, PBOOLEAN PeerClosed): From may be null (recv, not recvfrom); for
// TCP it is filled in from the one peer. *PeerClosed true (status Success, *Got 0, From untouched) is an orderly close, like a real recv() returning 0.
uint32_t nt_receive_socket(Process& p, const SyscallRequest& r);
// NtSocketControl(HANDLE, ULONG Operation, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength). Operations:
//   0 Shutdown (In: ULONG How - 0 receive, 1 send, 2 both)        1 SetNonBlocking (In: BOOLEAN)
//   2 GetSockName (Out: a socket address)                          3 GetPeerName (Out: the same)
//   4 BytesReadable (Out: ULONG, for FIONREAD)
//   5 InterfaceConfiguration (In: ULONG interface index; Out: address, netmask, gateway, DNS server, 4 bytes each in network order; STATUS_NO_MORE_ENTRIES past
//     the last)
//   6 SetV6Only (In: BOOLEAN; IPV6_V6ONLY, only on an AF_INET6 socket that is not bound, connected or used yet)
//   7 GetSocketInfo (Out: ULONG family, ULONG type, BOOLEAN v6only, 3 bytes padding)
//   8 InterfaceConfiguration6 (In: ULONG index; Out: link-local, global, router and DNS server addresses, 16 bytes each, then UCHAR prefix length, UCHAR flags
//     (1 link-local ready, 2 global ready), 2 bytes padding, ULONG MTU)
//   9 ResolveName (In: ULONG family - AF_INET asks DNS for A records, AF_INET6 for AAAA - then the name's bytes; Out: the 4- or 16-byte address). Always waits
//     for the answer; STATUS_NOT_FOUND if there is none. Any socket handle will do.
//  10 HasIpv6Route (Out: BOOLEAN - some interface has a global IPv6 address and a default router)
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
