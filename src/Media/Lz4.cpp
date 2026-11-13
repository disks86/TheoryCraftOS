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

#include "Media/Lz4.hpp"
#include "Utility/ByteStream.hpp"

namespace media {

namespace {

// XXH32 (the checksum of the LZ4 frame format).
constexpr uint32_t kPrime1 = 2654435761u, kPrime2 = 2246822519u, kPrime3 = 3266489917u, kPrime4 = 668265263u, kPrime5 = 374761393u;

constexpr uint32_t rotl(uint32_t v, uint32_t n) { return (v << n) | (v >> (32 - n)); }

uint32_t le32_at(ConstByteView data, uint64_t i) {
  return uint32_t{data[i]} | (uint32_t{data[i + 1]} << 8) | (uint32_t{data[i + 2]} << 16) | (uint32_t{data[i + 3]} << 24);
}

uint32_t xxh32(ConstByteView data, uint32_t seed) {
  const uint64_t size = data.size();
  uint64_t i = 0;
  uint32_t h;
  if (size >= 16) {
    uint32_t v1 = seed + kPrime1 + kPrime2, v2 = seed + kPrime2, v3 = seed, v4 = seed - kPrime1;
    for (; size - i >= 16; i += 16) {
      v1 = rotl(v1 + le32_at(data, i) * kPrime2, 13) * kPrime1;
      v2 = rotl(v2 + le32_at(data, i + 4) * kPrime2, 13) * kPrime1;
      v3 = rotl(v3 + le32_at(data, i + 8) * kPrime2, 13) * kPrime1;
      v4 = rotl(v4 + le32_at(data, i + 12) * kPrime2, 13) * kPrime1;
    }
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
  } else {
    h = seed + kPrime5;
  }
  h += static_cast<uint32_t>(size);
  for (; size - i >= 4; i += 4) h = rotl(h + le32_at(data, i) * kPrime3, 17) * kPrime4;
  for (; i < size; ++i) h = rotl(h + data[i] * kPrime5, 11) * kPrime1;
  h ^= h >> 15;
  h *= kPrime2;
  h ^= h >> 13;
  h *= kPrime3;
  h ^= h >> 16;
  return h;
}

// One block into `out`, which may grow to `limit` bytes; matches reach back at most to `history_start`.
[[nodiscard]] bool decode_block(ConstByteView in, ByteBuffer* out, uint64_t history_start, uint64_t limit) {
  if (in.empty()) return false;
  ByteReader r(in);
  // A length nibble of 15 continues in bytes of 255 until a smaller one; false if the input ends first.
  auto extend = [&r](uint64_t* length) {
    uint8_t byte;
    do {
      byte = r.u8();
      if (!r.ok()) return false;
      *length += byte;  // cannot wrap: at most 255 per input byte
    } while (byte == 255);
    return true;
  };
  for (;;) {
    const uint8_t token = r.u8();
    if (!r.ok()) return false;
    uint64_t literals = token >> 4;
    if (literals == 15 && !extend(&literals)) return false;
    const ConstByteView run = r.take(literals);
    if (!r.ok() || literals > limit - out->size() || !out->append(run)) return false;
    if (r.at_end()) return true;  // the last sequence has literals only
    const uint64_t distance = r.le16();
    if (!r.ok() || distance == 0 || distance > out->size() - history_start) return false;
    uint64_t length = token & 15u;
    if (length == 15 && !extend(&length)) return false;
    length += 4;
    if (length > limit - out->size()) return false;
    const uint64_t at = out->size();
    if (!out->put_zeros(length)) return false;
    ByteView data = out->writable_view();
    for (uint64_t i = 0; i < length; ++i) data[at + i] = data[at + i - distance];
  }
}

constexpr uint32_t kFrameMagic = 0x184D2204;
constexpr uint32_t kSkippableMagic = 0x184D2A50, kSkippableMask = 0xFFFFFFF0;

// One frame (or skippable frame) starting at the reader's position.
[[nodiscard]] bool decode_frame(ByteReader& r, ByteBuffer* out, uint64_t limit) {
  const uint32_t magic = r.le32();
  if (!r.ok()) return false;
  if ((magic & kSkippableMask) == kSkippableMagic) return r.skip(r.le32());
  if (magic != kFrameMagic) return false;

  const uint64_t descriptor_start = r.position();
  const uint8_t flags = r.u8();
  const uint8_t block_descriptor = r.u8();
  if (!r.ok() || (flags >> 6) != 1 || (flags & 0x02) || (block_descriptor & 0x8F)) return false;
  const bool independent = flags & 0x20, block_checksum = flags & 0x10, has_content_size = flags & 0x08,
             content_checksum = flags & 0x04, has_dictionary = flags & 0x01;
  const uint32_t size_code = (block_descriptor >> 4) & 7;
  if (size_code < 4) return false;
  const uint64_t block_max = uint64_t{1} << (8 + 2 * size_code);  // 64 KiB, 256 KiB, 1 MiB, 4 MiB
  const uint64_t content_size = has_content_size ? r.le64() : 0;
  if (has_dictionary) return false;  // the dictionary is not ours to have
  const uint64_t descriptor_size = r.position() - descriptor_start;
  const uint8_t header_checksum = r.u8();
  if (!r.ok()) return false;
  if (static_cast<uint8_t>(xxh32(r.whole().subview(descriptor_start, descriptor_size), 0) >> 8) != header_checksum) return false;

  const uint64_t frame_start = out->size();
  for (;;) {
    const uint32_t word = r.le32();
    if (!r.ok()) return false;
    if (word == 0) break;  // end mark
    const uint64_t size = word & 0x7FFFFFFFu;
    if (size > block_max) return false;
    const ConstByteView block = r.take(size);
    if (block_checksum && xxh32(block, 0) != r.le32()) return false;
    if (!r.ok()) return false;
    const uint64_t room = limit - out->size();
    const uint64_t block_limit = out->size() + (block_max < room ? block_max : room);
    if (word & 0x80000000u) {  // stored uncompressed
      if (size > block_limit - out->size() || !out->append(block)) return false;
    } else {
      // Linked blocks may refer back into earlier blocks of the frame (64 KiB at most, which a 16 bit offset cannot exceed).
      if (!decode_block(block, out, independent ? out->size() : frame_start, block_limit)) return false;
    }
  }
  const ConstByteView content = out->view().subview(frame_start);
  if (has_content_size && content.size() != content_size) return false;
  if (content_checksum) {
    const uint32_t stored = r.le32();
    if (!r.ok() || xxh32(content, 0) != stored) return false;
  }
  return true;
}

}  // namespace

bool lz4_block_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  const uint64_t start = out->size();
  const uint64_t limit = max_output > UINT64_MAX - start ? UINT64_MAX : start + max_output;
  if (decode_block(in, out, start, limit)) return true;
  out->truncate(start);
  return false;
}

bool lz4_frame_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  const uint64_t start = out->size();
  const uint64_t limit = max_output > UINT64_MAX - start ? UINT64_MAX : start + max_output;
  ByteReader r(in);
  bool ok = !in.empty();
  while (ok && !r.at_end()) ok = decode_frame(r, out, limit);
  if (!ok) out->truncate(start);
  return ok;
}

}  // namespace media
