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
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteStream.hpp"

// The pieces of EBML (the binary XML of Matroska and WebM) that readers share: element headers, variable length integers, and the numbers an
// element's payload holds. Header only.
namespace media {
namespace ebml {

// The id of an element (1-4 bytes, the length marker kept) and its size (1-8 bytes; `unknown` when all bits are 1, as live streams write). Result: 1 done,
// 0 more bytes are needed, -1 it is not an element.
inline int header(ConstByteView d, uint32_t* id, uint64_t* size, bool* unknown, uint32_t* length) {
  if (d.empty()) return 0;
  uint32_t id_length = 1;
  while (id_length <= 4 && !(d[0] & (0x80 >> (id_length - 1)))) ++id_length;
  if (id_length > 4) return -1;
  if (d.size() < id_length + 1u) return 0;
  uint32_t value = 0;
  for (uint32_t i = 0; i < id_length; ++i) value = (value << 8) | d[i];
  const uint8_t first = d[id_length];
  uint32_t size_length = 1;
  while (size_length <= 8 && !(first & (0x80 >> (size_length - 1)))) ++size_length;
  if (size_length > 8) return -1;
  if (d.size() < id_length + size_length) return 0;
  uint64_t v = first & (0xFFu >> size_length);
  bool all_ones = v == (0xFFu >> size_length);
  for (uint32_t i = 1; i < size_length; ++i) {
    v = (v << 8) | d[id_length + i];
    all_ones = all_ones && d[id_length + i] == 0xFF;
  }
  *id = value;
  *size = v;
  *unknown = all_ones;
  *length = id_length + size_length;
  return 1;
}

// A variable length integer (a track number, a lace size) at the reader's place.
inline bool vint(ByteReader& r, uint64_t* out, uint32_t* length = nullptr) {
  uint8_t first;
  if (!r.peek_u8(0, &first)) return false;
  uint32_t n = 1;
  while (n <= 8 && !(first & (0x80 >> (n - 1)))) ++n;
  if (n > 8) return false;
  uint64_t v = first & (0xFFu >> n);
  r.u8();
  for (uint32_t i = 1; i < n; ++i) v = (v << 8) | r.u8();
  if (length) *length = n;
  *out = v;
  return r.ok();
}

inline uint64_t to_uint(ConstByteView d) {
  uint64_t v = 0;
  for (uint64_t i = 0; i < d.size() && i < 8; ++i) v = (v << 8) | d[i];
  return v;
}

// A signed integer of 1 to 8 bytes (two's complement, big-endian).
inline int64_t to_int(ConstByteView d) {
  if (d.empty() || d.size() > 8) return 0;
  uint64_t v = (d[0] & 0x80) ? ~uint64_t{0} : 0;
  for (uint64_t i = 0; i < d.size(); ++i) v = (v << 8) | d[i];
  return static_cast<int64_t>(v);
}

inline double to_float(ConstByteView d) {
  if (d.size() == 4) {
    const uint32_t bits = static_cast<uint32_t>(to_uint(d));
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
  }
  if (d.size() == 8) {
    const uint64_t bits = to_uint(d);
    double f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
  }
  return 0;
}

// Element ids (with their length markers).
constexpr uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kInfo = 0x1549A966, kTracks = 0x1654AE6B,
                   kCluster = 0x1F43B675, kCues = 0x1C53BB6B, kTags = 0x1254C367, kChapters = 0x1043A770, kAttachments = 0x1941A469, kVoid = 0xEC,
                   kTimecodeScale = 0x2AD7B1, kDuration = 0x4489, kTimecode = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1,
                   kBlockDuration = 0x9B, kReferenceBlock = 0xFB, kDiscardPadding = 0x75A2, kEncryptedBlock = 0xAF, kTrackEntry = 0xAE,
                   kTrackNumber = 0xD7, kTrackType = 0x83, kFlagEnabled = 0xB9, kFlagDefault = 0x88, kCodecId = 0x86, kCodecPrivate = 0x63A2,
                   kCodecDelay = 0x56AA, kSeekPreRoll = 0x56BB, kDefaultDuration = 0x23E383, kVideo = 0xE0, kAudio = 0xE1, kPixelWidth = 0xB0,
                   kPixelHeight = 0xBA, kDisplayWidth = 0x54B0, kDisplayHeight = 0x54BA, kSamplingFrequency = 0xB5, kOutputSamplingFrequency = 0x78B5,
                   kChannels = 0x9F, kBitDepth = 0x6264, kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kContentEncodingOrder = 0x5031,
                   kContentEncodingScope = 0x5032, kContentEncodingType = 0x5033, kContentCompression = 0x5034, kContentCompAlgo = 0x4254,
                   kContentCompSettings = 0x4255, kContentEncryption = 0x5035, kContentEncAlgo = 0x47E1, kContentEncKeyId = 0x47E2,
                   kContentEncAesSettings = 0x47E7, kAesSettingsCipherMode = 0x47E8;

// True for the ids that begin a child of the segment: the end of a cluster of unknown size is where one of these shows up.
inline bool is_segment_child(uint32_t id) {
  return id == kCluster || id == kCues || id == kSeekHead || id == kInfo || id == kTracks || id == kTags || id == kChapters || id == kAttachments;
}

}  // namespace ebml
}  // namespace media
