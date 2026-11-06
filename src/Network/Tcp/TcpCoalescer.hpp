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

// Generic receive offload for TCP (Linux's GRO, in software): segments of one connection that arrive back to back in one poll are put
// together into one larger segment before TCP sees them, so TCP takes (and acknowledges) one segment instead of many. The rules are Linux's
// tcp_gro_receive: same addresses and ports, IPv4 without options or fragments (or IPv6 with TCP straight after its header, the same
// traffic class, flow label and hop limit), the next sequence number, the same acknowledgement, the same
// TCP options, only ACK (PSH may end a run), no larger than the first one (a shorter one ends the run), and checksums that are right (the
// interface checked them, or this does). The window of the last piece is kept.
#pragma once
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Network/NetworkTypes.hpp"

namespace network {

class TcpCoalescer {
 public:
  // `storage` holds the run being put together (an IP packet): it limits how long a run gets.
  explicit TcpCoalescer(ByteView storage) : storage_(storage) {}
  TcpCoalescer() = default;

  // Offers a received Ethernet frame addressed to this machine. True if it was taken (it starts a run or joins the one held); false if it
  // cannot be: the caller hands out the run held (held(), then clear()) and handles the frame itself - or offers it again, as the start of
  // a new run.
  bool offer(ConstByteView frame, const hardware::ReceivedFrame& checks);
  bool holding() const { return length_ != 0; }
  // The run: one IP packet with the (total or payload) length of all of it (its header checksum and TCP checksum are not recomputed: it is handed on as
  // checked).
  ConstByteView held() const { return ConstByteView(storage_).first(length_); }
  uint32_t pieces() const { return pieces_; }
  void clear() {
    length_ = 0;
    pieces_ = 0;
  }
  uint64_t merged() const { return merged_; }  // segments that joined a run (not counting the first of each)

 private:
  struct Segment {
    ConstByteView ip;       // the IP packet (without Ethernet padding)
    bool ipv6;
    uint32_t tcp_offset;    // from the start of the IP packet
    uint32_t tcp_header;
    uint32_t sequence, acknowledgement;
    uint8_t flags;
    ConstByteView payload;
  };
  static bool parse(ConstByteView frame, const hardware::ReceivedFrame& checks, Segment* out);
  bool joins(const Segment& s) const;

  ByteView storage_;
  uint32_t length_ = 0;      // bytes of storage_ in use
  uint32_t pieces_ = 0;
  uint32_t next_sequence_ = 0;
  uint32_t piece_size_ = 0;  // the payload of the first piece: later ones may not be larger
  uint32_t tcp_header_ = 0;
  uint32_t ip_header_ = 0;   // 20 (IPv4) or 40 (IPv6): where the run's TCP header is
  bool ipv6_ = false;
  bool closed_ = false;      // the run ended (PSH, or a short piece): the next segment starts another
  uint64_t merged_ = 0;
};

}  // namespace network
