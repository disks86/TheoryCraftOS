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
#include "Network/Udp/UdpSocket.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// Owns the UDP sockets and routes arriving datagrams to them by destination port.
class UdpManager {
 public:
  UdpManager(IIpLayer& ip, allocators::IAllocator& allocator) : ip_(ip), allocator_(allocator), sockets_(allocator) {}

  // Opens a socket on `port` (0 = pick an unused one). Null if the port is in use
  // or memory ran out.
  SharedPointer<UdpSocket> open(uint16_t port);

  // A UDP datagram arrived in an IP packet: `segment` is the UDP header and data.
  // True if a socket took it.
  bool handle(Ipv4Address source, Ipv4Address destination, const uint8_t* segment, uint32_t length);

  // Forgets sockets that were closed or that nobody holds any more.
  void reap();

 private:
  bool port_in_use(uint16_t port) const;

  IIpLayer& ip_;
  allocators::IAllocator& allocator_;
  List<SharedPointer<UdpSocket>> sockets_;
  uint16_t next_ephemeral_ = 49152;
};

}  // namespace network
