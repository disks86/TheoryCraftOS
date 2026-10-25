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

// RTP (RFC 3550) and RTCP: packet headers, header extensions (one-byte form, RFC 8285) and the RTCP packets WebRTC uses - sender and receiver reports, SDES, BYE, generic
// NACK (RFC 4585), picture loss indication, full intra request and REMB. Parsing is for input that cannot be trusted: every length is checked.
#pragma once
#include "Rtc/RtcBytes.hpp"
#include "Utility/String.hpp"

namespace rtc {

enum class Demux : uint8_t { Stun, Dtls, Rtp, Rtcp, Unknown };
// RFC 7983: what a datagram on the one port is, from its first bytes.
Demux demux(ConstByteView datagram);

constexpr uint32_t kRtpHeader = 12, kRtpMaxExtensions = 14;

struct RtpHeader {
  bool marker = false, has_extension = false;
  uint8_t payload_type = 0, csrc_count = 0;
  uint16_t sequence = 0;
  uint32_t timestamp = 0, ssrc = 0;
  uint64_t header_size = 0;                  // up to the payload
  uint64_t extension_offset = 0, extension_size = 0;   // the body of the extension (after its 4 byte header), in the packet
  uint16_t extension_profile = 0;
  uint64_t payload_size = 0;                 // without the padding
};
bool rtp_parse(ConstByteView packet, RtpHeader* out);
// The value of header extension `id` (one-byte form, profile 0xBEDE; the two-byte form 0x1000 too). False if the packet has none.
bool rtp_extension(ConstByteView packet, const RtpHeader& header, uint8_t id, ConstByteView* value);
// Appends a header (no extension unless `extension` is given as already-encoded one-byte elements, which are padded to a word).
bool rtp_write_header(ByteBuffer* out, const RtpHeader& header, ConstByteView extension_elements = ConstByteView());
// One-byte-form element: id (1..14), the value (1..16 bytes).
bool rtp_add_extension_element(ByteBuffer* elements, uint8_t id, ConstByteView value);

// --- RTCP
constexpr uint8_t kRtcpSr = 200, kRtcpRr = 201, kRtcpSdes = 202, kRtcpBye = 203, kRtcpRtpFb = 205, kRtcpPsFb = 206;
struct RtcpReportBlock {
  uint32_t ssrc = 0;
  uint8_t fraction_lost = 0;
  int32_t cumulative_lost = 0;
  uint32_t highest_sequence = 0, jitter = 0, last_sr = 0, delay_since_sr = 0;
};
struct RtcpPacket {
  uint8_t type = 0, count_or_format = 0;
  uint32_t sender_ssrc = 0;
  uint64_t body_offset = 0, body_size = 0;   // after the length word and the sender SSRC (or from the first byte after the header for SDES/BYE)
  // SR
  uint32_t ntp_hi = 0, ntp_lo = 0, rtp_timestamp = 0, packet_count = 0, octet_count = 0;
};
// Walks a compound packet: calls `visit(const RtcpPacket&, ConstByteView whole_packet)`. False if the compound packet is malformed.
template <class F>
bool rtcp_walk(ConstByteView data, F&& visit);
bool rtcp_report_block(ConstByteView packet, const RtcpPacket& header, uint32_t index, RtcpReportBlock* out);
// SDES / BYE / feedback payload helpers.
bool rtcp_bye_ssrc(ConstByteView packet, const RtcpPacket& header, uint32_t index, uint32_t* ssrc);

bool rtcp_write_sender_report(ByteBuffer* out, uint32_t ssrc, uint32_t ntp_hi, uint32_t ntp_lo, uint32_t rtp_timestamp, uint32_t packets, uint32_t octets,
                              const RtcpReportBlock* blocks, uint32_t block_count);
bool rtcp_write_receiver_report(ByteBuffer* out, uint32_t ssrc, const RtcpReportBlock* blocks, uint32_t block_count);
bool rtcp_write_sdes(ByteBuffer* out, uint32_t ssrc, StringView cname);
bool rtcp_write_bye(ByteBuffer* out, uint32_t ssrc);
bool rtcp_write_nack(ByteBuffer* out, uint32_t sender, uint32_t media, const uint16_t* lost, uint32_t lost_count);
bool rtcp_write_pli(ByteBuffer* out, uint32_t sender, uint32_t media);
bool rtcp_write_fir(ByteBuffer* out, uint32_t sender, uint32_t media, uint8_t sequence);
bool rtcp_write_remb(ByteBuffer* out, uint32_t sender, uint64_t bitrate, const uint32_t* ssrcs, uint32_t ssrc_count);
// Lost sequence numbers named by a generic NACK (PID + BLP items), into `out` (at most `capacity`); returns how many.
uint32_t rtcp_nack_list(ConstByteView packet, const RtcpPacket& header, uint16_t* out, uint32_t capacity);
bool rtcp_remb_bitrate(ConstByteView packet, const RtcpPacket& header, uint64_t* bitrate);

// ---- implementation of the walker
template <class F>
bool rtcp_walk(ConstByteView data, F&& visit) {
  uint64_t at = 0;
  if (data.empty()) return false;
  while (at < data.size()) {
    if (data.size() - at < 4 || (data[at] >> 6) != 2) return false;
    const uint64_t length = (uint64_t{load_be16(data, at + 2)} + 1) * 4;
    if (length > data.size() - at) return false;
    RtcpPacket p;
    p.type = data[at + 1];
    p.count_or_format = data[at] & 31;
    const bool has_ssrc = p.type != kRtcpSdes && p.type != kRtcpBye;
    if (has_ssrc) {
      if (length < 8) return false;
      p.sender_ssrc = load_be32(data, at + 4);
      p.body_offset = 8;
    } else {
      p.body_offset = 4;
    }
    if (p.type == kRtcpSr) {
      if (length < 28) return false;
      p.ntp_hi = load_be32(data, at + 8);
      p.ntp_lo = load_be32(data, at + 12);
      p.rtp_timestamp = load_be32(data, at + 16);
      p.packet_count = load_be32(data, at + 20);
      p.octet_count = load_be32(data, at + 24);
      p.body_offset = 28;
    }
    p.body_size = length - p.body_offset;
    visit(p, data.subview(at, length));
    at += length;
  }
  return true;
}

}  // namespace rtc
