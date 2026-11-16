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
#include "Process/LinuxPacketRing.hpp"

#include "Utility/ByteStream.hpp"

namespace process::linuxabi::ring {

namespace {

constexpr uint32_t kPage = 4096, kAlignment = 16, kLinkHeader = 14;
// status words: a place the kernel may fill (or the program has emptied) is 0
constexpr uint32_t kStatusUser = 1, kStatusCopy = 2, kStatusBlockTimeout = 0x20;
constexpr uint32_t kStatusSendRequest = 1, kStatusWrongFormat = 4;
// struct tpacket_block_desc (TPACKET_V3), at the start of each block: version (4), offset_to_priv (4), then struct tpacket_hdr_v1: block_status (4),
// num_pkts (4), offset_to_first_pkt (4), blk_len (4), seq_num (8), ts_first_pkt and ts_last_pkt (seconds and nanoseconds, 4 each)
constexpr uint32_t kBlockHeader = 48, kBlockStatusAt = 8;
constexpr uint64_t kDefaultRetireNs = 8000000;      // (what Linux works out for an ordinary interface when the program names no time)

uint32_t align(uint32_t v, uint32_t to) { return (v + to - 1) & ~(to - 1); }

// The status word of a frame or block, read and written in one piece: the program on the other side of the ring looks at it while this side changes it.
// (`at` is 4 bytes at a multiple of 4 in page-aligned memory, so the pointer is aligned for the atomic access; the bytes live as long as the view.)
uint32_t load_status(ConstByteView at) { return __atomic_load_n(reinterpret_cast<const uint32_t*>(at.data()), __ATOMIC_ACQUIRE); }
void store_status(ByteView at, uint32_t status) { __atomic_store_n(reinterpret_cast<uint32_t*>(at.data()), status, __ATOMIC_RELEASE); }

// Where a frame's status word is in its header: first in the first two layouts (as part of an unsigned long in the first), sixth in the third.
uint32_t status_at(Version version) { return version == Version::V3 ? 20 : 0; }

// Where a frame's parts are, counted from its header: the struct sockaddr_ll, the link header (if it has one) and what follows it.
struct Layout {
  uint32_t name = 0, link = 0, data = 0;
};
Layout layout_of(Version version, bool with_link, uint32_t reserve) {
  const uint32_t header = header_size(uint32_t(version));
  Layout l;
  l.name = align(header, kAlignment);
  // (as Linux places them: room for a link header of up to 16 bytes before the network header, which is aligned)
  l.data = with_link ? align(header + kNameSize + 16, kAlignment) + reserve : align(header + kNameSize, kAlignment) + 16 + reserve;
  l.link = with_link ? l.data - kLinkHeader : l.data;
  return l;
}

}  // namespace

uint32_t header_size(uint32_t version) {
  switch (version) {
    case 0: return 32;      // struct tpacket_hdr: tp_status (8), tp_len, tp_snaplen (4 each), tp_mac, tp_net (2 each), tp_sec, tp_usec (4 each), padded
    case 1: return 32;      // struct tpacket2_hdr: tp_status, tp_len, tp_snaplen (4 each), tp_mac, tp_net (2 each), tp_sec, tp_nsec (4 each), VLAN (4), padding (4)
    case 2: return 48;      // struct tpacket3_hdr: tp_next_offset, tp_sec, tp_nsec, tp_snaplen, tp_len, tp_status (4 each), tp_mac, tp_net (2 each), 12 more, padding (8)
    default: return 0;
  }
}

bool valid(Version version, const Request& r, uint32_t reserve) {
  const uint32_t least = header_size(uint32_t(version)) + kNameSize + reserve;
  if (r.block_size == 0 || r.block_size % kPage != 0 || r.block_count == 0) return false;
  if (r.frame_size < least || r.frame_size % kAlignment != 0 || r.frame_size > r.block_size) return false;
  const uint32_t per_block = r.block_size / r.frame_size;
  if (per_block == 0 || uint64_t{per_block} * r.block_count != r.frame_count) return false;
  if (version == Version::V3 && (r.private_size > r.block_size || kBlockHeader + align(r.private_size, 8) + least > r.block_size)) return false;
  return true;
}

void Receiver::start(Version version, const Request& request, uint32_t reserve) {
  *this = Receiver();
  sequence_ = 1;      // (blocks are numbered from 1)
  version_ = version;
  request_ = request;
  reserve_ = reserve;
}

void Receiver::open_block(ByteView ring, uint64_t now_ns) {
  const ByteView block = ring.subview(uint64_t{head_} * request_.block_size, request_.block_size);
  const uint32_t first = kBlockHeader + align(request_.private_size, 8);
  ByteWriter w{block.subview(0, kBlockHeader)};
  w.le32(2);                    // version (TPACKET_V3)
  w.le32(kBlockHeader);         // offset_to_priv
  w.le32(0);                    // block_status: the kernel's
  w.le32(0);                    // num_pkts
  w.le32(first);                // offset_to_first_pkt
  w.le32(first);                // blk_len (so far)
  w.le64(sequence_);
  w.zeros(16);
  open_ = true;
  used_ = first;
  packets_ = 0;
  last_ = 0;
  opened_ns_ = now_ns;
  first_ns_ = last_ns_ = 0;
}

void Receiver::close_block(ByteView ring, uint32_t status) {
  const ByteView block = ring.subview(uint64_t{head_} * request_.block_size, request_.block_size);
  if (packets_ != 0) {      // (the last frame is followed by none)
    ByteWriter next{block.subview(last_, 4)};
    next.le32(0);
  }
  ByteWriter w{block.subview(12, kBlockHeader - 12)};
  w.le32(packets_);
  w.le32(kBlockHeader + align(request_.private_size, 8));
  w.le32(used_);
  w.le64(sequence_);
  w.le32(uint32_t(first_ns_ / 1000000000u));
  w.le32(uint32_t(first_ns_ % 1000000000u));
  w.le32(uint32_t(last_ns_ / 1000000000u));
  w.le32(uint32_t(last_ns_ % 1000000000u));
  store_status(block.subview(kBlockStatusAt, 4), kStatusUser | status);      // (last: from here on the block is the program's)
  ++sequence_;
  head_ = (head_ + 1) % request_.block_count;
  open_ = false;
}

bool Receiver::put(ByteView ring, const Frame& frame, uint64_t now_ns) {
  if (ring.size() < request_.bytes() || request_.block_count == 0 || frame.name.size() != kNameSize) return false;
  const bool with_link = !frame.link.empty();
  const Layout at = layout_of(version_, with_link, reserve_);
  const uint64_t whole = frame.link.size() + frame.data.size();
  uint32_t kept = frame.kept < whole ? frame.kept : uint32_t(whole);
  uint32_t status = kStatusUser;
  if (version_ != Version::V3) {
    const ByteView slot = ring.subview(uint64_t{head_ / (request_.block_size / request_.frame_size)} * request_.block_size +
                                           uint64_t{head_ % (request_.block_size / request_.frame_size)} * request_.frame_size,
                                       request_.frame_size);
    if (load_status(slot.subview(0, 4)) != 0) return false;      // (the program still has this place)
    if (at.link >= request_.frame_size) return false;
    if (kept > request_.frame_size - at.link) {      // (what does not fit a frame is cut off, and the frame says so)
      kept = request_.frame_size - at.link;
      status |= kStatusCopy;
    }
    ByteWriter w{slot.subview(4, at.link - 4)};
    if (version_ == Version::V1) {
      w.le32(0);      // (the upper half of the status: an unsigned long)
      w.le32(uint32_t(whole));
      w.le32(kept);
      w.le16(uint16_t(at.link));
      w.le16(uint16_t(at.data));
      w.le32(uint32_t(frame.time_ns / 1000000000u));
      w.le32(uint32_t(frame.time_ns % 1000000000u / 1000u));
    } else {
      w.le32(uint32_t(whole));
      w.le32(kept);
      w.le16(uint16_t(at.link));
      w.le16(uint16_t(at.data));
      w.le32(uint32_t(frame.time_ns / 1000000000u));
      w.le32(uint32_t(frame.time_ns % 1000000000u));
      w.zeros(8);
    }
    slot.subview(at.name, kNameSize).copy_from(frame.name);
    const uint32_t from_link = kept < frame.link.size() ? kept : uint32_t(frame.link.size());
    slot.subview(at.link, from_link).copy_from(frame.link.first(from_link));
    slot.subview(at.link + from_link, kept - from_link).copy_from(frame.data.first(kept - from_link));
    store_status(slot.subview(0, 4), status);      // (last: from here on the frame is the program's)
    head_ = (head_ + 1) % request_.frame_count;
    return true;
  }
  // TPACKET_V3: frames follow one another in the block being filled; one that does not fit goes into the next block, if the program has given it back.
  const uint32_t first = kBlockHeader + align(request_.private_size, 8);
  if (at.link >= request_.block_size - first) return false;
  if (kept > request_.block_size - first - at.link) {
    kept = request_.block_size - first - at.link;
    status |= kStatusCopy;
  }
  const uint32_t size = align(at.link + kept, 8);
  for (int tries = 0; tries < 2; ++tries) {
    if (!open_) {
      const ByteView block = ring.subview(uint64_t{head_} * request_.block_size, request_.block_size);
      if (load_status(block.subview(kBlockStatusAt, 4)) != 0) {      // (the program still has the next block: frames are lost until it lets go)
        if (!was_frozen_) ++frozen_;
        was_frozen_ = true;
        return false;
      }
      was_frozen_ = false;
      open_block(ring, now_ns);
    }
    if (size <= request_.block_size - used_) break;
    close_block(ring, 0);
  }
  if (!open_ || size > request_.block_size - used_) return false;
  const ByteView block = ring.subview(uint64_t{head_} * request_.block_size, request_.block_size);
  const ByteView slot = block.subview(used_, size);
  ByteWriter w{slot.subview(0, at.name)};
  w.le32(size);      // tp_next_offset (0 once it turns out to be the block's last)
  w.le32(uint32_t(frame.time_ns / 1000000000u));
  w.le32(uint32_t(frame.time_ns % 1000000000u));
  w.le32(kept);
  w.le32(uint32_t(whole));
  w.le32(status);
  w.le16(uint16_t(at.link));
  w.le16(uint16_t(at.data));
  w.zeros(at.name - 28);
  slot.subview(at.name, kNameSize).copy_from(frame.name);
  const uint32_t from_link = kept < frame.link.size() ? kept : uint32_t(frame.link.size());
  slot.subview(at.link, from_link).copy_from(frame.link.first(from_link));
  slot.subview(at.link + from_link, kept - from_link).copy_from(frame.data.first(kept - from_link));
  if (packets_ == 0) first_ns_ = frame.time_ns;
  last_ns_ = frame.time_ns;
  last_ = used_;
  used_ += size;
  ++packets_;
  // (the counts a program may look at while the block fills)
  ByteWriter counts{block.subview(12, 12)};
  counts.le32(packets_);
  counts.le32(first);
  counts.le32(used_);
  return true;
}

void Receiver::tick(ByteView ring, uint64_t now_ns) {
  if (version_ != Version::V3 || !open_ || packets_ == 0 || ring.size() < request_.bytes()) return;
  const uint64_t wait = request_.retire_ms != 0 ? uint64_t{request_.retire_ms} * 1000000u : kDefaultRetireNs;
  if (now_ns - opened_ns_ >= wait) close_block(ring, kStatusBlockTimeout);
}

bool Receiver::readable(ConstByteView ring) const {
  if (ring.size() < request_.bytes() || request_.block_count == 0) return false;
  if (version_ == Version::V3) {      // the block before the one being filled: closed, and not given back yet
    const uint32_t previous = (head_ + request_.block_count - 1) % request_.block_count;
    return load_status(ring.subview(uint64_t{previous} * request_.block_size + kBlockStatusAt, 4)) != 0;
  }
  const uint32_t per_block = request_.block_size / request_.frame_size, previous = (head_ + request_.frame_count - 1) % request_.frame_count;
  return load_status(ring.subview(uint64_t{previous / per_block} * request_.block_size + uint64_t{previous % per_block} * request_.frame_size, 4)) != 0;
}

bool Transmitter::next(ByteView ring, bool use_offset, uint32_t most, bool skip_wrong, ConstByteView* out, Wrong* wrong) {
  *wrong = Wrong::No;
  if (ring.size() < request_.bytes() || request_.frame_count == 0) return false;
  const uint32_t per_block = request_.block_size / request_.frame_size;
  const ByteView slot = ring.subview(uint64_t{head_ / per_block} * request_.block_size + uint64_t{head_ % per_block} * request_.frame_size, request_.frame_size);
  const ByteView status = slot.subview(status_at(version_), 4);
  if (load_status(status) != kStatusSendRequest) return false;
  // What the program wrote in the header (read once: it may change it at any time): the frame's length, and where it starts if the socket says the
  // header tells (PACKET_TX_HAS_OFF); otherwise it follows the header.
  uint32_t length = 0, offset = 0;
  if (version_ == Version::V1) {
    ByteReader in{slot.subview(8, 12)};
    length = in.le32();
    in.skip(4);
    offset = in.le16();
  } else if (version_ == Version::V2) {
    ByteReader in{slot.subview(4, 10)};
    length = in.le32();
    in.skip(4);
    offset = in.le16();
  } else {
    ByteReader in{slot.subview(16, 4)};
    length = in.le32();
    ByteReader where{slot.subview(24, 2)};
    offset = where.le16();
  }
  const uint32_t header = align(header_size(uint32_t(version_)), kAlignment);
  if (!use_offset) offset = header;
  if (offset < header || offset > request_.frame_size || length > request_.frame_size - offset || length == 0 || length > most) {
    *wrong = length > most || (offset <= request_.frame_size && length > request_.frame_size - offset) ? Wrong::TooLong : Wrong::Malformed;
    if (skip_wrong) {      // (PACKET_LOSS: passed over, its place free again)
      store_status(status, 0);
      head_ = (head_ + 1) % request_.frame_count;
    } else {               // (it stays where it is, marked, for the program to put right)
      store_status(status, kStatusWrongFormat);
    }
    return true;
  }
  *out = slot.subview(offset, length);
  store_status(status, 0);      // TP_STATUS_AVAILABLE: taken (whoever called copies the frame before anything else touches the ring)
  head_ = (head_ + 1) % request_.frame_count;
  return true;
}

}  // namespace process::linuxabi::ring
