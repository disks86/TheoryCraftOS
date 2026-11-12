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

// The Linux socket system calls, for AF_UNIX sockets (UnixSockets.hpp: the same sockets Windows programs use, so the two can talk). Other families answer
// EAFNOSUPPORT. Each returns the value for the result register: the result, or -errno. Signals are not delivered here, so a send to a closed connection is
// just EPIPE (as with MSG_NOSIGNAL) and a blocked call ends with EINTR only when the program is told to stop.
namespace process::linuxabi {

uint64_t sys_socket(Process& p, uint64_t domain, uint64_t type, uint64_t protocol);
uint64_t sys_socketpair(Process& p, uint64_t domain, uint64_t type, uint64_t protocol, uint64_t pair);
uint64_t sys_pipe2(Process& p, uint64_t fds, uint64_t flags);
uint64_t sys_eventfd2(Process& p, uint64_t initial, uint64_t flags);      // (eventfd(initial) is eventfd2(initial, 0))      // (pipe(fds) is pipe2(fds, 0))
uint64_t sys_bind(Process& p, uint64_t fd, uint64_t address, uint64_t length);
uint64_t sys_connect(Process& p, uint64_t fd, uint64_t address, uint64_t length);
uint64_t sys_listen(Process& p, uint64_t fd, uint64_t backlog);
uint64_t sys_accept4(Process& p, uint64_t fd, uint64_t address, uint64_t length_at, uint64_t flags);
uint64_t sys_sendto(Process& p, uint64_t fd, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length);
uint64_t sys_recvfrom(Process& p, uint64_t fd, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at);
uint64_t sys_sendmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags);
uint64_t sys_recvmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags);
uint64_t sys_sendmmsg(Process& p, uint64_t fd, uint64_t vector, uint64_t count, uint64_t flags);
uint64_t sys_recvmmsg(Process& p, uint64_t fd, uint64_t vector, uint64_t count, uint64_t flags, uint64_t timeout);
uint64_t sys_shutdown(Process& p, uint64_t fd, uint64_t how);
uint64_t sys_getsockname(Process& p, uint64_t fd, uint64_t address, uint64_t length_at);
uint64_t sys_getpeername(Process& p, uint64_t fd, uint64_t address, uint64_t length_at);
uint64_t sys_setsockopt(Process& p, uint64_t fd, uint64_t level, uint64_t name, uint64_t value, uint64_t length);
uint64_t sys_getsockopt(Process& p, uint64_t fd, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at);
// poll(fds, n, timeout ms; negative waits for ever) and ppoll(fds, n, timespec* or 0, sigmask): sockets report their state, other descriptors are always ready
// for what they were asked (files and devices do not block here).
uint64_t sys_poll(Process& p, uint64_t fds, uint64_t count, uint64_t timeout_ns);
uint64_t sys_ppoll(Process& p, uint64_t fds, uint64_t count, uint64_t timespec);
// select(n, read, write, except, timeval*) and pselect6(n, read, write, except, timespec*, sigmask): the same over fd_sets.
uint64_t sys_select(Process& p, uint64_t count, uint64_t read_set, uint64_t write_set, uint64_t except_set, uint64_t time, bool nanoseconds);

// For the file calls: read()/write()/readv()/writev() on a socket descriptor are recv()/send() without flags or addresses.
uint64_t socket_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t socket_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t socket_vector_io(Process& p, uint64_t fd, uint64_t iov, uint64_t count, bool write);
// ioctl FIONREAD / FIONBIO and fcntl O_NONBLOCK of a socket descriptor.
uint64_t socket_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument);
bool socket_non_blocking(Process& p, uint64_t fd);
void set_socket_non_blocking(Process& p, uint64_t fd, bool on);
// Is the file `host_path` (of the OS's file system) bound to a socket right now? stat() then reports it as one (S_IFSOCK).
bool is_socket_file(StringView host_path);

}  // namespace process::linuxabi
