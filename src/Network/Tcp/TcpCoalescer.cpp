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
  if (in.be16() != kEtherTypeIpv4) return false;
  const ConstByteView rest = in.rest();
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
  const ConstByteView packet = rest.first(total);
  if (!checks.ipv4_header_checksum_good && checksum_finish(checksum_add(0, packet.first(kIpv4HeaderSize))) != 0) return false;
  ByteReader tcp(packet.dropped(kIpv4HeaderSize));
  tcp.skip(4);
  const uint32_t sequence = tcp.be32();
  const uint32_t acknowledgement = tcp.be32();
  const uint8_t offset = tcp.u8();
  const uint8_t flags = tcp.u8();
  const uint32_t header = (offset >> 4) * 4u;
  if (!tcp.ok() || header < 20 || kIpv4HeaderSize + header >= total) return false;  // (no payload: nothing to put together)
  if ((flags & ~kPsh) != kAck) return false;                                         // only ACK, maybe PSH
  if (!checks.transport_checksum_good) {
    Ipv4Address source, destination;
    ByteReader addresses(packet.subview(12, 8));
    source = Ipv4Address::read(addresses);
    destination = Ipv4Address::read(addresses);
    const ConstByteView segment = packet.dropped(kIpv4HeaderSize);
    if (checksum_finish(checksum_add(pseudo_header_sum(source, destination, kProtocolTcp, static_cast<uint16_t>(segment.size())), segment)) != 0)
      return false;
  }
  *out = Segment{packet, kIpv4HeaderSize, header, sequence, acknowledgement, flags, packet.dropped(kIpv4HeaderSize + header)};
  return true;
}

bool TcpCoalescer::joins(const Segment& s) const {
  if (closed_ || s.tcp_header != tcp_header_ || s.sequence != next_sequence_ || s.payload.size() > piece_size_) return false;
  if (s.payload.size() > storage_.size() - length_) return false;
  const ConstByteView run = held();
  // the same type of service, time to live, protocol, addresses (IP bytes 1, 8-9, 12-19), ports, acknowledgement and options
  if (s.ip[1] != run[1] || s.ip[8] != run[8] || s.ip[9] != run[9]) return false;
  for (uint32_t i = 12; i < 20; ++i)
    if (s.ip[i] != run[i]) return false;
  const ConstByteView a = s.ip.subview(s.tcp_offset, s.tcp_header), b = run.subview(kIpv4HeaderSize, tcp_header_);
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
    closed_ = false;
    return true;
  }
  if (!joins(s)) return false;
  storage_.subview(length_, s.payload.size()).copy_from(s.payload);
  length_ += static_cast<uint32_t>(s.payload.size());
  ++pieces_;
  ++merged_;
  next_sequence_ += static_cast<uint32_t>(s.payload.size());
  storage_[2] = static_cast<uint8_t>(length_ >> 8);  // the run's IP total length
  storage_[3] = static_cast<uint8_t>(length_);
  const uint32_t tcp = kIpv4HeaderSize;
  storage_[tcp + 13] = static_cast<uint8_t>(storage_[tcp + 13] | (s.flags & kPsh));
  storage_[tcp + kTcpWindow] = s.ip[s.tcp_offset + kTcpWindow];  // the latest window
  storage_[tcp + kTcpWindow + 1] = s.ip[s.tcp_offset + kTcpWindow + 1];
  if ((s.flags & kPsh) || s.payload.size() < piece_size_) closed_ = true;
  return true;
}

}  // namespace network
