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

#include "Rtc/Rtp.hpp"

namespace rtc {

Demux demux(ConstByteView d) {
  if (d.empty()) return Demux::Unknown;
  const uint8_t b = d[0];
  if (b <= 3) return Demux::Stun;
  if (b >= 20 && b <= 63) return Demux::Dtls;
  if (b >= 128 && b <= 191) {
    if (d.size() >= 2 && d[1] >= 192 && d[1] <= 223) return Demux::Rtcp;   // (the payload type field of RTP never reaches 192 here: RFC 5761)
    return d.size() >= kRtpHeader ? Demux::Rtp : Demux::Unknown;
  }
  return Demux::Unknown;
}

bool rtp_parse(ConstByteView p, RtpHeader* out) {
  if (p.size() < kRtpHeader || (p[0] >> 6) != 2) return false;
  out->has_extension = (p[0] & 0x10) != 0;
  out->csrc_count = p[0] & 15;
  out->marker = (p[1] & 0x80) != 0;
  out->payload_type = p[1] & 0x7F;
  out->sequence = load_be16(p, 2);
  out->timestamp = load_be32(p, 4);
  out->ssrc = load_be32(p, 8);
  uint64_t at = kRtpHeader + uint64_t{out->csrc_count} * 4;
  if (at > p.size()) return false;
  out->extension_offset = out->extension_size = 0;
  out->extension_profile = 0;
  if (out->has_extension) {
    if (p.size() - at < 4) return false;
    out->extension_profile = load_be16(p, at);
    const uint64_t words = load_be16(p, at + 2);
    if (words * 4 > p.size() - at - 4) return false;
    out->extension_offset = at + 4;
    out->extension_size = words * 4;
    at += 4 + words * 4;
  }
  out->header_size = at;
  uint64_t end = p.size();
  if (p[0] & 0x20) {   // padding: the last byte counts it
    const uint8_t pad = p[p.size() - 1];
    if (pad == 0 || pad > end - at) return false;
    end -= pad;
  }
  out->payload_size = end - at;
  return true;
}

bool rtp_extension(ConstByteView p, const RtpHeader& h, uint8_t id, ConstByteView* value) {
  if (!h.has_extension) return false;
  const ConstByteView body = p.subview(h.extension_offset, h.extension_size);
  const bool one_byte = h.extension_profile == 0xBEDE, two_byte = (h.extension_profile & 0xFFF0) == 0x1000;
  if (!one_byte && !two_byte) return false;
  uint64_t at = 0;
  while (at < body.size()) {
    if (body[at] == 0) {   // padding
      ++at;
      continue;
    }
    uint32_t eid, len;
    if (one_byte) {
      eid = body[at] >> 4;
      len = (body[at] & 15) + 1u;
      if (eid == 15) return false;
      ++at;
    } else {
      if (body.size() - at < 2) return false;
      eid = body[at];
      len = body[at + 1];
      at += 2;
    }
    if (len > body.size() - at) return false;
    if (eid == id) {
      *value = body.subview(at, len);
      return true;
    }
    at += len;
  }
  return false;
}

bool rtp_add_extension_element(ByteBuffer* e, uint8_t id, ConstByteView value) {
  if (id < 1 || id > kRtpMaxExtensions || value.empty() || value.size() > 16) return false;
  return e->put8(static_cast<uint8_t>(id << 4 | (value.size() - 1))) && e->append(value);
}

bool rtp_write_header(ByteBuffer* out, const RtpHeader& h, ConstByteView ext) {
  const bool extension = !ext.empty();
  if (h.csrc_count != 0) return false;   // (a sender never mixes)
  if (!out->put8(static_cast<uint8_t>(0x80 | (extension ? 0x10 : 0))) || !out->put8(static_cast<uint8_t>((h.marker ? 0x80 : 0) | (h.payload_type & 0x7F))) || !put_be16(out, h.sequence) ||
      !put_be32(out, h.timestamp) || !put_be32(out, h.ssrc))
    return false;
  if (!extension) return true;
  const uint64_t words = (ext.size() + 3) / 4;
  return put_be16(out, 0xBEDE) && put_be16(out, static_cast<uint32_t>(words)) && out->append(ext) && out->put_zeros(words * 4 - ext.size());
}

bool rtcp_report_block(ConstByteView p, const RtcpPacket& h, uint32_t index, RtcpReportBlock* out) {
  if (h.type != kRtcpSr && h.type != kRtcpRr) return false;
  if (index >= h.count_or_format || h.body_size < (uint64_t{index} + 1) * 24) return false;
  const uint64_t at = h.body_offset + uint64_t{index} * 24;
  out->ssrc = load_be32(p, at);
  out->fraction_lost = p[at + 4];
  uint32_t lost = (uint32_t{p[at + 5]} << 16) | load_be16(p, at + 6);
  out->cumulative_lost = (lost & 0x800000) ? static_cast<int32_t>(lost) - 0x1000000 : static_cast<int32_t>(lost);
  out->highest_sequence = load_be32(p, at + 8);
  out->jitter = load_be32(p, at + 12);
  out->last_sr = load_be32(p, at + 16);
  out->delay_since_sr = load_be32(p, at + 20);
  return true;
}

bool rtcp_bye_ssrc(ConstByteView p, const RtcpPacket& h, uint32_t index, uint32_t* ssrc) {
  if (h.type != kRtcpBye || index >= h.count_or_format || h.body_size < (uint64_t{index} + 1) * 4) return false;
  *ssrc = load_be32(p, h.body_offset + uint64_t{index} * 4);
  return true;
}

namespace {

bool header(ByteBuffer* out, uint8_t count_or_format, uint8_t type, uint64_t words_after_first) {
  return out->put8(static_cast<uint8_t>(0x80 | count_or_format)) && out->put8(type) && put_be16(out, static_cast<uint32_t>(words_after_first));
}

bool blocks_out(ByteBuffer* out, const RtcpReportBlock* b, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t lost = static_cast<uint32_t>(b[i].cumulative_lost) & 0xFFFFFF;
    if (!put_be32(out, b[i].ssrc) || !out->put8(b[i].fraction_lost) || !out->put8(static_cast<uint8_t>(lost >> 16)) || !put_be16(out, lost) || !put_be32(out, b[i].highest_sequence) ||
        !put_be32(out, b[i].jitter) || !put_be32(out, b[i].last_sr) || !put_be32(out, b[i].delay_since_sr))
      return false;
  }
  return true;
}

}  // namespace

bool rtcp_write_sender_report(ByteBuffer* out, uint32_t ssrc, uint32_t hi, uint32_t lo, uint32_t ts, uint32_t packets, uint32_t octets, const RtcpReportBlock* b, uint32_t n) {
  if (n > 31) return false;
  return header(out, static_cast<uint8_t>(n), kRtcpSr, 6 + uint64_t{n} * 6) && put_be32(out, ssrc) && put_be32(out, hi) && put_be32(out, lo) && put_be32(out, ts) && put_be32(out, packets) &&
         put_be32(out, octets) && blocks_out(out, b, n);
}

bool rtcp_write_receiver_report(ByteBuffer* out, uint32_t ssrc, const RtcpReportBlock* b, uint32_t n) {
  if (n > 31) return false;
  return header(out, static_cast<uint8_t>(n), kRtcpRr, 1 + uint64_t{n} * 6) && put_be32(out, ssrc) && blocks_out(out, b, n);
}

bool rtcp_write_sdes(ByteBuffer* out, uint32_t ssrc, StringView cname) {
  if (cname.size() > 255) return false;
  const uint64_t item = 2 + cname.size() + 1;   // CNAME item and the terminating zero
  const uint64_t chunk = (4 + item + 3) & ~uint64_t{3};
  if (!header(out, 1, kRtcpSdes, chunk / 4) || !put_be32(out, ssrc) || !out->put8(1) || !out->put8(static_cast<uint8_t>(cname.size())) ||
      !out->append(ConstByteView(reinterpret_cast<const uint8_t*>(cname.data()), cname.size())) || !out->put8(0))
    return false;
  return out->put_zeros(chunk - 4 - item);
}

bool rtcp_write_bye(ByteBuffer* out, uint32_t ssrc) { return header(out, 1, kRtcpBye, 1) && put_be32(out, ssrc); }

namespace {

// Walks the lost numbers as (PID, BLP) items; with `out` it writes them. Returns the number of items.
uint32_t nack_items(const uint16_t* lost, uint32_t count, ByteBuffer* out) {
  uint32_t items = 0, i = 0;
  while (i < count) {
    const uint16_t pid = lost[i];
    uint32_t blp = 0, j = i + 1;
    for (; j < count; ++j) {
      const uint16_t d = static_cast<uint16_t>(lost[j] - pid);
      if (d < 1 || d > 16) break;
      blp |= 1u << (d - 1);
    }
    if (out && !(put_be16(out, pid) && put_be16(out, blp))) return 0;
    ++items;
    i = j;
  }
  return items;
}

}  // namespace

bool rtcp_write_nack(ByteBuffer* out, uint32_t sender, uint32_t media, const uint16_t* lost, uint32_t count) {
  const uint32_t items = nack_items(lost, count, nullptr);
  if (items == 0 || items > 200) return false;
  return header(out, 1, kRtcpRtpFb, 2 + uint64_t{items}) && put_be32(out, sender) && put_be32(out, media) && nack_items(lost, count, out) == items;
}

bool rtcp_write_pli(ByteBuffer* out, uint32_t sender, uint32_t media) { return header(out, 1, kRtcpPsFb, 2) && put_be32(out, sender) && put_be32(out, media); }

bool rtcp_write_fir(ByteBuffer* out, uint32_t sender, uint32_t media, uint8_t sequence) {
  return header(out, 4, kRtcpPsFb, 4) && put_be32(out, sender) && put_be32(out, 0) && put_be32(out, media) && out->put8(sequence) && out->put_zeros(3);
}

bool rtcp_write_remb(ByteBuffer* out, uint32_t sender, uint64_t bitrate, const uint32_t* ssrcs, uint32_t n) {
  if (n > 255) return false;
  uint32_t exp = 0;
  uint64_t mantissa = bitrate;
  while (mantissa > 0x3FFFF) {
    mantissa >>= 1;
    ++exp;
  }
  if (exp > 63) return false;
  if (!header(out, 15, kRtcpPsFb, 4 + uint64_t{n}) || !put_be32(out, sender) || !put_be32(out, 0) || !out->put8('R') || !out->put8('E') || !out->put8('M') || !out->put8('B') ||
      !out->put8(static_cast<uint8_t>(n)) || !out->put8(static_cast<uint8_t>(exp << 2 | (mantissa >> 16))) || !put_be16(out, static_cast<uint32_t>(mantissa)))
    return false;
  for (uint32_t i = 0; i < n; ++i)
    if (!put_be32(out, ssrcs[i])) return false;
  return true;
}

uint32_t rtcp_nack_list(ConstByteView p, const RtcpPacket& h, uint16_t* out, uint32_t capacity) {
  if (h.type != kRtcpRtpFb || h.count_or_format != 1 || h.body_size < 4) return 0;
  uint32_t n = 0;
  for (uint64_t at = h.body_offset + 4; at + 4 <= h.body_offset + h.body_size; at += 4) {   // (after the media SSRC)
    const uint16_t pid = load_be16(p, at), blp = load_be16(p, at + 2);
    if (n < capacity) out[n++] = pid;
    for (uint32_t bit = 0; bit < 16; ++bit)
      if ((blp >> bit) & 1 && n < capacity) out[n++] = static_cast<uint16_t>(pid + bit + 1);
  }
  return n;
}

bool rtcp_remb_bitrate(ConstByteView p, const RtcpPacket& h, uint64_t* bitrate) {
  if (h.type != kRtcpPsFb || h.count_or_format != 15 || h.body_size < 12 || p[h.body_offset + 4] != 'R' || p[h.body_offset + 5] != 'E' || p[h.body_offset + 6] != 'M' ||
      p[h.body_offset + 7] != 'B')
    return false;
  const uint64_t at = h.body_offset + 9;
  const uint32_t exp = p[at] >> 2;
  const uint64_t mantissa = (uint64_t{p[at] & 3} << 16) | load_be16(p, at + 1);
  *bitrate = exp > 46 ? ~uint64_t{0} : mantissa << exp;
  return true;
}

}  // namespace rtc
