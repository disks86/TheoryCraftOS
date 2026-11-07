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
#include "Network/RcuSocketTable.hpp"
#include "Network/Udp/UdpSocket.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// Owns the UDP sockets and routes arriving datagrams to them by destination port. Thread-safe: a datagram finds its socket in a hash table read
// with RCU (RcuSocketTable: no lock), and is handed to it under the socket's own lock; opening and forgetting sockets take table_mutex_.
class UdpManager {
 public:
  UdpManager(IIpLayer& ip, allocators::IAllocator& allocator) : ip_(ip), allocator_(allocator), table_(allocator), sockets_(allocator) {}

  // Opens a socket on `port` (0 = pick an unused one). Null if the port is in use
  // or memory ran out.
  SharedPointer<UdpSocket> open(uint16_t port, IpFamilies families = IpFamilies::Both);

  // A UDP datagram arrived in an IP packet: `segment` is the UDP header and data.
  // True if a socket took it.
  // `checksum_verified`: the network interface checked the checksum already (and found it right).
  bool handle(const IpAddress& source, const IpAddress& destination, ConstByteView segment, bool checksum_verified = false);

  // Forgets sockets that were closed or that nobody holds any more.
  void reap();

 private:
  bool port_in_use(uint16_t port, IpFamilies families) const;

  IIpLayer& ip_;
  allocators::IAllocator& allocator_;
  RcuSocketTable<UdpSocket, 32> table_;   // by local port (grows with what it holds)
  mutable kernel::Mutex table_mutex_;      // guards the table's changes and the list (never held while calling into a socket)
  List<SharedPointer<UdpSocket>> sockets_;   // every open socket (to forget the closed ones)
  uint16_t next_ephemeral_ = 49152;
};

}  // namespace network
