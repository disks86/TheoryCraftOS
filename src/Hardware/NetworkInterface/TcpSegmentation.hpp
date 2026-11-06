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

// What a driver needs to hand a TCP segment to a controller that cuts it into frames itself (TCP segmentation offload): where the headers
// are, checked against the frame (IPv4, or IPv6 with TCP straight after the fixed header), and the header changes the controllers expect
// (as igb_tso / e1000_tso and, for IPv6, tcp_v6_gso_csum_prep make them): the IPv4 total length and header checksum zero, or the IPv6
// payload length zero (the controller writes each frame's), and the pseudo-header sum in the TCP checksum field without the length (each
// frame's length is added by the controller).
#pragma once
#include <stdint.h>
#include "Hardware/NetworkInterface/NetworkTypes.hpp"

namespace hardware {

struct TsoLayout {
  uint16_t ip_start = 0;       // 14: an untagged Ethernet frame
  uint16_t tcp_start = 0;      // where the TCP header begins
  uint16_t header_length = 0;  // Ethernet, IP and TCP headers together
  uint32_t payload = 0;        // TCP payload bytes (what is cut into segments)
  bool ipv6 = false;
};

// Which IP version a frame carries (by its EtherType): 4, 6, or 0 for anything else.
inline uint32_t frame_ip_version(ConstByteView frame) {
  if (frame.size() < kEthernetHeaderSize) return 0;
  if (frame[12] == 0x08 && frame[13] == 0x00) return 4;
  if (frame[12] == 0x86 && frame[13] == 0xDD) return 6;
  return 0;
}

// Reads the layout of an IPv4 TCP frame whose checksum request (as transmit_segmented() gets it) points at its TCP checksum field. False
// for anything else: not IPv4, not TCP, a fragment, lengths that do not agree with the frame, or no payload.
inline bool tso_layout(ConstByteView frame, TransmitChecksum checksum, TsoLayout* out) {
  if (frame_ip_version(frame) == 6) {
    ByteReader in(frame.dropped(kEthernetHeaderSize));
    const uint32_t first = in.be32();
    const uint16_t payload_length = in.be16();
    const uint8_t next = in.u8();
    const uint32_t tcp_start = kEthernetHeaderSize + 40;
    if (!in.ok() || (first >> 28) != 6 || next != 6 || payload_length != frame.size() - tcp_start || tcp_start + 20 > frame.size()) return false;
    const uint32_t tcp_header = (frame[tcp_start + 12] >> 4) * 4u;
    if (tcp_header < 20 || tcp_start + tcp_header >= frame.size() || checksum.start != tcp_start || !checksum.tcp()) return false;
    *out = TsoLayout{static_cast<uint16_t>(kEthernetHeaderSize), static_cast<uint16_t>(tcp_start), static_cast<uint16_t>(tcp_start + tcp_header),
                     static_cast<uint32_t>(frame.size() - tcp_start - tcp_header), true};
    return true;
  }
  ByteReader in(frame);
  in.skip(12);
  const uint16_t ether_type = in.be16();
  const uint8_t version_and_length = in.u8();
  in.skip(1);
  const uint16_t total = in.be16();
  in.skip(2);
  const uint16_t fragment = in.be16();
  in.skip(1);
  const uint8_t protocol = in.u8();
  if (!in.ok() || ether_type != 0x0800 || (version_and_length >> 4) != 4 || protocol != 6 || (fragment & 0x3FFF)) return false;
  const uint32_t ip_header = (version_and_length & 0x0F) * 4u;
  const uint32_t tcp_start = kEthernetHeaderSize + ip_header;
  if (ip_header < 20 || total != frame.size() - kEthernetHeaderSize || tcp_start + 20 > frame.size()) return false;
  const uint32_t tcp_header = (frame[tcp_start + 12] >> 4) * 4u;
  if (tcp_header < 20 || tcp_start + tcp_header >= frame.size()) return false;
  if (checksum.start != tcp_start || !checksum.tcp()) return false;
  *out = TsoLayout{static_cast<uint16_t>(kEthernetHeaderSize), static_cast<uint16_t>(tcp_start), static_cast<uint16_t>(tcp_start + tcp_header),
                   static_cast<uint32_t>(frame.size() - tcp_start - tcp_header)};
  return true;
}

// The header changes above, in the driver's copy of the headers (the first layout.header_length bytes of the frame; the payload may be
// elsewhere, in the next transmit buffers).
inline bool prepare_tso_headers(ByteView frame, const TsoLayout& layout) {
  if (frame.size() < layout.header_length || layout.header_length < layout.tcp_start + 20u) return false;
  if (layout.ipv6) {
    frame[layout.ip_start + 4] = 0;  // payload length
    frame[layout.ip_start + 5] = 0;
  } else {
    frame[layout.ip_start + 2] = 0;  // total length
    frame[layout.ip_start + 3] = 0;
    frame[layout.ip_start + 10] = 0;  // header checksum
    frame[layout.ip_start + 11] = 0;
  }
  // The seed covers the TCP length of the whole segment: subtract it (ones'-complement arithmetic: add its complement).
  const uint32_t tcp_length = layout.header_length - layout.tcp_start + layout.payload;
  const uint32_t field = layout.tcp_start + 16u;
  uint32_t sum = (uint32_t{frame[field]} << 8) | frame[field + 1];
  sum += static_cast<uint16_t>(~tcp_length);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  frame[field] = static_cast<uint8_t>(sum >> 8);
  frame[field + 1] = static_cast<uint8_t>(sum);
  return true;
}

}  // namespace hardware
