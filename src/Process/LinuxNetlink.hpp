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

#include "Process/LinuxNetlinkMessages.hpp"

namespace process {
class Process;

namespace linuxabi {

// AF_NETLINK sockets for Linux programs (LinuxNetlink.cpp): how a program asks the kernel about the network (getifaddrs, if_nametoindex, `ip addr`) and
// hears of changes to it.
uint64_t netlink_socket(Process& p, uint64_t type, uint64_t protocol);
// Is `fd` one? Then *link is what the calls below take.
bool netlink_descriptor(Process& p, uint64_t fd, uint64_t* link, bool* non_blocking = nullptr);
uint64_t netlink_bind(Process& p, uint64_t link, uint64_t address, uint64_t length);
uint64_t netlink_connect(Process& p, uint64_t link, uint64_t address, uint64_t length);
uint64_t netlink_sendto(Process& p, uint64_t link, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length);
uint64_t netlink_sendmsg(Process& p, uint64_t link, uint64_t message, uint64_t flags);
uint64_t netlink_recvfrom(Process& p, uint64_t link, bool non_blocking, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at);
uint64_t netlink_recvmsg(Process& p, uint64_t link, bool non_blocking, uint64_t message, uint64_t flags);
uint64_t netlink_vector_io(Process& p, uint64_t link, bool non_blocking, uint64_t vectors, uint64_t count, bool write);
uint64_t netlink_getsockname(Process& p, uint64_t link, uint64_t address, uint64_t length_at, bool peer);
uint64_t netlink_setsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length);
uint64_t netlink_getsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at);
// FIONREAD: the size of the datagram a read would get.
uint64_t netlink_pending(Process& p, uint64_t link, uint64_t argument);
// The interface ioctls of any socket (SIOCGIFNAME, SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFCONF, ...). False if `request` is not one of them;
// otherwise *result is the call's result.
bool interface_ioctl(Process& p, uint64_t request, uint64_t argument, uint64_t* result);
// For poll() (the system's lock held): is there a datagram to read? *watching: the socket listens for changes to the network, which nothing announces,
// so a wait has to look again after a while.
bool netlink_ready_locked(uint64_t link, bool* watching);
// The network as Linux programs are shown it: the loopback as interface 1 ("lo"), then each network interface ("eth0", ...).
void network_snapshot(netlink::Snapshot* out);

}  // namespace linuxabi
}  // namespace process
