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

// Local procedure call: the classic LPC ports (NtCreatePort, NtConnectPort, NtListenPort, NtAcceptConnectPort, NtRequestWaitReplyPort ...) and the ALPC ones (NtAlpc*) that most
// programs use now, both on one kind of object. A server makes a connection port with a name; a client connects to the name; the server accepts (which makes a pair of
// communication ports, one for each side) and then the two send each other messages: a request waits for its reply, a datagram does not. A message is a PORT_MESSAGE (a 0x28
// byte header and its data) of at most the maximum length of the port. Not done: shared sections and handles passed in messages (ALPC attributes), reading and writing the
// memory of the client (NtReadRequestData), impersonating the client. Each call returns an NTSTATUS.
namespace process::windowsabi {

uint32_t nt_create_port(Process& p, const SyscallRequest& r);               // (PHANDLE, POBJECT_ATTRIBUTES, MaxConnectionInfoLength, MaxMessageLength, MaxPoolUsage)
uint32_t nt_connect_port(Process& p, const SyscallRequest& r, bool secure); // (PHANDLE, PUNICODE_STRING, QoS, ClientView, [Sid,] ServerView, MaxMessageLength, ConnectionInfo, ConnectionInfoLength)
uint32_t nt_listen_port(Process& p, const SyscallRequest& r);               // (PortHandle, PPORT_MESSAGE ConnectionRequest)
uint32_t nt_accept_connect_port(Process& p, const SyscallRequest& r);       // (PHANDLE, PortContext, PPORT_MESSAGE ConnectionRequest, BOOLEAN Accept, ServerView, ClientView)
uint32_t nt_complete_connect_port(Process& p, const SyscallRequest& r);     // (PortHandle)
uint32_t nt_request_port(Process& p, const SyscallRequest& r);              // (PortHandle, PPORT_MESSAGE): a datagram
uint32_t nt_request_wait_reply_port(Process& p, const SyscallRequest& r);   // (PortHandle, PPORT_MESSAGE Request, PPORT_MESSAGE Reply)
uint32_t nt_reply_port(Process& p, const SyscallRequest& r);                // (PortHandle, PPORT_MESSAGE Reply)
uint32_t nt_reply_wait_receive_port(Process& p, const SyscallRequest& r, bool with_timeout);   // (PortHandle, PVOID* PortContext, PPORT_MESSAGE Reply, PPORT_MESSAGE Receive[, Timeout])
uint32_t nt_reply_wait_reply_port(Process& p, const SyscallRequest& r);     // (PortHandle, PPORT_MESSAGE Reply)
// NtRegisterThreadTerminatePort(PortHandle): when the calling thread ends the other end of the port gets an LPC_CLIENT_DIED message.
uint32_t nt_register_thread_terminate_port(Process& p, const SyscallRequest& r);
void ports_thread_ended(uint64_t process, uint64_t thread);
uint32_t nt_query_information_port(Process& p, const SyscallRequest& r);    // (PortHandle, Class, Buffer, Length, PULONG ReturnLength)

uint32_t nt_alpc_create_port(Process& p, const SyscallRequest& r);          // (PHANDLE, POBJECT_ATTRIBUTES, PALPC_PORT_ATTRIBUTES)
uint32_t nt_alpc_connect_port(Process& p, const SyscallRequest& r);         // (PHANDLE, PUNICODE_STRING, POBJECT_ATTRIBUTES, PALPC_PORT_ATTRIBUTES, Flags, PSID, PPORT_MESSAGE, PULONG BufferLength, OutAttributes, InAttributes, PLARGE_INTEGER Timeout)
uint32_t nt_alpc_accept_connect_port(Process& p, const SyscallRequest& r);  // (PHANDLE, ConnectionPortHandle, Flags, POBJECT_ATTRIBUTES, PALPC_PORT_ATTRIBUTES, PortContext, PPORT_MESSAGE, PALPC_MESSAGE_ATTRIBUTES, BOOLEAN Accept)
uint32_t nt_alpc_send_wait_receive_port(Process& p, const SyscallRequest& r);  // (PortHandle, Flags, SendMessage, SendAttributes, ReceiveMessage, PULONG BufferLength, ReceiveAttributes, PLARGE_INTEGER Timeout)
uint32_t nt_alpc_disconnect_port(Process& p, const SyscallRequest& r);      // (PortHandle, Flags)
uint32_t nt_alpc_query_information(Process& p, const SyscallRequest& r);    // (PortHandle, Class, Buffer, Length, PULONG ReturnLength)
uint32_t nt_alpc_set_information(Process& p, const SyscallRequest& r);      // (PortHandle, Class, Buffer, Length)
uint32_t nt_alpc_validate_port(Process& p, const SyscallRequest& r);        // calls that only need a port handle: security contexts, cancelling a message

}  // namespace process::windowsabi
