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

// The block-compressed texture formats BC1 .. BC7: each 4 x 4 pixels are one block of 8 (BC1, BC4) or 16 bytes, rows of blocks top to bottom.
// A device with graphics::kFeatureCompressedFormats samples them as they are (virgl); for the others the memory manager decodes them when it uploads them: into 32-bit
// pixels (decode_blocks) or, for the formats whose values do not fit 8-bit unsigned channels (BC4 / BC5 SNORM, BC6H), into floats (decode_blocks_float, decoded_format).
// The decoding is the one of the Direct3D 11 functional specification (and the Khronos Data Format specification: S3TC, RGTC, BPTC):
//   - BC1 .. BC3: the two 5:6:5 endpoints expanded by bit replication, the interpolated colours rounded as (2a + b + 1) / 3 and (a + b + 1) / 2, BC1's three-colour mode
//     with transparent black when the first endpoint is not greater, BC2's 4-bit alphas times 17, BC3's eight- and six-value alpha ramps.
//   - BC4 / BC5: one or two channels coded like BC3's alpha. UNORM: 8-bit endpoints; SNORM: signed bytes, -128 read as -127, the ramp chosen by comparing the bytes as
//     they are; the extra values of the six-value ramp are -1 and +1. decode_blocks rounds the UNORM ramp like BC3's alpha; decode_blocks_float interpolates exactly.
//   - BC6H: 14 modes (10 with two regions and one of 32 partitions, 4 with one region), endpoints of 6 .. 16 bits, the other three as deltas from the first in the
//     transformed modes, unquantized to 16 bits, interpolated with 3- or 4-bit weights and scaled by 31/64 (unsigned) or 31/32 (signed) into half floats. The reserved
//     modes decode to 0. One judgement call: a signed 16-bit endpoint of -32768 (only mode 14 can hold one) is taken as -32767, so that the block cannot make -infinity
//     (BC6H has no infinities or NaNs: every other input finishes within +-0x7BFF).
//   - BC7: 8 modes with 1 .. 3 subsets (64 partitions each for 2 and 3), 4 .. 8-bit endpoints with shared or per-endpoint P-bits expanded by bit replication, 2 .. 4-bit
//     weights, the channel rotation and the index selection of modes 4 and 5. A block without a mode bit (first byte 0) is transparent black.
// Every bit pattern is a block: decoding never fails once the sizes are right.
#pragma once

#include "Graphics/GraphicsTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

namespace bc_detail {

// The bytes of a block are read through this: a view that is too short is refused before (decode_blocks checks the size), so every index is inside.
inline uint32_t le16(ConstByteView b, uint64_t at) { return uint32_t{b[at]} | (uint32_t{b[at + 1]} << 8); }
inline uint32_t le32(ConstByteView b, uint64_t at) { return le16(b, at) | (le16(b, at + 2) << 16); }
inline uint64_t le64(ConstByteView b, uint64_t at) { return uint64_t{le32(b, at)} | (uint64_t{le32(b, at + 4)} << 32); }

// The 16 pixels of a decoded block, in rows of four.
struct Pixels8 { uint32_t px[16]; };   // 0xAARRGGBB
struct PixelsF { Vec4 px[16]; };

// A 5:6:5 colour as 8-bit channels (the high bits repeated in the low ones).
struct Rgb { uint32_t r, g, b; };
inline Rgb expand565(uint32_t c) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}
inline uint32_t pack(uint32_t a, Rgb c) { return (a << 24) | (c.r << 16) | (c.g << 8) | c.b; }

// The four colours of a colour block (BC1's, or the colour half of BC2 / BC3, which always has four colours) as 0xAARRGGBB with alpha 255; `three` gets BC1's
// three-colour mode when the first endpoint is not greater than the second (the fourth colour is then transparent black).
inline void palette(uint32_t c0, uint32_t c1, bool allow_three, uint32_t out[4]) {
  const Rgb a = expand565(c0), b = expand565(c1);
  out[0] = pack(255, a);
  out[1] = pack(255, b);
  if (c0 > c1 || !allow_three) {
    out[2] = pack(255, {(2 * a.r + b.r + 1) / 3, (2 * a.g + b.g + 1) / 3, (2 * a.b + b.b + 1) / 3});
    out[3] = pack(255, {(a.r + 2 * b.r + 1) / 3, (a.g + 2 * b.g + 1) / 3, (a.b + 2 * b.b + 1) / 3});
  } else {
    out[2] = pack(255, {(a.r + b.r + 1) / 2, (a.g + b.g + 1) / 2, (a.b + b.b + 1) / 2});
    out[3] = 0;
  }
}

// BC3's alpha ramp (and BC4 / BC5 UNORM's in 8 bits): eight values from two endpoints, or six and 0 and 255.
inline void alpha_ramp(uint32_t a0, uint32_t a1, uint32_t out[8]) {
  out[0] = a0;
  out[1] = a1;
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) out[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (uint32_t i = 1; i < 5; ++i) out[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    out[6] = 0;
    out[7] = 255;
  }
}

// The 48 bits of 3-bit indices of a BC3 alpha / BC4 channel block (bytes 2 .. 7 of its 8).
inline uint64_t channel_indices(ConstByteView b) {
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 6; ++i) bits |= uint64_t{b[2 + i]} << (8 * i);
  return bits;
}

// BC1 .. BC3 blocks into 8-bit pixels.
inline void decode_bc123(ConstByteView b, bool bc1, bool bc2, Pixels8* out) {
  const uint64_t colour_at = bc1 ? 0 : 8;
  uint32_t colours[4];
  palette(le16(b, colour_at), le16(b, colour_at + 2), bc1, colours);
  const uint32_t indices = le32(b, colour_at + 4);
  uint32_t ramp[8] = {};
  uint64_t alpha_bits = 0;
  if (!bc1 && !bc2) {
    alpha_ramp(b[0], b[1], ramp);
    alpha_bits = channel_indices(b);
  }
  for (uint32_t n = 0; n < 16; ++n) {
    uint32_t c = colours[(indices >> (2 * n)) & 3];
    if (bc2) {
      const uint32_t a = (b[n / 2] >> (4 * (n & 1))) & 15;
      c = (c & 0xFFFFFFu) | ((a * 17) << 24);
    } else if (!bc1) {
      c = (c & 0xFFFFFFu) | (ramp[(alpha_bits >> (3 * n)) & 7] << 24);
    }
    out->px[n] = c;
  }
}

// One BC4 channel (an 8-byte half of a BC5 block) into 8-bit values (UNORM only).
inline void decode_channel8(ConstByteView b, uint32_t out[16]) {
  uint32_t ramp[8];
  alpha_ramp(b[0], b[1], ramp);
  const uint64_t bits = channel_indices(b);
  for (uint32_t n = 0; n < 16; ++n) out[n] = ramp[(bits >> (3 * n)) & 7];
}

// One BC4 channel into floats: UNORM 0 .. 1, SNORM -1 .. 1, interpolated exactly.
inline void decode_channel_float(ConstByteView b, bool snorm, float out[16]) {
  float ramp[8];
  if (snorm) {
    const int32_t raw0 = static_cast<int8_t>(b[0]), raw1 = static_cast<int8_t>(b[1]);
    const int32_t r0 = raw0 < -127 ? -127 : raw0, r1 = raw1 < -127 ? -127 : raw1;
    ramp[0] = static_cast<float>(r0) / 127.0f;
    ramp[1] = static_cast<float>(r1) / 127.0f;
    if (raw0 > raw1) {
      for (int32_t i = 1; i < 7; ++i) ramp[i + 1] = static_cast<float>((7 - i) * r0 + i * r1) / (7.0f * 127.0f);
    } else {
      for (int32_t i = 1; i < 5; ++i) ramp[i + 1] = static_cast<float>((5 - i) * r0 + i * r1) / (5.0f * 127.0f);
      ramp[6] = -1.0f;
      ramp[7] = 1.0f;
    }
  } else {
    const uint32_t r0 = b[0], r1 = b[1];
    ramp[0] = static_cast<float>(r0) / 255.0f;
    ramp[1] = static_cast<float>(r1) / 255.0f;
    if (r0 > r1) {
      for (uint32_t i = 1; i < 7; ++i) ramp[i + 1] = static_cast<float>((7 - i) * r0 + i * r1) / (7.0f * 255.0f);
    } else {
      for (uint32_t i = 1; i < 5; ++i) ramp[i + 1] = static_cast<float>((5 - i) * r0 + i * r1) / (5.0f * 255.0f);
      ramp[6] = 0.0f;
      ramp[7] = 1.0f;
    }
  }
  const uint64_t bits = channel_indices(b);
  for (uint32_t n = 0; n < 16; ++n) out[n] = ramp[(bits >> (3 * n)) & 7];
}

// The 128 bits of a 16-byte block, read from bit 0 up. A read past the end gives zero bits (it cannot happen with the mode tables below, whose layouts add up to 128).
class BlockBits {
 public:
  explicit BlockBits(ConstByteView b) : lo_(le64(b, 0)), hi_(le64(b, 8)) {}
  // The next n (<= 32) bits, the first one the lowest.
  uint32_t read(uint32_t n) {
    if (n == 0 || n > 32 || pos_ >= 128) {
      pos_ = pos_ + n > 128 ? 128 : pos_ + n;
      return 0;
    }
    uint64_t v;
    if (pos_ == 0) v = lo_;
    else if (pos_ < 64) v = (lo_ >> pos_) | (hi_ << (64 - pos_));
    else v = hi_ >> (pos_ - 64);
    pos_ = pos_ + n > 128 ? 128 : pos_ + n;
    return static_cast<uint32_t>(v & ((uint64_t{1} << n) - 1));
  }
  uint32_t position() const { return pos_; }

 private:
  uint64_t lo_, hi_;
  uint32_t pos_ = 0;
};

// The interpolation weights of BPTC (BC6H and BC7) for 2-, 3- and 4-bit indices.
constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
inline uint32_t weight(uint32_t bits, uint32_t index) {
  return bits == 2 ? kWeights2[index & 3] : bits == 3 ? kWeights3[index & 7] : kWeights4[index & 15];
}

// The 64 two-subset partitions (BC7; BC6H uses the first 32): bit n is the subset of pixel n.
constexpr uint16_t kPartitions2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
// The 64 three-subset partitions (BC7): the subset of each pixel.
constexpr uint8_t kPartitions3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1}, {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2},
    {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1}, {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2},
    {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1}, {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0},
    {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1}, {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1}, {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2},
    {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0}, {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0},
    {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1},
    {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1}, {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1}, {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2},
    {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2}, {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2}, {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};
// The anchor pixel (its index has one bit fewer) of the second subset of the two-subset partitions, and of the second and third subsets of the three-subset ones;
// the first subset's anchor is pixel 0.
constexpr uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
constexpr uint8_t kAnchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
constexpr uint8_t kAnchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};
constexpr uint32_t partition_subset(uint32_t subsets, uint32_t partition, uint32_t pixel) {
  return subsets == 2 ? (kPartitions2[partition & 63] >> (pixel & 15)) & 1 : subsets == 3 ? kPartitions3[partition & 63][pixel & 15] : 0;
}
constexpr bool is_anchor(uint32_t subsets, uint32_t partition, uint32_t pixel) {
  return pixel == 0 || (subsets == 2 && pixel == kAnchor2[partition & 63]) || (subsets == 3 && (pixel == kAnchor3a[partition & 63] || pixel == kAnchor3b[partition & 63]));
}
// Every anchor lies in its own subset.
constexpr bool anchors_consistent() {
  for (uint32_t p = 0; p < 64; ++p) {
    if (partition_subset(2, p, 0) != 0 || partition_subset(2, p, kAnchor2[p]) != 1) return false;
    if (partition_subset(3, p, 0) != 0 || partition_subset(3, p, kAnchor3a[p]) != 1 || partition_subset(3, p, kAnchor3b[p]) != 2) return false;
  }
  return true;
}
static_assert(anchors_consistent(), "BPTC anchor tables do not match the partitions");

// ---- BC7

struct Bc7Mode {
  uint8_t subsets, partition_bits, rotation_bits, selection_bits, colour_bits, alpha_bits, endpoint_pbits, shared_pbits, index_bits, index2_bits;
};
constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};
// Every mode's fields fill the 128 bits exactly.
constexpr bool bc7_modes_fill_blocks() {
  for (uint32_t m = 0; m < 8; ++m) {
    const Bc7Mode& d = kBc7Modes[m];
    const uint32_t endpoints = 2u * d.subsets;
    const uint32_t bits = (m + 1) + d.partition_bits + d.rotation_bits + d.selection_bits + endpoints * (3u * d.colour_bits + d.alpha_bits) + endpoints * d.endpoint_pbits +
                          d.subsets * d.shared_pbits + 16u * d.index_bits - d.subsets + (d.index2_bits ? 16u * d.index2_bits - 1 : 0);
    if (bits != 128) return false;
  }
  return true;
}
static_assert(bc7_modes_fill_blocks(), "BC7 mode table does not add up to 128 bits");

// A quantized endpoint channel of `bits` bits (5 .. 8, P-bit included) to 8 bits by repeating its high bits.
constexpr uint32_t expand_bits(uint32_t v, uint32_t bits) { return bits >= 8 ? v & 255 : ((v << (8 - bits)) | (v >> (2 * bits - 8))) & 255; }
constexpr uint32_t interpolate8(uint32_t a, uint32_t b, uint32_t w) { return ((64 - w) * a + w * b + 32) >> 6; }

inline void decode_bc7(ConstByteView b, Pixels8* out) {
  const uint32_t first = b[0];
  if (first == 0) {
    for (uint32_t& p : out->px) p = 0;
    return;
  }
  const uint32_t mode = static_cast<uint32_t>(__builtin_ctz(first));   // 0 .. 7: first is a nonzero byte
  const Bc7Mode& d = kBc7Modes[mode & 7];
  BlockBits bits(b);
  bits.read(mode + 1);
  const uint32_t partition = bits.read(d.partition_bits) & 63, rotation = bits.read(d.rotation_bits) & 3, selection = bits.read(d.selection_bits) & 1;
  uint32_t ep[3][2][4] = {};   // [subset][endpoint][r, g, b, a]
  const uint32_t subsets = d.subsets > 3 ? 3 : d.subsets;
  for (uint32_t c = 0; c < 3; ++c)
    for (uint32_t s = 0; s < subsets; ++s)
      for (auto& e : ep[s]) e[c] = bits.read(d.colour_bits);
  if (d.alpha_bits)
    for (uint32_t s = 0; s < subsets; ++s)
      for (auto& e : ep[s]) e[3] = bits.read(d.alpha_bits);
  uint32_t pbit[3][2] = {};
  const bool has_pbit = d.endpoint_pbits || d.shared_pbits;
  if (d.endpoint_pbits) {
    for (uint32_t s = 0; s < subsets; ++s)
      for (uint32_t& p : pbit[s]) p = bits.read(1);
  } else if (d.shared_pbits) {
    for (uint32_t s = 0; s < subsets; ++s) pbit[s][0] = pbit[s][1] = bits.read(1);
  }
  const uint32_t colour_precision = d.colour_bits + (has_pbit ? 1 : 0), alpha_precision = d.alpha_bits + (has_pbit ? 1 : 0);
  for (uint32_t s = 0; s < subsets; ++s) {
    for (uint32_t e = 0; e < 2; ++e) {
      for (uint32_t c = 0; c < 3; ++c) ep[s][e][c] = expand_bits(has_pbit ? (ep[s][e][c] << 1) | pbit[s][e] : ep[s][e][c], colour_precision);
      ep[s][e][3] = d.alpha_bits ? expand_bits(has_pbit ? (ep[s][e][3] << 1) | pbit[s][e] : ep[s][e][3], alpha_precision) : 255;
    }
  }
  uint32_t index[16], index2[16] = {};
  for (uint32_t n = 0; n < 16; ++n) index[n] = bits.read(d.index_bits - (is_anchor(subsets, partition, n) ? 1 : 0));
  if (d.index2_bits)
    for (uint32_t n = 0; n < 16; ++n) index2[n] = bits.read(d.index2_bits - (n == 0 ? 1 : 0));
  // Modes 4 and 5: colour from the first index set and alpha from the second; mode 4's selection bit swaps them.
  const uint32_t colour_index_bits = d.index2_bits && selection ? d.index2_bits : d.index_bits;
  const uint32_t alpha_index_bits = d.index2_bits ? (selection ? d.index_bits : d.index2_bits) : d.index_bits;
  for (uint32_t n = 0; n < 16; ++n) {
    const uint32_t s = partition_subset(subsets, partition, n);
    const uint32_t ci = d.index2_bits && selection ? index2[n] : index[n];
    const uint32_t ai = d.index2_bits ? (selection ? index[n] : index2[n]) : index[n];
    const uint32_t cw = weight(colour_index_bits, ci), aw = weight(alpha_index_bits, ai);
    uint32_t r = interpolate8(ep[s][0][0], ep[s][1][0], cw), g = interpolate8(ep[s][0][1], ep[s][1][1], cw), bl = interpolate8(ep[s][0][2], ep[s][1][2], cw);
    uint32_t a = interpolate8(ep[s][0][3], ep[s][1][3], aw);
    uint32_t t;
    if (rotation == 1) t = a, a = r, r = t;
    else if (rotation == 2) t = a, a = g, g = t;
    else if (rotation == 3) t = a, a = bl, bl = t;
    out->px[n] = (a << 24) | (r << 16) | (g << 8) | bl;
  }
}

// ---- BC6H

// The fields of a BC6H block header: the four endpoints (w and x of region 0, y and z of region 1) of each channel, and the partition.
enum Bc6Field : uint8_t { kRw, kRx, kRy, kRz, kGw, kGx, kGy, kGz, kBw, kBx, kBy, kBz, kPartition, kNoField };
// `count` bits of a field from bit `first` up, in the order the block holds them.
struct Bc6Run { uint8_t field, first, count; };
struct Bc6Mode {
  uint8_t mode_bits;      // 2 or 5
  bool two_regions, transformed;
  uint8_t endpoint_bits;
  uint8_t delta_bits[3];  // r, g, b (the bits of x, y, z; endpoint_bits when not transformed)
  Bc6Run runs[24];        // the header after the mode bits; unused runs have count 0
};
// The fourteen modes in the order of the Direct3D specification (mode 1 = 00, mode 2 = 01, mode 3 = 00010, ... mode 14 = 01111).
constexpr Bc6Mode kBc6Modes[14] = {
    {2, true, true, 10, {5, 5, 5}, {{kGy, 4, 1}, {kBy, 4, 1}, {kBz, 4, 1}, {kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 5}, {kGz, 4, 1}, {kGy, 0, 4}, {kGx, 0, 5},
                                    {kBz, 0, 1}, {kGz, 0, 4}, {kBx, 0, 5}, {kBz, 1, 1}, {kBy, 0, 4}, {kRy, 0, 5}, {kBz, 2, 1}, {kRz, 0, 5}, {kBz, 3, 1}, {kPartition, 0, 5}}},
    {2, true, true, 7, {6, 6, 6}, {{kGy, 5, 1}, {kGz, 4, 1}, {kGz, 5, 1}, {kRw, 0, 7}, {kBz, 0, 1}, {kBz, 1, 1}, {kBy, 4, 1}, {kGw, 0, 7}, {kBy, 5, 1}, {kBz, 2, 1},
                                   {kGy, 4, 1}, {kBw, 0, 7}, {kBz, 3, 1}, {kBz, 5, 1}, {kBz, 4, 1}, {kRx, 0, 6}, {kGy, 0, 4}, {kGx, 0, 6}, {kGz, 0, 4}, {kBx, 0, 6},
                                   {kBy, 0, 4}, {kRy, 0, 6}, {kRz, 0, 6}, {kPartition, 0, 5}}},
    {5, true, true, 11, {5, 4, 4}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 5}, {kRw, 10, 1}, {kGy, 0, 4}, {kGx, 0, 4}, {kGw, 10, 1}, {kBz, 0, 1}, {kGz, 0, 4},
                                    {kBx, 0, 4}, {kBw, 10, 1}, {kBz, 1, 1}, {kBy, 0, 4}, {kRy, 0, 5}, {kBz, 2, 1}, {kRz, 0, 5}, {kBz, 3, 1}, {kPartition, 0, 5}}},
    {5, true, true, 11, {4, 5, 4}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 4}, {kRw, 10, 1}, {kGz, 4, 1}, {kGy, 0, 4}, {kGx, 0, 5}, {kGw, 10, 1}, {kGz, 0, 4},
                                    {kBx, 0, 4}, {kBw, 10, 1}, {kBz, 1, 1}, {kBy, 0, 4}, {kRy, 0, 4}, {kBz, 0, 1}, {kBz, 2, 1}, {kRz, 0, 4}, {kGy, 4, 1}, {kBz, 3, 1},
                                    {kPartition, 0, 5}}},
    {5, true, true, 11, {4, 4, 5}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 4}, {kRw, 10, 1}, {kBy, 4, 1}, {kGy, 0, 4}, {kGx, 0, 4}, {kGw, 10, 1}, {kBz, 0, 1},
                                    {kGz, 0, 4}, {kBx, 0, 5}, {kBw, 10, 1}, {kBy, 0, 4}, {kRy, 0, 4}, {kBz, 1, 1}, {kBz, 2, 1}, {kRz, 0, 4}, {kBz, 4, 1}, {kBz, 3, 1},
                                    {kPartition, 0, 5}}},
    {5, true, true, 9, {5, 5, 5}, {{kRw, 0, 9}, {kBy, 4, 1}, {kGw, 0, 9}, {kGy, 4, 1}, {kBw, 0, 9}, {kBz, 4, 1}, {kRx, 0, 5}, {kGz, 4, 1}, {kGy, 0, 4}, {kGx, 0, 5},
                                   {kBz, 0, 1}, {kGz, 0, 4}, {kBx, 0, 5}, {kBz, 1, 1}, {kBy, 0, 4}, {kRy, 0, 5}, {kBz, 2, 1}, {kRz, 0, 5}, {kBz, 3, 1}, {kPartition, 0, 5}}},
    {5, true, true, 8, {6, 5, 5}, {{kRw, 0, 8}, {kGz, 4, 1}, {kBy, 4, 1}, {kGw, 0, 8}, {kBz, 2, 1}, {kGy, 4, 1}, {kBw, 0, 8}, {kBz, 3, 1}, {kBz, 4, 1}, {kRx, 0, 6},
                                   {kGy, 0, 4}, {kGx, 0, 5}, {kBz, 0, 1}, {kGz, 0, 4}, {kBx, 0, 5}, {kBz, 1, 1}, {kBy, 0, 4}, {kRy, 0, 6}, {kRz, 0, 6}, {kPartition, 0, 5}}},
    {5, true, true, 8, {5, 6, 5}, {{kRw, 0, 8}, {kBz, 0, 1}, {kBy, 4, 1}, {kGw, 0, 8}, {kGy, 5, 1}, {kGy, 4, 1}, {kBw, 0, 8}, {kGz, 5, 1}, {kBz, 4, 1}, {kRx, 0, 5},
                                   {kGz, 4, 1}, {kGy, 0, 4}, {kGx, 0, 6}, {kGz, 0, 4}, {kBx, 0, 5}, {kBz, 1, 1}, {kBy, 0, 4}, {kRy, 0, 5}, {kBz, 2, 1}, {kRz, 0, 5},
                                   {kBz, 3, 1}, {kPartition, 0, 5}}},
    {5, true, true, 8, {5, 5, 6}, {{kRw, 0, 8}, {kBz, 1, 1}, {kBy, 4, 1}, {kGw, 0, 8}, {kBy, 5, 1}, {kGy, 4, 1}, {kBw, 0, 8}, {kBz, 5, 1}, {kBz, 4, 1}, {kRx, 0, 5},
                                   {kGz, 4, 1}, {kGy, 0, 4}, {kGx, 0, 5}, {kBz, 0, 1}, {kGz, 0, 4}, {kBx, 0, 6}, {kBy, 0, 4}, {kRy, 0, 5}, {kBz, 2, 1}, {kRz, 0, 5},
                                   {kBz, 3, 1}, {kPartition, 0, 5}}},
    {5, true, false, 6, {6, 6, 6}, {{kRw, 0, 6}, {kGz, 4, 1}, {kBz, 0, 1}, {kBz, 1, 1}, {kBy, 4, 1}, {kGw, 0, 6}, {kGy, 5, 1}, {kBy, 5, 1}, {kBz, 2, 1}, {kGy, 4, 1},
                                    {kBw, 0, 6}, {kGz, 5, 1}, {kBz, 3, 1}, {kBz, 5, 1}, {kBz, 4, 1}, {kRx, 0, 6}, {kGy, 0, 4}, {kGx, 0, 6}, {kGz, 0, 4}, {kBx, 0, 6},
                                    {kBy, 0, 4}, {kRy, 0, 6}, {kRz, 0, 6}, {kPartition, 0, 5}}},
    {5, false, false, 10, {10, 10, 10}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 10}, {kGx, 0, 10}, {kBx, 0, 10}}},
    {5, false, true, 11, {9, 9, 9}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 9}, {kRw, 10, 1}, {kGx, 0, 9}, {kGw, 10, 1}, {kBx, 0, 9}, {kBw, 10, 1}}},
    // modes 13 and 14 hold the high bits of the first endpoint reversed (the highest first)
    {5, false, true, 12, {8, 8, 8}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 8}, {kRw, 11, 1}, {kRw, 10, 1}, {kGx, 0, 8}, {kGw, 11, 1}, {kGw, 10, 1},
                                     {kBx, 0, 8}, {kBw, 11, 1}, {kBw, 10, 1}}},
    {5, false, true, 16, {4, 4, 4}, {{kRw, 0, 10}, {kGw, 0, 10}, {kBw, 0, 10}, {kRx, 0, 4}, {kRw, 15, 1}, {kRw, 14, 1}, {kRw, 13, 1}, {kRw, 12, 1}, {kRw, 11, 1},
                                     {kRw, 10, 1}, {kGx, 0, 4}, {kGw, 15, 1}, {kGw, 14, 1}, {kGw, 13, 1}, {kGw, 12, 1}, {kGw, 11, 1}, {kGw, 10, 1}, {kBx, 0, 4},
                                     {kBw, 15, 1}, {kBw, 14, 1}, {kBw, 13, 1}, {kBw, 12, 1}, {kBw, 11, 1}, {kBw, 10, 1}}},
};
// Each mode's header is 82 bits (two regions; 46 index bits follow) or 65 (one region; 63 index bits), and gives every field exactly its bits, each once.
constexpr bool bc6_modes_consistent() {
  for (const Bc6Mode& m : kBc6Modes) {
    uint32_t total = m.mode_bits, seen[kNoField] = {};
    for (const Bc6Run& r : m.runs) {
      if (r.count == 0) continue;
      if (r.field >= kNoField || r.first + r.count > 16) return false;
      const uint32_t bits = ((1u << r.count) - 1) << r.first;
      if (seen[r.field] & bits) return false;
      seen[r.field] |= bits;
      total += r.count;
    }
    if (total != (m.two_regions ? 82u : 65u)) return false;
    for (uint32_t c = 0; c < 3; ++c) {
      if (seen[c * 4] != (1u << m.endpoint_bits) - 1) return false;
      for (uint32_t e = 1; e < 4; ++e) {
        const uint32_t want = e == 1 || m.two_regions ? (1u << m.delta_bits[c]) - 1 : 0;
        if (seen[c * 4 + e] != want) return false;
      }
      if (!m.transformed && m.delta_bits[c] != m.endpoint_bits) return false;
    }
    if (seen[kPartition] != (m.two_regions ? 31u : 0u)) return false;
  }
  return true;
}
static_assert(bc6_modes_consistent(), "BC6H mode table is inconsistent");
// The mode of the low 5 bits of a block (the 2-bit modes 00 and 01 for any upper bits): its index in kBc6Modes, or -1 for the reserved ones.
constexpr int8_t bc6_mode_of(uint32_t low5) {
  if ((low5 & 3) == 0) return 0;
  if ((low5 & 3) == 1) return 1;
  constexpr uint8_t kFive[12] = {0x02, 0x06, 0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x03, 0x07, 0x0B, 0x0F};
  for (uint32_t i = 0; i < 12; ++i)
    if (kFive[i] == (low5 & 31)) return static_cast<int8_t>(i + 2);
  return -1;
}

constexpr int32_t sign_extend(uint32_t v, uint32_t bits) {
  const uint32_t m = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
  v &= m;
  return bits == 0 || bits >= 32 ? static_cast<int32_t>(v) : (v & (1u << (bits - 1))) ? static_cast<int32_t>(v) - static_cast<int32_t>(1u << bits) : static_cast<int32_t>(v);
}
constexpr int32_t unquantize_bc6(int32_t comp, uint32_t bits, bool is_signed) {
  if (!is_signed) {
    if (bits >= 15 || comp == 0) return comp;
    if (comp == static_cast<int32_t>((1u << bits) - 1)) return 0xFFFF;
    return ((comp << 16) + 0x8000) >> bits;
  }
  if (bits >= 16) return comp < -32767 ? -32767 : comp;   // (-32768 taken as -32767: see the top of the file)
  const bool negative = comp < 0;
  int32_t magnitude = negative ? -comp : comp, unq;
  if (magnitude == 0) unq = 0;
  else if (magnitude >= static_cast<int32_t>((1u << (bits - 1)) - 1)) unq = 0x7FFF;
  else unq = ((magnitude << 15) + 0x4000) >> (bits - 1);
  return negative ? -unq : unq;
}
constexpr uint16_t finish_bc6(int32_t v, bool is_signed) {
  if (!is_signed) return static_cast<uint16_t>((v * 31) >> 6);
  return v < 0 ? static_cast<uint16_t>((((-v) * 31) >> 5) | 0x8000) : static_cast<uint16_t>((v * 31) >> 5);
}

// A BC6H block into half floats: out[n][0 .. 2] r, g, b of pixel n.
inline void decode_bc6(ConstByteView b, bool is_signed, uint16_t out[16][3]) {
  const int8_t mode_index = bc6_mode_of(b[0] & 31);
  if (mode_index < 0) {
    for (uint32_t n = 0; n < 16; ++n) out[n][0] = out[n][1] = out[n][2] = 0;
    return;
  }
  const Bc6Mode& m = kBc6Modes[static_cast<uint32_t>(mode_index) % 14];
  BlockBits bits(b);
  bits.read(m.mode_bits);
  uint32_t field[kNoField] = {};
  for (const Bc6Run& r : m.runs)
    if (r.count && r.field < kNoField) field[r.field] |= bits.read(r.count) << (r.first & 15);
  const uint32_t regions = m.two_regions ? 2 : 1, eb = m.endpoint_bits, mask = (1u << eb) - 1;
  int32_t ep[4][3];   // [w, x, y, z][r, g, b], unquantized
  for (uint32_t c = 0; c < 3; ++c) {
    int32_t e[4];
    const uint32_t w = field[c * 4] & mask;
    e[0] = is_signed ? sign_extend(w, eb) : static_cast<int32_t>(w);
    for (uint32_t k = 1; k < 4; ++k) {
      const uint32_t v = field[c * 4 + k];
      if (m.transformed) {
        const uint32_t sum = (w + static_cast<uint32_t>(sign_extend(v, m.delta_bits[c]))) & mask;
        e[k] = is_signed ? sign_extend(sum, eb) : static_cast<int32_t>(sum);
      } else {
        e[k] = is_signed ? sign_extend(v, eb) : static_cast<int32_t>(v & mask);
      }
    }
    for (uint32_t k = 0; k < 4; ++k) ep[k][c] = unquantize_bc6(e[k], eb, is_signed);
  }
  const uint32_t partition = m.two_regions ? field[kPartition] & 31 : 0, index_bits = m.two_regions ? 3 : 4;
  for (uint32_t n = 0; n < 16; ++n) {
    const uint32_t region = regions == 2 ? partition_subset(2, partition, n) : 0;
    const uint32_t index = bits.read(index_bits - (is_anchor(regions, partition, n) ? 1 : 0));
    const int32_t w = static_cast<int32_t>(weight(index_bits, index));
    for (uint32_t c = 0; c < 3; ++c) {
      const int32_t v = (ep[region * 2][c] * (64 - w) + ep[region * 2 + 1][c] * w + 32) >> 6;
      out[n][c] = finish_bc6(v, is_signed);
    }
  }
}

inline Vec4 unpack_float(uint32_t c) {
  return {static_cast<float>((c >> 16) & 255) / 255.0f, static_cast<float>((c >> 8) & 255) / 255.0f, static_cast<float>(c & 255) / 255.0f,
          static_cast<float>(c >> 24) / 255.0f};
}

}  // namespace bc_detail

constexpr bool is_compressed_format(Format f) {
  switch (f) {
    case Format::Bc1Unorm: case Format::Bc2Unorm: case Format::Bc3Unorm: case Format::Bc1Srgb: case Format::Bc2Srgb: case Format::Bc3Srgb:
    case Format::Bc4Unorm: case Format::Bc4Snorm: case Format::Bc5Unorm: case Format::Bc5Snorm: case Format::Bc6hUfloat: case Format::Bc6hSfloat:
    case Format::Bc7Unorm: case Format::Bc7Srgb:
      return true;
    default:
      return false;
  }
}
// The bytes of one 4 x 4 block (0 for a format that is not block-compressed).
constexpr uint32_t compressed_block_bytes(Format f) {
  return f == Format::Bc1Unorm || f == Format::Bc1Srgb || f == Format::Bc4Unorm || f == Format::Bc4Snorm ? 8 : is_compressed_format(f) ? 16 : 0;
}
// The bytes of the blocks of a w x h image, and of one row of blocks.
constexpr uint64_t compressed_row_bytes(Format f, uint32_t w) { return ((uint64_t{w} + 3) / 4) * compressed_block_bytes(f); }
constexpr uint64_t compressed_image_bytes(Format f, uint32_t w, uint32_t h) { return compressed_row_bytes(f, w) * ((uint64_t{h} + 3) / 4); }
// The format the pixels of a compressed format are decoded into for a device that cannot sample it: 32-bit pixels for the formats whose values are 8-bit unsigned
// (decode_blocks), floats for BC4 / BC5 SNORM and BC6H (decode_blocks_float). B8G8R8A8Unorm for anything else.
constexpr Format decoded_format(Format f) {
  switch (f) {
    case Format::Bc1Srgb: case Format::Bc2Srgb: case Format::Bc3Srgb: case Format::Bc7Srgb: return Format::B8G8R8A8Srgb;
    case Format::Bc4Snorm: case Format::Bc5Snorm: return Format::R32G32B32A32Float;
    case Format::Bc6hUfloat: case Format::Bc6hSfloat: return Format::R16G16B16A16Float;
    default: return Format::B8G8R8A8Unorm;
  }
}
// Whether a compressed format decodes to floats (decode_blocks_float) rather than 32-bit pixels.
constexpr bool decodes_to_floats(Format f) { return f == Format::Bc4Snorm || f == Format::Bc5Snorm || f == Format::Bc6hUfloat || f == Format::Bc6hSfloat; }

namespace bc_detail {

// Checks the sizes, decodes every block with `block(view, pixels)` and writes the pixels that lie in the image.
template <typename Pixel, typename Pixels, typename DecodeBlock>
bool decode_image(Format f, ConstByteView blocks, uint32_t w, uint32_t h, ArrayView<Pixel> out, DecodeBlock block) {
  const uint32_t size = compressed_block_bytes(f);
  if (size == 0 || w == 0 || h == 0 || blocks.size() != compressed_image_bytes(f, w, h) || out.size() != uint64_t{w} * h) return false;
  const uint64_t across = (uint64_t{w} + 3) / 4, down = (uint64_t{h} + 3) / 4;
  for (uint64_t by = 0; by < down; ++by) {
    for (uint64_t bx = 0; bx < across; ++bx) {
      Pixels px;
      block(blocks.subview((by * across + bx) * size, size), &px);
      for (uint64_t py = 0; py < 4 && by * 4 + py < h; ++py)
        for (uint64_t qx = 0; qx < 4 && bx * 4 + qx < w; ++qx) out[(by * 4 + py) * w + bx * 4 + qx] = px.px[py * 4 + qx];
    }
  }
  return true;
}

}  // namespace bc_detail

// Decodes the blocks of a w x h image (`blocks`: compressed_image_bytes of them, rows of blocks top to bottom) into w * h pixels, 0xAARRGGBB, rows top to bottom (the
// blocks of the right and bottom edges are cut to the image): BC1 .. BC3, BC4 UNORM (red, green and blue 0, alpha 255), BC5 UNORM (red, green, blue 0, alpha 255) and BC7
// (the encoded values: sRGB is not linearised). False when a view has the wrong size or the format is not one of these (decodes_to_floats); `out` is then unchanged.
inline bool decode_blocks(Format f, ConstByteView blocks, uint32_t w, uint32_t h, ArrayView<uint32_t> out) {
  using namespace bc_detail;
  if (!is_compressed_format(f) || decodes_to_floats(f)) return false;
  return decode_image<uint32_t, Pixels8>(f, blocks, w, h, out, [f](ConstByteView b, Pixels8* px) {
    switch (f) {
      case Format::Bc4Unorm: {
        uint32_t r[16];
        decode_channel8(b, r);
        for (uint32_t n = 0; n < 16; ++n) px->px[n] = 0xFF000000u | (r[n] << 16);
        break;
      }
      case Format::Bc5Unorm: {
        uint32_t r[16], g[16];
        decode_channel8(b.subview(0, 8), r);
        decode_channel8(b.subview(8, 8), g);
        for (uint32_t n = 0; n < 16; ++n) px->px[n] = 0xFF000000u | (r[n] << 16) | (g[n] << 8);
        break;
      }
      case Format::Bc7Unorm: case Format::Bc7Srgb: decode_bc7(b, px); break;
      default: decode_bc123(b, f == Format::Bc1Unorm || f == Format::Bc1Srgb, f == Format::Bc2Unorm || f == Format::Bc2Srgb, px); break;
    }
  });
}

// Decodes the blocks of any block-compressed format into w * h Vec4s (r, g, b, a), as decode_blocks does: the 8-bit values of BC1 .. BC3 and BC7 as c / 255, BC4 / BC5
// UNORM 0 .. 1 and SNORM -1 .. 1 (interpolated exactly), BC6H's half floats; the channels a format does not have are 0 and alpha 1.
inline bool decode_blocks_float(Format f, ConstByteView blocks, uint32_t w, uint32_t h, ArrayView<Vec4> out) {
  using namespace bc_detail;
  if (!is_compressed_format(f)) return false;
  return decode_image<Vec4, PixelsF>(f, blocks, w, h, out, [f](ConstByteView b, PixelsF* px) {
    switch (f) {
      case Format::Bc4Unorm: case Format::Bc4Snorm: {
        float r[16];
        decode_channel_float(b, f == Format::Bc4Snorm, r);
        for (uint32_t n = 0; n < 16; ++n) px->px[n] = {r[n], 0.0f, 0.0f, 1.0f};
        break;
      }
      case Format::Bc5Unorm: case Format::Bc5Snorm: {
        float r[16], g[16];
        decode_channel_float(b.subview(0, 8), f == Format::Bc5Snorm, r);
        decode_channel_float(b.subview(8, 8), f == Format::Bc5Snorm, g);
        for (uint32_t n = 0; n < 16; ++n) px->px[n] = {r[n], g[n], 0.0f, 1.0f};
        break;
      }
      case Format::Bc6hUfloat: case Format::Bc6hSfloat: {
        uint16_t half[16][3];
        decode_bc6(b, f == Format::Bc6hSfloat, half);
        for (uint32_t n = 0; n < 16; ++n) px->px[n] = {half_to_float(half[n][0]), half_to_float(half[n][1]), half_to_float(half[n][2]), 1.0f};
        break;
      }
      default: {
        Pixels8 p8;
        if (f == Format::Bc7Unorm || f == Format::Bc7Srgb) decode_bc7(b, &p8);
        else decode_bc123(b, f == Format::Bc1Unorm || f == Format::Bc1Srgb, f == Format::Bc2Unorm || f == Format::Bc2Srgb, &p8);
        for (uint32_t n = 0; n < 16; ++n) px->px[n] = unpack_float(p8.px[n]);
        break;
      }
    }
  });
}

}  // namespace graphics
