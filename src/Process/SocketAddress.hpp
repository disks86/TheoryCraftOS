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
#include "Network/NetworkTypes.hpp"
#include "Utility/ByteStream.hpp"

// A Winsock socket address as a program's memory holds it, read and written for the socket services:
//   SOCKADDR_IN  (16 bytes): USHORT family = AF_INET (2, little-endian), USHORT port (network order), 4 address bytes, 8 zero bytes;
//   SOCKADDR_IN6 (28 bytes): USHORT family = AF_INET6 (23), USHORT port, ULONG flow info, 16 address bytes, ULONG scope id.
// The bytes come from the program and are not trusted: every read is bounds-checked.
namespace process {

constexpr uint16_t kWinAfInet = 2, kWinAfInet6 = 23;
constexpr uint32_t kSockaddrInSize = 16, kSockaddrIn6Size = 28;

// False for an unknown family or a buffer too short for it. Flow info and scope id are read past (an interface is chosen by the address itself).
inline bool parse_socket_address(ConstByteView bytes, network::IpAddress* address, uint16_t* port) {
  ByteReader in(bytes);
  const uint16_t family = in.le16();
  const uint16_t net_port = in.be16();
  if (!in.ok()) return false;
  if (family == kWinAfInet) {
    const uint32_t v4 = in.be32();
    if (!in.ok() || bytes.size() < kSockaddrInSize) return false;
    *address = network::IpAddress(network::Ipv4Address::from_value(v4));
  } else if (family == kWinAfInet6) {
    network::Ipv6Address v6;
    in.skip(4);
    if (!in.read_into(ByteView(v6.bytes)) || bytes.size() < kSockaddrIn6Size) return false;
    *address = network::IpAddress(v6);
  } else {
    return false;
  }
  *port = net_port;
  return true;
}

// The address as a socket of the given family shows it (an IPv6 socket's IPv4 peers come here already mapped). Returns the bytes written, 0 if `out` is too small.
inline uint32_t write_socket_address(ByteView out, const network::IpAddress& address, uint16_t port) {
  ByteWriter w(out);
  if (address.v6) {
    w.le16(kWinAfInet6);
    w.be16(port);
    w.le32(0);  // flow info
    w.bytes(ConstByteView(address.ipv6.bytes));
    w.le32(0);  // scope id
  } else {
    w.le16(kWinAfInet);
    w.be16(port);
    w.be32(address.ipv4.value());
    w.zeros(8);
  }
  return w.ok() ? uint32_t(w.written().size()) : 0;
}

}  // namespace process
