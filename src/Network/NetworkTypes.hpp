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
#include "Utility/ByteStream.hpp"

// Types shared by the network stack. Everything on the wire is big-endian;
// packets are read and written through ByteReader / ByteWriter, which check
// every access against the packet's bounds.
namespace network {

inline uint32_t load32(const uint8_t (&p)[4]) {  // a fixed 4-byte array: always in bounds
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
inline void store32(uint8_t (&p)[4], uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

struct Ipv4Address {
  uint8_t octets[4];

  static constexpr Ipv4Address make(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { return {{a, b, c, d}}; }
  static Ipv4Address from_value(uint32_t v) {
    Ipv4Address a;
    store32(a.octets, v);
    return a;
  }
  // From / to a stream (all zero and the stream in error if there are too few bytes).
  static Ipv4Address read(ByteReader& in) {
    Ipv4Address a{};
    in.read_into(ByteView(a.octets));
    return a;
  }
  void write(ByteWriter& out) const { out.bytes(ConstByteView(octets)); }

  uint32_t value() const { return load32(octets); }  // a.b.c.d as one integer, a in the top byte
  bool operator==(const Ipv4Address& o) const { return value() == o.value(); }
  bool operator!=(const Ipv4Address& o) const { return !(*this == o); }
  bool is_unspecified() const { return value() == 0; }
  bool is_limited_broadcast() const { return value() == 0xFFFFFFFF; }
  bool is_multicast() const { return (octets[0] & 0xF0) == 0xE0; }
  bool is_link_local() const { return octets[0] == 169 && octets[1] == 254; }  // 169.254.0.0/16 (RFC 3927)
  bool in_subnet(const Ipv4Address& network_address, const Ipv4Address& mask) const {
    return (value() & mask.value()) == (network_address.value() & mask.value());
  }
  // The "all hosts on this subnet" address.
  static Ipv4Address subnet_broadcast(const Ipv4Address& address, const Ipv4Address& mask) {
    return from_value(address.value() | ~mask.value());
  }
};

constexpr Ipv4Address kAnyAddress = Ipv4Address::make(0, 0, 0, 0);
constexpr Ipv4Address kBroadcastAddress = Ipv4Address::make(255, 255, 255, 255);

// What a host needs to know to talk IP: its own address, the subnet it is on,
// the router to use for everything else, and a DNS server.
struct IpConfiguration {
  Ipv4Address address = kAnyAddress;
  Ipv4Address netmask = kAnyAddress;
  Ipv4Address gateway = kAnyAddress;
  Ipv4Address dns = kAnyAddress;

  bool configured() const { return !address.is_unspecified(); }
};

constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kEtherTypeArp = 0x0806;

constexpr uint8_t kProtocolIcmp = 1;
constexpr uint8_t kProtocolIgmp = 2;
constexpr uint8_t kProtocolTcp = 6;
constexpr uint8_t kProtocolUdp = 17;

constexpr uint32_t kIpv4HeaderSize = 20;
constexpr uint32_t kMtu = 1500;
constexpr uint32_t kMaxIpPayload = kMtu - kIpv4HeaderSize;  // no fragmentation is supported

}  // namespace network
