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

// Pieces shared by the QUIC code (RFC 9000): variable length integers, connection IDs, ranges of numbers, and the constants of the wire format.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Utility/ByteStream.hpp"

namespace network::quic {

constexpr uint32_t kVersion1 = 0x00000001;
constexpr uint32_t kMaxConnectionIdLength = 20;

// Variable length integers (RFC 9000 16): the top two bits of the first byte give the length (1, 2, 4 or 8 bytes).
constexpr uint64_t kMaxVarint = (uint64_t{1} << 62) - 1;
inline uint32_t varint_size(uint64_t v) { return v < 64 ? 1 : v < 16384 ? 2 : v < (uint64_t{1} << 30) ? 4 : 8; }
inline uint64_t read_varint(ByteReader& r) {
  uint8_t first = 0;
  if (!r.peek_u8(0, &first)) {
    r.take(1);   // (sets the error)
    return 0;
  }
  const uint32_t n = 1u << (first >> 6);
  const ConstByteView bytes = r.take(n);
  if (!r.ok()) return 0;
  uint64_t v = bytes[0] & 0x3F;
  for (uint32_t i = 1; i < n; ++i) v = (v << 8) | bytes[i];
  return v;
}
inline void write_varint(ByteWriter& w, uint64_t v) {
  const uint32_t n = varint_size(v);
  if (v > kMaxVarint) {
    w.zeros(w.space() + 1);   // (an error: it cannot be written)
    return;
  }
  const uint32_t prefix = n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : 3;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t b = static_cast<uint8_t>(v >> (8 * (n - 1 - i)));
    if (i == 0) b = static_cast<uint8_t>((b & 0x3F) | (prefix << 6));
    w.u8(b);
  }
}
// A varint of exactly 2 bytes (a length that is filled in later), for values below 16384.
inline void write_varint2(ByteWriter& w, uint32_t v) { w.be16(static_cast<uint16_t>(0x4000 | v)); }

struct ConnectionId {
  uint8_t length = 0;
  uint8_t bytes[kMaxConnectionIdLength] = {};
  ConstByteView view() const { return ConstByteView(bytes, length); }
  bool operator==(const ConnectionId& o) const {
    if (length != o.length) return false;
    for (uint32_t i = 0; i < length; ++i)
      if (bytes[i] != o.bytes[i]) return false;
    return true;
  }
  bool operator!=(const ConnectionId& o) const { return !(*this == o); }
  bool set(ConstByteView v) {
    if (v.size() > kMaxConnectionIdLength) return false;
    length = static_cast<uint8_t>(v.size());
    for (uint32_t i = 0; i < length; ++i) bytes[i] = v[i];
    return true;
  }
};

// A set of numbers kept as sorted, separate [first, last] ranges (packet numbers received; bytes of a stream acknowledged or lost).
struct Range {
  uint64_t first = 0, last = 0;   // inclusive
};
class RangeSet {
 public:
  explicit RangeSet(allocators::IAllocator& allocator) : ranges_(allocator) {}
  bool empty() const { return ranges_.count() == 0; }
  uint64_t count() const { return ranges_.count(); }
  // Ranges in ascending order.
  const Range& at(uint64_t i) const { return ranges_[i]; }
  uint64_t largest() const { return ranges_.count() ? ranges_[ranges_.count() - 1].last : 0; }
  uint64_t smallest() const { return ranges_.count() ? ranges_[0].first : 0; }
  bool contains(uint64_t v) const {
    for (uint64_t i = 0; i < ranges_.count(); ++i)
      if (v >= ranges_[i].first && v <= ranges_[i].last) return true;
    return false;
  }
  // Adds [first, last], merging with neighbours. False on memory shortage.
  bool add(uint64_t first, uint64_t last);
  // Removes everything below `v`.
  void remove_below(uint64_t v);
  // Removes [first, last].
  bool remove(uint64_t first, uint64_t last);
  void clear() { ranges_.clear(); }
  // How many numbers are in the set.
  uint64_t total() const {
    uint64_t n = 0;
    for (uint64_t i = 0; i < ranges_.count(); ++i) n += ranges_[i].last - ranges_[i].first + 1;
    return n;
  }
  // Keeps only the newest `max` ranges (the oldest are forgotten).
  void trim(uint64_t max) {
    while (ranges_.count() > max) ranges_.remove_at(0);
  }

 private:
  List<Range> ranges_;
};

}  // namespace network::quic
