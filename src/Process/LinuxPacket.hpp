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

#include "Process/MemoryFile.hpp"

namespace process {
class Process;

namespace linuxabi {

// AF_PACKET sockets for Linux programs (LinuxPacket.cpp): the Ethernet frames of the network interfaces, received and sent as they are. What a DHCP
// client of a program's own, ARP tools and packet capture are built on.
uint64_t packet_socket(Process& p, uint64_t type, uint64_t protocol);
// Is `fd` one? Then *link is what the calls below take.
bool packet_descriptor(Process& p, uint64_t fd, uint64_t* link, bool* non_blocking = nullptr);
uint64_t packet_bind(Process& p, uint64_t link, uint64_t address, uint64_t length);
uint64_t packet_sendto(Process& p, uint64_t link, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length);
uint64_t packet_sendmsg(Process& p, uint64_t link, uint64_t message, uint64_t flags);
uint64_t packet_recvfrom(Process& p, uint64_t link, bool non_blocking, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at);
uint64_t packet_recvmsg(Process& p, uint64_t link, bool non_blocking, uint64_t message, uint64_t flags);
uint64_t packet_vector_io(Process& p, uint64_t link, bool non_blocking, uint64_t vectors, uint64_t count, bool write);
uint64_t packet_getsockname(Process& p, uint64_t link, uint64_t address, uint64_t length_at);
uint64_t packet_setsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length);
uint64_t packet_getsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at);
// FIONREAD: the size of the frame a read would get.
uint64_t packet_pending(Process& p, uint64_t link, uint64_t argument);
// For poll() (the system's lock held): is there a frame to read, or with a receive ring something in the ring the program has not given back? (Nothing
// tells a wait when a frame arrives: it looks again every 2 ms. With a ring this is also what moves frames into it.)
bool packet_ready_locked(uint64_t link, uint64_t realtime_ns, uint64_t monotonic_ns);
// For mmap(): the socket's rings, the receive ring first, each with a use for the caller and its size. How many there are (0: none to map).
uint32_t packet_rings(uint64_t link, BlockRef blocks[2], uint64_t sizes[2]);

}  // namespace linuxabi
}  // namespace process
