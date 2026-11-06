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
  bool is_loopback() const { return octets[0] == 127; }  // 127.0.0.0/8: this machine, never on a wire
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

// An IPv6 address (RFC 4291), 16 bytes in network order.
struct Ipv6Address {
  uint8_t bytes[16];

  static Ipv6Address read(ByteReader& in) {
    Ipv6Address a{};
    in.read_into(ByteView(a.bytes));
    return a;
  }
  void write(ByteWriter& out) const { out.bytes(ConstByteView(bytes)); }
  // From eight 16-bit groups, as written (fe80::1 is make(0xfe80, 0, 0, 0, 0, 0, 0, 1)).
  static constexpr Ipv6Address make(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e, uint16_t f, uint16_t g, uint16_t h) {
    return {{static_cast<uint8_t>(a >> 8), static_cast<uint8_t>(a), static_cast<uint8_t>(b >> 8), static_cast<uint8_t>(b),
             static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c), static_cast<uint8_t>(d >> 8), static_cast<uint8_t>(d),
             static_cast<uint8_t>(e >> 8), static_cast<uint8_t>(e), static_cast<uint8_t>(f >> 8), static_cast<uint8_t>(f),
             static_cast<uint8_t>(g >> 8), static_cast<uint8_t>(g), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h)}};
  }
  uint16_t group(uint32_t i) const { return static_cast<uint16_t>((bytes[2 * (i & 7)] << 8) | bytes[2 * (i & 7) + 1]); }

  bool operator==(const Ipv6Address& o) const {
    for (int i = 0; i < 16; ++i)
      if (bytes[i] != o.bytes[i]) return false;
    return true;
  }
  bool operator!=(const Ipv6Address& o) const { return !(*this == o); }
  bool is_unspecified() const {
    for (uint8_t b : bytes)
      if (b) return false;
    return true;
  }
  bool is_loopback() const {  // ::1
    for (int i = 0; i < 15; ++i)
      if (bytes[i]) return false;
    return bytes[15] == 1;
  }
  bool is_multicast() const { return bytes[0] == 0xFF; }
  bool is_link_local() const { return bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80; }  // fe80::/10
  // The first `length` bits are those of `prefix`.
  bool in_prefix(const Ipv6Address& prefix, uint32_t length) const {
    for (uint32_t i = 0; i < 16 && length > 0; ++i, length = length > 8 ? length - 8 : 0) {
      const uint8_t mask = static_cast<uint8_t>(length >= 8 ? 0xFF : 0xFF << (8 - length));
      if ((bytes[i] & mask) != (prefix.bytes[i] & mask)) return false;
    }
    return true;
  }
  // A /64 prefix with the interface identifier made from a MAC address (modified EUI-64, RFC 4291 appendix A).
  static Ipv6Address from_prefix_and_mac(const Ipv6Address& prefix, const uint8_t (&mac)[6]) {
    Ipv6Address a = prefix;
    a.bytes[8] = static_cast<uint8_t>(mac[0] ^ 0x02);
    a.bytes[9] = mac[1];
    a.bytes[10] = mac[2];
    a.bytes[11] = 0xFF;
    a.bytes[12] = 0xFE;
    a.bytes[13] = mac[3];
    a.bytes[14] = mac[4];
    a.bytes[15] = mac[5];
    return a;
  }
  // The solicited-node multicast group of this address (ff02::1:ffXX:XXXX, RFC 4291 2.7.1): where neighbor solicitations for it go.
  Ipv6Address solicited_node() const {
    Ipv6Address a = make(0xFF02, 0, 0, 0, 0, 1, 0xFF00, 0);
    a.bytes[13] = bytes[13];
    a.bytes[14] = bytes[14];
    a.bytes[15] = bytes[15];
    return a;
  }
};

constexpr Ipv6Address kIpv6Any = Ipv6Address::make(0, 0, 0, 0, 0, 0, 0, 0);
constexpr Ipv6Address kIpv6Loopback = Ipv6Address::make(0, 0, 0, 0, 0, 0, 0, 1);
constexpr Ipv6Address kIpv6AllNodes = Ipv6Address::make(0xFF02, 0, 0, 0, 0, 0, 0, 1);
constexpr Ipv6Address kIpv6AllRouters = Ipv6Address::make(0xFF02, 0, 0, 0, 0, 0, 0, 2);
constexpr Ipv6Address kIpv6AllMldv2Routers = Ipv6Address::make(0xFF02, 0, 0, 0, 0, 0, 0, 0x16);
constexpr Ipv6Address kIpv6LinkLocalPrefix = Ipv6Address::make(0xFE80, 0, 0, 0, 0, 0, 0, 0);

// An address of either family, for the transports that run over both (TCP, UDP). Converts from either kind of address.
struct IpAddress {
  bool v6 = false;
  Ipv4Address ipv4 = kAnyAddress;
  Ipv6Address ipv6 = kIpv6Any;

  constexpr IpAddress() = default;
  constexpr IpAddress(Ipv4Address a) : ipv4(a) {}
  constexpr IpAddress(const Ipv6Address& a) : v6(true), ipv6(a) {}

  bool operator==(const IpAddress& o) const { return v6 == o.v6 && (v6 ? ipv6 == o.ipv6 : ipv4 == o.ipv4); }
  bool operator!=(const IpAddress& o) const { return !(*this == o); }
  bool is_unspecified() const { return v6 ? ipv6.is_unspecified() : ipv4.is_unspecified(); }
  bool is_loopback() const { return v6 ? ipv6.is_loopback() : ipv4.is_loopback(); }
  bool is_multicast() const { return v6 ? ipv6.is_multicast() : ipv4.is_multicast(); }
};

// What SLAAC and the router advertisements gave an interface (RFC 4861, 4862).
struct Ipv6Configuration {
  Ipv6Address link_local = kIpv6Any;  // fe80::/64 + the MAC's interface identifier, once duplicate address detection passed
  bool link_local_ready = false;
  Ipv6Address global = kIpv6Any;      // from the advertised /64 prefix, once it passed duplicate address detection
  bool global_ready = false;
  Ipv6Address prefix = kIpv6Any;      // the on-link prefix (a /64)
  uint8_t prefix_length = 0;
  Ipv6Address router = kIpv6Any;      // the default router's link-local address (unspecified: none)
  Ipv6Address dns = kIpv6Any;         // from the RDNSS option (RFC 8106), if the router sent one
  uint32_t mtu = 1500;

  bool configured() const { return link_local_ready; }
};

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
constexpr uint16_t kEtherTypeIpv6 = 0x86DD;

constexpr uint8_t kProtocolIcmp = 1;
constexpr uint8_t kProtocolIgmp = 2;
constexpr uint8_t kProtocolTcp = 6;
constexpr uint8_t kProtocolUdp = 17;
constexpr uint8_t kProtocolIcmpv6 = 58;

constexpr uint32_t kIpv4HeaderSize = 20;
constexpr uint32_t kMtu = 1500;
constexpr uint32_t kMaxIpPayload = kMtu - kIpv4HeaderSize;  // no fragmentation is supported
constexpr uint32_t kIpv6HeaderSize = 40;
constexpr uint32_t kMaxIpv6Payload = kMtu - kIpv6HeaderSize;  // (IPv6 routers do not fragment: neither does this)

}  // namespace network
