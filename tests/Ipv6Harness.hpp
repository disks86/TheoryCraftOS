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

// IPv6 pieces shared by the host tests that need them: address text, raw IPv6 frames, and a router advertisement that gives a stack on the fake
// wire (NetworkHarness.hpp) a global address, a default router and a DNS server.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Network/Checksum.hpp"
#include "Network/NetworkTypes.hpp"
#include "Network/Dns/IDnsResolver.hpp"
#include "Network/INetworkStack.hpp"

using namespace network;

typedef std::vector<uint8_t> Bytes;

static Ipv6Address v6(const char* text) {
  Ipv6Address a{};
  const bool ok = parse_ipv6(StringView(text), &a);
  if (!ok) { printf("bad test address %s\n", text); abort(); }
  return a;
}
static std::string text(const Ipv6Address& a) {
  char buffer[48];
  format_ipv6(a, ArrayView<char>(buffer));
  return buffer;
}

// An Ethernet frame with an IPv6 packet: `payload` after the 40-byte header, ICMPv6 checksums filled in.
static Bytes ipv6_frame(const uint8_t (&dst_mac)[6], const uint8_t (&src_mac)[6], const Ipv6Address& src, const Ipv6Address& dst, uint8_t next,
                        uint8_t hop_limit, Bytes payload) {
  if (next == 58 && payload.size() >= 4) {
    payload[2] = payload[3] = 0;
    const uint16_t sum = checksum_finish(checksum_add(pseudo_header_sum(src, dst, 58, (uint32_t)payload.size()), ConstByteView(payload.data(), payload.size())));
    payload[2] = (uint8_t)(sum >> 8);
    payload[3] = (uint8_t)sum;
  }
  Bytes f(14 + 40);
  memcpy(&f[0], dst_mac, 6);
  memcpy(&f[6], src_mac, 6);
  f[12] = 0x86; f[13] = 0xDD;
  f[14] = 0x60;
  f[18] = (uint8_t)(payload.size() >> 8);
  f[19] = (uint8_t)payload.size();
  f[20] = next; f[21] = hop_limit;
  memcpy(&f[22], src.bytes, 16);
  memcpy(&f[38], dst.bytes, 16);
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

static const uint8_t kRouterMac[6] = {0x02, 0xAA, 0, 0, 0, 0x01};
static const uint8_t kAllNodesMac[6] = {0x33, 0x33, 0, 0, 0, 1};

// A router advertisement from fe80::1: lifetime, an autonomous on-link /64 `prefix`, an MTU, a DNS server.
static Bytes router_advertisement(const Ipv6Address& prefix, uint16_t router_lifetime, uint32_t valid = 86400, uint8_t hop_limit = 255,
                                  bool autonomous = true, uint8_t prefix_length = 64) {
  Bytes m = {134, 0, 0, 0, 64, 0, (uint8_t)(router_lifetime >> 8), (uint8_t)router_lifetime, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t slla[8] = {1, 1, kRouterMac[0], kRouterMac[1], kRouterMac[2], kRouterMac[3], kRouterMac[4], kRouterMac[5]};
  m.insert(m.end(), slla, slla + 8);
  Bytes pi = {3, 4, prefix_length, (uint8_t)(0x80 | (autonomous ? 0x40 : 0))};
  for (int i = 3; i >= 0; --i) pi.push_back((uint8_t)(valid >> (8 * i)));
  for (int i = 3; i >= 0; --i) pi.push_back((uint8_t)((valid / 2) >> (8 * i)));
  pi.insert(pi.end(), 4, 0);
  pi.insert(pi.end(), prefix.bytes, prefix.bytes + 16);
  m.insert(m.end(), pi.begin(), pi.end());
  const uint8_t mtu[8] = {5, 1, 0, 0, 0, 0, 0x05, 0xDC};
  m.insert(m.end(), mtu, mtu + 8);
  Bytes rdnss = {25, 3, 0, 0, 0, 0, 0x0E, 0x10};
  const Ipv6Address dns = v6("2001:db8:1::53");
  rdnss.insert(rdnss.end(), dns.bytes, dns.bytes + 16);
  m.insert(m.end(), rdnss.begin(), rdnss.end());
  return ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), kIpv6AllNodes, 58, hop_limit, m);
}


// A DNS server on a stack (port 53): A and AAAA answers for one name (either left unspecified has no record: an empty answer), NXDOMAIN for
// every other name. serve() answers what has arrived; call it while the client waits.
struct DnsServer {
  SharedPointer<IUdpSocket> socket;
  std::string name;
  Ipv4Address a = kAnyAddress;
  Ipv6Address aaaa = kIpv6Any;
  bool sinkhole = false;   // answer 0.0.0.0 and :: for the name, as an ad-blocking DNS server does for a blocked one
  uint32_t queries = 0, aaaa_queries = 0;
  DnsServer(INetworkStack& stack, const char* served) : socket(stack.udp_open(53)), name(served) {}
  std::vector<uint8_t> wire_name() const {
    std::vector<uint8_t> out;
    size_t start = 0;
    for (;;) {
      size_t dot = name.find('.', start);
      if (dot == std::string::npos) dot = name.size();
      out.push_back((uint8_t)(dot - start));
      out.insert(out.end(), name.begin() + (long)start, name.begin() + (long)dot);
      if (dot == name.size()) break;
      start = dot + 1;
    }
    out.push_back(0);
    return out;
  }
  void serve() {
    uint8_t q[512];
    IpAddress from;
    uint16_t port = 0;
    uint32_t n = 0;
    const std::vector<uint8_t> wanted = wire_name();
    while (socket->receive_from_ip(ByteView(q), &from, &port, &n)) {
      if (n < 17 || n > sizeof q) continue;
      ++queries;
      std::vector<uint8_t> r(q, q + n);
      const uint16_t type = (uint16_t)(q[n - 4] << 8 | q[n - 3]);
      if (type == 28) ++aaaa_queries;
      const bool known = n == 12 + wanted.size() + 4 && !memcmp(q + 12, wanted.data(), wanted.size());
      r[2] = 0x81;
      r[3] = known ? 0x80 : 0x83;
      const bool has = sinkhole || (type == 28 ? !aaaa.is_unspecified() : !a.is_unspecified());
      if (known && has) {
        r[7] = 1;
        const uint8_t head[] = {0xC0, 12, 0, (uint8_t)type, 0, 1, 0, 0, 0, 60, 0, (uint8_t)(type == 28 ? 16 : 4)};
        r.insert(r.end(), head, head + sizeof head);
        if (type == 28) {
          const Ipv6Address six = sinkhole ? kIpv6Any : aaaa;
          r.insert(r.end(), six.bytes, six.bytes + 16);
        } else {
          const uint32_t v = sinkhole ? 0 : a.value();
          const uint8_t b4[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
          r.insert(r.end(), b4, b4 + 4);
        }
      }
      socket->send_to_ip(from, port, ConstByteView(r.data(), r.size()));
    }
  }
};
