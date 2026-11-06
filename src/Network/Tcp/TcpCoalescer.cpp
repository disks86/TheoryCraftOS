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

#include "Network/Tcp/TcpCoalescer.hpp"
#include "Network/Checksum.hpp"

namespace network {

namespace {
constexpr uint8_t kAck = 0x10, kPsh = 0x08;
constexpr uint32_t kTcpWindow = 14;  // offsets in the TCP header
}  // namespace

bool TcpCoalescer::parse(ConstByteView frame, const hardware::ReceivedFrame& checks, Segment* out) {
  ByteReader in(frame);
  in.skip(12);
  const uint16_t ether_type = in.be16();
  const ConstByteView rest = in.rest();
  ConstByteView packet;
  uint32_t ip_header = 0;
  IpAddress source, destination;
  if (ether_type == kEtherTypeIpv4) {
    ByteReader ip(rest);
    const uint8_t version_and_length = ip.u8();
    ip.skip(1);
    const uint16_t total = ip.be16();
    ip.skip(2);
    const uint16_t fragment = ip.be16();
    ip.skip(1);
    const uint8_t protocol = ip.u8();
    if (!ip.ok() || version_and_length != 0x45 || protocol != kProtocolTcp || (fragment & 0x3FFF) || total > rest.size() || total < kIpv4HeaderSize + 20)
      return false;
    packet = rest.first(total);
    if (!checks.ipv4_header_checksum_good && checksum_finish(checksum_add(0, packet.first(kIpv4HeaderSize))) != 0) return false;
    ByteReader addresses(packet.subview(12, 8));
    source = IpAddress(Ipv4Address::read(addresses));
    destination = IpAddress(Ipv4Address::read(addresses));
    ip_header = kIpv4HeaderSize;
  } else if (ether_type == kEtherTypeIpv6) {
    ByteReader ip(rest);
    const uint32_t first = ip.be32();
    const uint16_t payload_length = ip.be16();
    const uint8_t next = ip.u8();
    ip.skip(1);
    const Ipv6Address s6 = Ipv6Address::read(ip), d6 = Ipv6Address::read(ip);
    if (!ip.ok() || (first >> 28) != 6 || next != kProtocolTcp || payload_length < 20 || payload_length > rest.size() - kIpv6HeaderSize) return false;
    packet = rest.first(kIpv6HeaderSize + payload_length);
    source = IpAddress(s6);
    destination = IpAddress(d6);
    ip_header = kIpv6HeaderSize;
  } else {
    return false;
  }
  ByteReader tcp(packet.dropped(ip_header));
  tcp.skip(4);
  const uint32_t sequence = tcp.be32();
  const uint32_t acknowledgement = tcp.be32();
  const uint8_t offset = tcp.u8();
  const uint8_t flags = tcp.u8();
  const uint32_t header = (offset >> 4) * 4u;
  if (!tcp.ok() || header < 20 || ip_header + header >= packet.size()) return false;  // (no payload: nothing to put together)
  if ((flags & ~kPsh) != kAck) return false;                                           // only ACK, maybe PSH
  if (!checks.transport_checksum_good) {
    const ConstByteView segment = packet.dropped(ip_header);
    if (checksum_finish(checksum_add(pseudo_header_sum(source, destination, kProtocolTcp, static_cast<uint32_t>(segment.size())), segment)) != 0)
      return false;
  }
  *out = Segment{packet, ether_type == kEtherTypeIpv6, ip_header, header, sequence, acknowledgement, flags, packet.dropped(ip_header + header)};
  return true;
}

bool TcpCoalescer::joins(const Segment& s) const {
  if (closed_ || s.ipv6 != ipv6_ || s.tcp_header != tcp_header_ || s.sequence != next_sequence_ || s.payload.size() > piece_size_) return false;
  if (s.payload.size() > storage_.size() - length_) return false;
  const ConstByteView run = held();
  if (ipv6_) {
    // the same version, traffic class and flow label (bytes 0-3), next header and hop limit (6-7), addresses (8-39)
    for (uint32_t i = 0; i < 40; ++i)
      if ((i < 4 || i >= 6) && s.ip[i] != run[i]) return false;
  } else {
    // the same type of service, time to live, protocol, addresses (IP bytes 1, 8-9, 12-19)
    if (s.ip[1] != run[1] || s.ip[8] != run[8] || s.ip[9] != run[9]) return false;
    for (uint32_t i = 12; i < 20; ++i)
      if (s.ip[i] != run[i]) return false;
  }
  // ports, acknowledgement and options
  const ConstByteView a = s.ip.subview(s.tcp_offset, s.tcp_header), b = run.subview(ip_header_, tcp_header_);
  for (uint32_t i = 0; i < s.tcp_header; ++i) {
    if (i >= 4 && i < 8) continue;    // (the sequence number, checked above)
    if (i >= 13 && i < 18) continue;  // (flags, window, checksum)
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool TcpCoalescer::offer(ConstByteView frame, const hardware::ReceivedFrame& checks) {
  if (storage_.empty()) return false;
  Segment s;
  if (!parse(frame, checks, &s)) return false;
  if (!holding()) {
    if ((s.flags & kPsh) || s.ip.size() > storage_.size()) return false;  // (a run of one PSH segment is just the segment)
    storage_.first(s.ip.size()).copy_from(s.ip);
    length_ = static_cast<uint32_t>(s.ip.size());
    pieces_ = 1;
    next_sequence_ = s.sequence + static_cast<uint32_t>(s.payload.size());
    piece_size_ = static_cast<uint32_t>(s.payload.size());
    tcp_header_ = s.tcp_header;
    ip_header_ = s.tcp_offset;
    ipv6_ = s.ipv6;
    closed_ = false;
    return true;
  }
  if (!joins(s)) return false;
  storage_.subview(length_, s.payload.size()).copy_from(s.payload);
  length_ += static_cast<uint32_t>(s.payload.size());
  ++pieces_;
  ++merged_;
  next_sequence_ += static_cast<uint32_t>(s.payload.size());
  if (ipv6_) {  // the run's IPv6 payload length
    storage_[4] = static_cast<uint8_t>((length_ - kIpv6HeaderSize) >> 8);
    storage_[5] = static_cast<uint8_t>(length_ - kIpv6HeaderSize);
  } else {  // the run's IP total length
    storage_[2] = static_cast<uint8_t>(length_ >> 8);
    storage_[3] = static_cast<uint8_t>(length_);
  }
  const uint32_t tcp = ip_header_;
  storage_[tcp + 13] = static_cast<uint8_t>(storage_[tcp + 13] | (s.flags & kPsh));
  storage_[tcp + kTcpWindow] = s.ip[s.tcp_offset + kTcpWindow];  // the latest window
  storage_[tcp + kTcpWindow + 1] = s.ip[s.tcp_offset + kTcpWindow + 1];
  if ((s.flags & kPsh) || s.payload.size() < piece_size_) closed_ = true;
  return true;
}

}  // namespace network
