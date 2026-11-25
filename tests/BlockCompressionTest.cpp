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

// Graphics/BlockCompression.hpp: BC1 .. BC7 blocks with values worked out by hand from the Direct3D specification (both BC1 modes, BC2's alphas, both BC3 ramps, both BC4 / BC5
// ramps UNORM and SNORM, BC7 modes 1, 4 (both index selections), 5, 6 and the block without a mode, BC6H modes 1, 11 and 14 and the reserved modes), images whose edges cut
// blocks, the sizes, views of the wrong size (shortened), blocks with a bit turned or of random bytes, which always decode to values in the format's range (every bit pattern
// is a block), and 256 random blocks of each of BC4 .. BC7 (every BC6H and BC7 mode among them) compared with Mesa's decoder (tests/data/bc/reference.bin, made by
// scripts/bc-reference.c).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <cstring>
#include <vector>
#include "Graphics/BlockCompression.hpp"
#include "Utility/ArrayView.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

namespace {
int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

using graphics::Format;

bool decode(Format f, const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h, std::vector<uint32_t>* out) {
  out->assign(uint64_t{w} * h, 0xDEADBEEFu);
  return graphics::decode_blocks(f, ConstByteView(blocks.data(), blocks.size()), w, h, ArrayView<uint32_t>(out->data(), out->size()));
}
// A colour block: endpoints c0, c1 (5:6:5) and the 2-bit indices of the 16 pixels.
void colour_block(std::vector<uint8_t>* b, uint16_t c0, uint16_t c1, uint32_t indices) {
  b->push_back(c0 & 0xFF); b->push_back(c0 >> 8); b->push_back(c1 & 0xFF); b->push_back(c1 >> 8);
  for (int i = 0; i < 4; ++i) b->push_back((indices >> (8 * i)) & 0xFF);
}

void test_bc1() {
  // red (0xF800) and blue (0x001F), four-colour mode: pixel n has index n % 4
  std::vector<uint8_t> b;
  uint32_t indices = 0;
  for (int n = 0; n < 16; ++n) indices |= uint32_t(n % 4) << (2 * n);
  colour_block(&b, 0xF800, 0x001F, indices);
  std::vector<uint32_t> px;
  CHECK(decode(Format::Bc1Unorm, b, 4, 4, &px));
  CHECK(px[0] == 0xFFFF0000u && px[1] == 0xFF0000FFu);
  CHECK(px[2] == 0xFFAA0055u && px[3] == 0xFF5500AAu);     // (2 * 255 + 0 + 1) / 3 = 170, (255 + 1) / 3 = 85
  CHECK(px[15] == px[3] && px[4] == px[0]);
  // three-colour mode (c0 <= c1): index 2 is the average, 3 transparent black
  b.clear();
  colour_block(&b, 0x001F, 0xF800, indices);
  CHECK(decode(Format::Bc1Unorm, b, 4, 4, &px));
  CHECK(px[0] == 0xFF0000FFu && px[1] == 0xFFFF0000u && px[2] == 0xFF800080u && px[3] == 0x00000000u);
  // the 6-bit green is expanded by repeating its high bits: 0x07E0 is green 255; 0x0400 is green 32 << 2 | 32 >> 4 = 130
  b.clear();
  colour_block(&b, 0x07E0, 0x0400, 0x4);   // (pixel 1 has index 1)
  CHECK(decode(Format::Bc1Unorm, b, 4, 4, &px));
  CHECK(px[0] == 0xFF00FF00u && px[1] == 0xFF008200u);
}

void test_bc2_bc3() {
  // BC2: sixteen 4-bit alphas 0 .. 15 (times 17), then a white block
  std::vector<uint8_t> b;
  for (int i = 0; i < 8; ++i) b.push_back(uint8_t((2 * i) | ((2 * i + 1) << 4)));
  colour_block(&b, 0xFFFF, 0xFFFF, 0);
  std::vector<uint32_t> px;
  CHECK(decode(Format::Bc2Unorm, b, 4, 4, &px));
  bool alphas = true;
  for (int n = 0; n < 16; ++n) alphas = alphas && px[n] == ((uint32_t(n * 17) << 24) | 0xFFFFFFu);
  CHECK(alphas);
  // BC2's colour block always has four colours, even when c0 <= c1
  b.assign(8, 0xFF);
  colour_block(&b, 0x001F, 0xF800, 0xFFFFFFFFu);   // (every pixel index 3)
  CHECK(decode(Format::Bc2Unorm, b, 4, 4, &px));
  CHECK(px[0] == 0xFFAA0055u);
  // BC3, eight-value ramp: a0 = 255, a1 = 0; indices 0 .. 7 in pixels 0 .. 7
  b.clear();
  b.push_back(255); b.push_back(0);
  uint64_t bits = 0;
  for (int n = 0; n < 16; ++n) bits |= uint64_t(n % 8) << (3 * n);
  for (int i = 0; i < 6; ++i) b.push_back(uint8_t(bits >> (8 * i)));
  colour_block(&b, 0x0000, 0x0000, 0);
  CHECK(decode(Format::Bc3Unorm, b, 4, 4, &px));
  const uint32_t ramp8[8] = {255, 0, 219, 182, 146, 109, 73, 36};   // ((7 - i) * 255 + 3) / 7
  bool ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && (px[n] >> 24) == ramp8[n % 8] && (px[n] & 0xFFFFFF) == 0;
  CHECK(ok);
  // six-value ramp: a0 = 0 <= a1 = 255: 0, 255, 51, 102, 153, 204, then 0 and 255
  b[0] = 0; b[1] = 255;
  CHECK(decode(Format::Bc3Unorm, b, 4, 4, &px));
  const uint32_t ramp6[8] = {0, 255, 51, 102, 153, 204, 0, 255};
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && (px[n] >> 24) == ramp6[n % 8];
  CHECK(ok);
}

void test_sizes_and_edges() {
  CHECK(graphics::compressed_block_bytes(Format::Bc1Unorm) == 8 && graphics::compressed_block_bytes(Format::Bc3Srgb) == 16 && graphics::compressed_block_bytes(Format::B8G8R8A8Unorm) == 0);
  CHECK(graphics::compressed_image_bytes(Format::Bc1Unorm, 5, 9) == 2 * 3 * 8 && graphics::compressed_image_bytes(Format::Bc2Unorm, 1, 1) == 16);
  CHECK(graphics::decoded_format(Format::Bc2Srgb) == Format::B8G8R8A8Srgb && graphics::decoded_format(Format::Bc1Unorm) == Format::B8G8R8A8Unorm);
  // a 6 x 5 image: 2 x 2 blocks, each of one colour; the pixels outside the image are not written
  std::vector<uint8_t> b;
  const uint16_t colours[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  for (uint16_t c : colours) colour_block(&b, c, c, 0);
  std::vector<uint32_t> px;
  CHECK(decode(Format::Bc1Unorm, b, 6, 5, &px));
  CHECK(px[0] == 0xFFFF0000u && px[5] == 0xFF00FF00u && px[4 * 6] == 0xFF0000FFu && px[4 * 6 + 5] == 0xFFFFFFFFu && px[3 * 6 + 3] == 0xFFFF0000u);
  // wrong sizes are refused and leave the output alone
  std::vector<uint8_t> shorter(b.begin(), b.end() - 1);
  CHECK(!decode(Format::Bc1Unorm, shorter, 6, 5, &px) && px[0] == 0xDEADBEEFu);
  for (size_t n = 0; n < b.size(); ++n) {
    std::vector<uint8_t> cut(b.begin(), b.begin() + n);
    CHECK(!decode(Format::Bc1Unorm, cut, 6, 5, &px));
  }
  std::vector<uint32_t> small(3);
  CHECK(!graphics::decode_blocks(Format::Bc1Unorm, ConstByteView(b.data(), b.size()), 6, 5, ArrayView<uint32_t>(small.data(), small.size())));
  CHECK(!decode(Format::B8G8R8A8Unorm, b, 6, 5, &px));
  CHECK(!decode(Format::Bc1Unorm, b, 0, 5, &px));
}

void test_bits() {
  // every block is a block: a turned bit or random bytes always decode, and only change the pixels of their own block
  uint32_t seed = 12345;
  auto next = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  const Format formats[3] = {Format::Bc1Unorm, Format::Bc2Unorm, Format::Bc3Srgb};
  int decoded = 0;
  for (Format f : formats) {
    std::vector<uint8_t> b(graphics::compressed_image_bytes(f, 8, 8));
    for (uint8_t& v : b) v = uint8_t(next());
    std::vector<uint32_t> base, px;
    CHECK(decode(f, b, 8, 8, &base));
    for (size_t bit = 0; bit < b.size() * 8; ++bit) {
      std::vector<uint8_t> flipped = b;
      flipped[bit / 8] ^= uint8_t(1u << (bit % 8));
      const bool ok = decode(f, flipped, 8, 8, &px);
      CHECK(ok);
      if (!ok) continue;
      ++decoded;
      const uint32_t block = uint32_t(bit / 8 / graphics::compressed_block_bytes(f)), bx = block % 2, by = block / 2;
      bool others_same = true;
      for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x)
          if (x / 4 != bx || y / 4 != by) others_same = others_same && px[y * 8 + x] == base[y * 8 + x];
      CHECK(others_same);
    }
  }
  printf("  %d images with a bit turned decoded\n", decoded);
}

// ---- BC4 .. BC7

bool decode_f(Format f, const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h, std::vector<graphics::Vec4>* out) {
  out->assign(uint64_t{w} * h, graphics::Vec4{-9, -9, -9, -9});
  return graphics::decode_blocks_float(f, ConstByteView(blocks.data(), blocks.size()), w, h, ArrayView<graphics::Vec4>(out->data(), out->size()));
}
bool near(float a, float b, float tolerance = 1e-6f) { return std::fabs(a - b) <= tolerance; }
uint16_t half_of(float f) { return graphics::float_to_half(f); }

// Writes the bits of a 16-byte block from bit 0 up.
struct BitWriter {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(16, 0);
  uint32_t pos = 0;
  void put(uint32_t value, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i, ++pos)
      if ((value >> i) & 1) bytes[pos / 8] |= uint8_t(1u << (pos % 8));
  }
};

// A BC4 channel block: endpoints and the 3-bit index n % 8 for pixel n.
void channel_block(std::vector<uint8_t>* b, uint8_t r0, uint8_t r1) {
  b->push_back(r0); b->push_back(r1);
  uint64_t bits = 0;
  for (int n = 0; n < 16; ++n) bits |= uint64_t(n % 8) << (3 * n);
  for (int i = 0; i < 6; ++i) b->push_back(uint8_t(bits >> (8 * i)));
}

void test_bc4_bc5() {
  std::vector<uint8_t> b;
  std::vector<uint32_t> px;
  std::vector<graphics::Vec4> fx;
  // UNORM, eight values: 255 and 0; in 8 bits rounded as BC3's alpha, in floats (7 - i) / 7
  channel_block(&b, 255, 0);
  CHECK(decode(Format::Bc4Unorm, b, 4, 4, &px));
  const uint32_t ramp8[8] = {255, 0, 219, 182, 146, 109, 73, 36};
  bool ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && px[n] == (0xFF000000u | (ramp8[n % 8] << 16));
  CHECK(ok);
  CHECK(decode_f(Format::Bc4Unorm, b, 4, 4, &fx));
  const float framp8[8] = {1, 0, 6 / 7.f, 5 / 7.f, 4 / 7.f, 3 / 7.f, 2 / 7.f, 1 / 7.f};
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && near(fx[n].x, framp8[n % 8]) && fx[n].y == 0 && fx[n].z == 0 && fx[n].w == 1;
  CHECK(ok);
  // six values: 0 <= 255: 0, 255, 51, 102, 153, 204, 0, 255
  b[0] = 0; b[1] = 255;
  CHECK(decode(Format::Bc4Unorm, b, 4, 4, &px) && decode_f(Format::Bc4Unorm, b, 4, 4, &fx));
  const uint32_t ramp6[8] = {0, 255, 51, 102, 153, 204, 0, 255};
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && px[n] == (0xFF000000u | (ramp6[n % 8] << 16)) && near(fx[n].x, ramp6[n % 8] / 255.f);
  CHECK(ok);
  // SNORM, eight values: 127 > -127: (7 - 2i) / 7
  b.clear();
  channel_block(&b, 0x7F, 0x81);
  CHECK(!decode(Format::Bc4Snorm, b, 4, 4, &px) && px[0] == 0xDEADBEEFu);   // (floats only)
  CHECK(decode_f(Format::Bc4Snorm, b, 4, 4, &fx));
  const float sramp8[8] = {1, -1, 5 / 7.f, 3 / 7.f, 1 / 7.f, -1 / 7.f, -3 / 7.f, -5 / 7.f};
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && near(fx[n].x, sramp8[n % 8]) && fx[n].w == 1;
  CHECK(ok);
  // six values: -128 (read as -127) <= 127: -1, 1, -3/5, -1/5, 1/5, 3/5, then -1 and 1
  b[0] = 0x80; b[1] = 0x7F;
  CHECK(decode_f(Format::Bc4Snorm, b, 4, 4, &fx));
  const float sramp6[8] = {-1, 1, -0.6f, -0.2f, 0.2f, 0.6f, -1, 1};
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && near(fx[n].x, sramp6[n % 8]);
  CHECK(ok);
  // BC5: red then green
  b.clear();
  channel_block(&b, 255, 0);
  channel_block(&b, 0, 255);
  CHECK(decode(Format::Bc5Unorm, b, 4, 4, &px) && decode_f(Format::Bc5Unorm, b, 4, 4, &fx));
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && px[n] == (0xFF000000u | (ramp8[n % 8] << 16) | (ramp6[n % 8] << 8)) && near(fx[n].y, ramp6[n % 8] / 255.f) && fx[n].z == 0;
  CHECK(ok);
  b.clear();
  channel_block(&b, 0x7F, 0x81);
  channel_block(&b, 0x80, 0x7F);
  CHECK(decode_f(Format::Bc5Snorm, b, 4, 4, &fx));
  ok = true;
  for (int n = 0; n < 16; ++n) ok = ok && near(fx[n].x, sramp8[n % 8]) && near(fx[n].y, sramp6[n % 8]) && fx[n].z == 0 && fx[n].w == 1;
  CHECK(ok);
}

void test_bc7() {
  std::vector<uint32_t> px;
  std::vector<graphics::Vec4> fx;
  // mode 6: one subset, 7-bit endpoints with a P-bit each, 4-bit indices (pixel n has index n)
  BitWriter w;
  w.put(0x40, 7);
  w.put(127, 7); w.put(0, 7);    // red
  w.put(0, 7); w.put(127, 7);    // green
  w.put(64, 7); w.put(64, 7);    // blue
  w.put(127, 7); w.put(127, 7);  // alpha
  w.put(1, 1); w.put(0, 1);      // P-bits
  w.put(0, 3);
  for (uint32_t n = 1; n < 16; ++n) w.put(n, 4);
  CHECK(w.pos == 128);
  CHECK(decode(Format::Bc7Unorm, w.bytes, 4, 4, &px));
  // endpoints (255, 1, 129, 255) and (0, 254, 128, 254); pixel 8 has weight 34: (120, 135, 128, 254)
  CHECK(px[0] == 0xFFFF0181u && px[15] == 0xFE00FE80u && px[8] == 0xFE788780u);
  CHECK(decode_f(Format::Bc7Srgb, w.bytes, 4, 4, &fx) && fx[0].x == 1.0f && fx[0].y == 1 / 255.0f && fx[8].w == 254 / 255.0f);
  // mode 1: two subsets (partition 0: columns 2 and 3 are subset 1, its anchor pixel 15), 6-bit endpoints with a shared P-bit per subset, 3-bit indices
  w = BitWriter();
  w.put(0x2, 2);
  w.put(0, 6);
  const uint32_t ends[3][4] = {{63, 0, 0, 0}, {0, 0, 63, 0}, {0, 0, 0, 63}};   // [channel][s0e0, s0e1, s1e0, s1e1]
  for (const auto& c : ends)
    for (uint32_t v : c) w.put(v, 6);
  w.put(1, 1); w.put(0, 1);
  w.put(0, 2);                         // pixel 0 (anchor)
  w.put(7, 3);                         // pixel 1: second endpoint of subset 0
  w.put(0, 3);                         // pixel 2: first of subset 1
  w.put(7, 3);                         // pixel 3: second of subset 1
  for (uint32_t n = 4; n < 15; ++n) w.put(0, 3);
  w.put(0, 2);                         // pixel 15 (anchor of subset 1)
  CHECK(w.pos == 128);
  CHECK(decode(Format::Bc7Unorm, w.bytes, 4, 4, &px));
  // subset 0 with P-bit 1: (255, 2, 2) and (2, 2, 2); subset 1 with P-bit 0: (0, 253, 0) and (0, 0, 253)
  CHECK(px[0] == 0xFFFF0202u && px[1] == 0xFF020202u && px[2] == 0xFF00FD00u && px[3] == 0xFF0000FDu && px[4] == 0xFFFF0202u);
  // mode 4: rotation 1 (alpha and red swap), 5-bit colour, 6-bit alpha, 2-bit and 3-bit indices; first with the index selection bit 0, then 1
  for (uint32_t selection = 0; selection < 2; ++selection) {
    w = BitWriter();
    w.put(0x10, 5);
    w.put(1, 2);
    w.put(selection, 1);
    w.put(31, 5); w.put(0, 5); w.put(0, 5); w.put(0, 5); w.put(0, 5); w.put(0, 5);
    w.put(0, 6); w.put(63, 6);
    w.put(0, 1);
    for (uint32_t n = 1; n < 16; ++n) w.put(0, 2);
    w.put(3, 2);
    for (uint32_t n = 1; n < 16; ++n) w.put(7, 3);
    CHECK(w.pos == 128);
    CHECK(decode(Format::Bc7Unorm, w.bytes, 4, 4, &px));
    if (selection == 0) {
      // colour index 0: red 255; alpha index 3 of 3 bits for pixel 0 (weight 27): 108, 255 elsewhere; then swapped
      CHECK(px[0] == 0xFF6C0000u && px[1] == 0xFFFF0000u);
    } else {
      // colour from the 3-bit indices (pixel 0 weight 27: red 147; the others red 0), alpha from the 2-bit ones (0); then swapped
      CHECK(px[0] == 0x93000000u && px[1] == 0x00000000u);
    }
  }
  // mode 5: no rotation, 7-bit colour, 8-bit alpha, two sets of 2-bit indices
  w = BitWriter();
  w.put(0x20, 6);
  w.put(0, 2);
  w.put(127, 7); w.put(0, 7); w.put(0, 7); w.put(127, 7); w.put(0, 7); w.put(0, 7);
  w.put(255, 8); w.put(0, 8);
  w.put(1, 1);
  for (uint32_t n = 1; n < 16; ++n) w.put(1, 2);
  w.put(1, 1);
  for (uint32_t n = 1; n < 16; ++n) w.put(2, 2);
  CHECK(w.pos == 128);
  CHECK(decode(Format::Bc7Unorm, w.bytes, 4, 4, &px));
  // weight 21: red (43 * 255 + 32) >> 6 = 171, green 84; alpha 171 for pixel 0 (index 1), 84 for the others (index 2, weight 43)
  CHECK(px[0] == 0xABAB5400u && px[5] == 0x54AB5400u);
  // no mode bit: transparent black
  std::vector<uint8_t> zero(16, 0xFF);
  zero[0] = 0;
  CHECK(decode(Format::Bc7Unorm, zero, 4, 4, &px) && px[0] == 0 && px[15] == 0);
  CHECK(decode_f(Format::Bc7Unorm, zero, 4, 4, &fx) && fx[3].x == 0 && fx[3].w == 0);
}

void test_bc6h() {
  std::vector<graphics::Vec4> fx;
  std::vector<uint32_t> px;
  // mode 11 (00011): one region, 10-bit endpoints (1023, 0, 512) and (0, 1023, 512), 4-bit indices: pixel 0 index 0, pixel 15 index 15
  BitWriter w;
  w.put(0x03, 5);
  w.put(1023, 10); w.put(0, 10); w.put(512, 10);
  w.put(0, 10); w.put(1023, 10); w.put(512, 10);
  w.put(0, 3);
  for (uint32_t n = 1; n < 16; ++n) w.put(n == 15 ? 15 : 0, 4);
  CHECK(w.pos == 128);
  CHECK(!decode(Format::Bc6hUfloat, w.bytes, 4, 4, &px));
  CHECK(decode_f(Format::Bc6hUfloat, w.bytes, 4, 4, &fx));
  // 1023 -> 0xFFFF -> * 31 / 64 = 0x7BFF; 512 -> ((512 << 16) + 0x8000) >> 10 = 32800 -> 15887 = 0x3E0F
  CHECK(half_of(fx[0].x) == 0x7BFF && half_of(fx[0].y) == 0 && half_of(fx[0].z) == 0x3E0F && fx[0].w == 1.0f);
  CHECK(half_of(fx[15].x) == 0 && half_of(fx[15].y) == 0x7BFF && half_of(fx[15].z) == 0x3E0F);
  // signed: 1023 is -1 -> -96 -> -93 (0x805D); 512 is -512 -> -0x7FFF -> 0xFBFF
  CHECK(decode_f(Format::Bc6hSfloat, w.bytes, 4, 4, &fx));
  CHECK(half_of(fx[0].x) == 0x805D && half_of(fx[0].y) == 0 && half_of(fx[0].z) == 0xFBFF);
  // mode 1 (00): two regions, partition 13 (rows 2 and 3 region 1, anchor 15), red w = 512 and deltas x = -1, y = -16, z = +15 (5 bits); green and blue 0
  w = BitWriter();
  w.put(0, 2); w.put(0, 3);
  w.put(512, 10); w.put(0, 10); w.put(0, 10);
  w.put(0x1F, 5);                            // rx
  w.put(0, 1 + 4 + 5 + 1 + 4 + 5 + 1 + 4);   // gz4, gy, gx, bz0, gz, bx, bz1, by
  w.put(0x10, 5);                            // ry
  w.put(0, 1);
  w.put(0x0F, 5);                            // rz
  w.put(0, 1);
  w.put(13, 5);
  CHECK(w.pos == 82);
  w.put(0, 2);                               // pixel 0
  w.put(7, 3);                               // pixel 1
  for (uint32_t n = 2; n < 15; ++n) w.put(0, 3);
  w.put(3, 2);                               // pixel 15 (anchor)
  CHECK(w.pos == 128);
  CHECK(decode_f(Format::Bc6hUfloat, w.bytes, 4, 4, &fx));
  // endpoints 512, 511, 496, 527 -> 32800, 32736, 31776, 33760; pixel 15 weight 27: 32613 -> 15796
  CHECK(half_of(fx[0].x) == 0x3E0F && half_of(fx[1].x) == 0x3DF0 && half_of(fx[8].x) == 0x3C1F && half_of(fx[15].x) == 0x3DB4 && half_of(fx[15].y) == 0 && half_of(fx[15].z) == 0);
  // reserved modes: 0
  for (uint8_t mode : {0x13, 0x17, 0x1B, 0x1F}) {
    std::vector<uint8_t> b(16, 0xA5);
    b[0] = uint8_t((0xA5 & ~31) | mode);
    CHECK(decode_f(Format::Bc6hSfloat, b, 4, 4, &fx) && fx[0].x == 0 && fx[7].y == 0 && fx[15].z == 0 && fx[15].w == 1);
    CHECK(decode_f(Format::Bc6hUfloat, b, 4, 4, &fx) && fx[0].x == 0 && fx[15].z == 0);
  }
  // mode 14 (01111) with the first endpoint's red 0x8000 (bit 15 is the first of the reversed high bits): unsigned 32768 -> 0x3E00; signed -32768 is taken as -32767
  w = BitWriter();
  w.put(0x0F, 5);
  w.put(0, 30);
  w.put(0, 4);
  w.put(1, 1);
  CHECK(decode_f(Format::Bc6hUfloat, w.bytes, 4, 4, &fx) && half_of(fx[0].x) == 0x3E00 && half_of(fx[5].x) == 0x3E00);
  CHECK(decode_f(Format::Bc6hSfloat, w.bytes, 4, 4, &fx) && half_of(fx[0].x) == 0xFBFF && std::isfinite(fx[0].x));
}

const Format kNewFormats[8] = {Format::Bc4Unorm, Format::Bc4Snorm, Format::Bc5Unorm, Format::Bc5Snorm, Format::Bc6hUfloat, Format::Bc6hSfloat, Format::Bc7Unorm, Format::Bc7Srgb};
const Format kAllFormats[14] = {Format::Bc1Unorm, Format::Bc2Unorm, Format::Bc3Unorm, Format::Bc1Srgb, Format::Bc2Srgb, Format::Bc3Srgb, Format::Bc4Unorm,
                                Format::Bc4Snorm, Format::Bc5Unorm, Format::Bc5Snorm, Format::Bc6hUfloat, Format::Bc6hSfloat, Format::Bc7Unorm, Format::Bc7Srgb};

// The decoded values are what the format can hold: finite, UNORM 0 .. 1, SNORM -1 .. 1, BC6H halves within +-65504 (unsigned never negative) and alpha 1.
bool in_range(Format f, const std::vector<graphics::Vec4>& fx) {
  for (const graphics::Vec4& v : fx) {
    const float c[4] = {v.x, v.y, v.z, v.w};
    for (float x : c)
      if (!std::isfinite(x)) return false;
    if (f == Format::Bc6hUfloat || f == Format::Bc6hSfloat) {
      for (int i = 0; i < 3; ++i) {
        const uint16_t h = half_of(c[i]);
        if ((h & 0x7FFF) > 0x7BFF || (f == Format::Bc6hUfloat && (h & 0x8000))) return false;
      }
      if (v.w != 1) return false;
    } else {
      const float lo = f == Format::Bc4Snorm || f == Format::Bc5Snorm ? -1 : 0;
      for (float x : c)
        if (x < lo || x > 1) return false;
    }
  }
  return true;
}

void test_new_sizes() {
  for (Format f : kAllFormats) CHECK(graphics::is_compressed_format(f));
  CHECK(!graphics::is_compressed_format(Format::R8Unorm) && !graphics::is_compressed_format(Format::R16G16B16A16Float));
  CHECK(graphics::compressed_block_bytes(Format::Bc4Unorm) == 8 && graphics::compressed_block_bytes(Format::Bc4Snorm) == 8 && graphics::compressed_block_bytes(Format::Bc5Snorm) == 16 &&
        graphics::compressed_block_bytes(Format::Bc6hUfloat) == 16 && graphics::compressed_block_bytes(Format::Bc7Srgb) == 16 && graphics::compressed_block_bytes(Format::R32Float) == 0);
  CHECK(graphics::compressed_image_bytes(Format::Bc4Unorm, 5, 3) == 16 && graphics::compressed_image_bytes(Format::Bc7Unorm, 5, 3) == 32);
  CHECK(graphics::compressed_row_bytes(Format::Bc7Unorm, 0xFFFFFFFFu) == uint64_t{0x40000000} * 16);   // (no 32-bit wrap)
  CHECK(graphics::decoded_format(Format::Bc4Unorm) == Format::B8G8R8A8Unorm && graphics::decoded_format(Format::Bc5Unorm) == Format::B8G8R8A8Unorm &&
        graphics::decoded_format(Format::Bc7Unorm) == Format::B8G8R8A8Unorm && graphics::decoded_format(Format::Bc7Srgb) == Format::B8G8R8A8Srgb &&
        graphics::decoded_format(Format::Bc3Srgb) == Format::B8G8R8A8Srgb && graphics::decoded_format(Format::Bc4Snorm) == Format::R32G32B32A32Float &&
        graphics::decoded_format(Format::Bc5Snorm) == Format::R32G32B32A32Float && graphics::decoded_format(Format::Bc6hUfloat) == Format::R16G16B16A16Float &&
        graphics::decoded_format(Format::Bc6hSfloat) == Format::R16G16B16A16Float);
  for (Format f : kAllFormats) CHECK(graphics::decodes_to_floats(f) == graphics::is_float_format(graphics::decoded_format(f)));
  CHECK(!graphics::decodes_to_floats(Format::R32G32B32A32Float));
}

uint32_t g_seed = 987654321;
uint32_t next_random() { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return g_seed; }

void test_new_truncation_and_edges() {
  for (Format f : kAllFormats) {
    const bool floats = graphics::decodes_to_floats(f);
    std::vector<uint8_t> b(graphics::compressed_image_bytes(f, 8, 8));
    for (uint8_t& v : b) v = uint8_t(next_random());
    std::vector<uint32_t> px;
    std::vector<graphics::Vec4> fx;
    CHECK(decode_f(f, b, 8, 8, &fx));
    CHECK(decode(f, b, 8, 8, &px) == !floats);
    bool refused = true;
    for (size_t n = 0; n < b.size(); ++n) {
      std::vector<uint8_t> cut(b.begin(), b.begin() + n);
      refused = refused && !decode_f(f, cut, 8, 8, &fx) && fx[0].x == -9 && fx[63].w == -9 && !decode(f, cut, 8, 8, &px) && px[0] == 0xDEADBEEFu && px[63] == 0xDEADBEEFu;
    }
    std::vector<uint8_t> longer = b;
    longer.push_back(0);
    refused = refused && !decode_f(f, longer, 8, 8, &fx) && !decode(f, longer, 8, 8, &px);
    std::vector<graphics::Vec4> small(63);
    refused = refused && !graphics::decode_blocks_float(f, ConstByteView(b.data(), b.size()), 8, 8, ArrayView<graphics::Vec4>(small.data(), small.size()));
    refused = refused && !decode_f(f, b, 0, 8, &fx) && !decode_f(f, b, 8, 0, &fx);
    CHECK(refused);
    // a 5 x 3 image (2 x 1 blocks) is the top-left of the 8 x 4 image of the same blocks
    std::vector<uint8_t> two(graphics::compressed_image_bytes(f, 5, 3));
    for (uint8_t& v : two) v = uint8_t(next_random());
    std::vector<graphics::Vec4> full, cut;
    CHECK(decode_f(f, two, 8, 4, &full) && decode_f(f, two, 5, 3, &cut));
    bool same = true;
    for (uint32_t y = 0; y < 3; ++y)
      for (uint32_t x = 0; x < 5; ++x) same = same && std::memcmp(&cut[y * 5 + x], &full[y * 8 + x], sizeof(graphics::Vec4)) == 0;
    CHECK(same);
    if (!floats) {
      std::vector<uint32_t> full8, cut8;
      CHECK(decode(f, two, 8, 4, &full8) && decode(f, two, 5, 3, &cut8));
      same = true;
      for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 5; ++x) same = same && cut8[y * 5 + x] == full8[y * 8 + x];
      CHECK(same);
    }
  }
  std::vector<graphics::Vec4> fx;
  CHECK(!decode_f(Format::R8Unorm, std::vector<uint8_t>(16), 4, 4, &fx) && !decode_f(Format::R32G32B32A32Float, std::vector<uint8_t>(16), 4, 4, &fx) && fx[0].x == -9);
}

void test_new_bits() {
  int flips = 0, randoms = 0;
  for (Format f : kNewFormats) {
    const uint32_t size = graphics::compressed_block_bytes(f);
    const bool floats = graphics::decodes_to_floats(f);
    const bool bc7 = f == Format::Bc7Unorm || f == Format::Bc7Srgb;
    for (uint32_t trial = 0; trial < (bc7 ? 8u : (f == Format::Bc6hUfloat || f == Format::Bc6hSfloat) ? 14u : 2u); ++trial) {
      std::vector<uint8_t> b(size);
      for (uint8_t& v : b) v = uint8_t(next_random());
      if (bc7) b[0] = uint8_t((b[0] & ~((2u << trial) - 1)) | (1u << trial));   // (each mode)
      if (f == Format::Bc6hUfloat || f == Format::Bc6hSfloat) {
        const uint8_t modes[14] = {0x00, 0x01, 0x02, 0x06, 0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x03, 0x07, 0x0B, 0x0F};
        b[0] = uint8_t((b[0] & (trial < 2 ? ~3u : ~31u)) | modes[trial]);
      }
      std::vector<graphics::Vec4> fx;
      std::vector<uint32_t> px8;
      bool ok = true;
      for (uint32_t bit = 0; bit < size * 8; ++bit) {
        std::vector<uint8_t> flipped = b;
        flipped[bit / 8] ^= uint8_t(1u << (bit % 8));
        ok = ok && decode_f(f, flipped, 4, 4, &fx) && in_range(f, fx);
        if (!floats) {
          // the 8-bit pixels and the floats agree
          ok = ok && decode(f, flipped, 4, 4, &px8);
          for (int n = 0; n < 16 && ok; ++n)
            ok = (bc7 ? fx[n].x == ((px8[n] >> 16) & 255) / 255.0f && fx[n].w == (px8[n] >> 24) / 255.0f : near(fx[n].x, ((px8[n] >> 16) & 255) / 255.0f, 0.5f / 255 + 1e-6f)) &&
                 near(fx[n].y, ((px8[n] >> 8) & 255) / 255.0f, 0.5f / 255 + 1e-6f);
        }
        ++flips;
      }
      CHECK(ok);
    }
    // many random blocks, with every BC6H and every BC7 mode
    std::vector<uint8_t> many(graphics::compressed_image_bytes(f, 64, 64));
    for (uint8_t& v : many) v = uint8_t(next_random());
    for (uint32_t i = 0; i < 256; ++i) {
      uint8_t& first = many[i * size];
      if (bc7) first = i % 9 == 8 ? 0 : uint8_t((first & ~((2u << (i % 9)) - 1)) | (1u << (i % 9)));
      if (f == Format::Bc6hUfloat || f == Format::Bc6hSfloat) first = uint8_t((first & ~31u) | (i % 32));
    }
    std::vector<graphics::Vec4> fx;
    std::vector<uint32_t> px;
    CHECK(decode_f(f, many, 64, 64, &fx) && in_range(f, fx));
    CHECK(decode(f, many, 64, 64, &px) == !floats);
    randoms += 256;
  }
  printf("  %d blocks of BC4 .. BC7 with a bit turned and %d random blocks decoded\n", flips, randoms);
}

// ---- Mesa's decoder (llvmpipe; scripts/bc-reference.c wrote tests/data/bc/reference.bin)

uint32_t get32(const std::vector<uint8_t>& d, size_t at) { return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (uint32_t(d[at + 3]) << 24); }

void test_reference() {
  FILE* file = fopen("tests/data/bc/reference.bin", "rb");
  CHECK(file != nullptr);
  if (!file) return;
  std::vector<uint8_t> data;
  uint8_t chunk[4096];
  for (size_t n; (n = fread(chunk, 1, sizeof chunk, file)) > 0;) data.insert(data.end(), chunk, chunk + n);
  fclose(file);
  const bool header = data.size() > 8 && std::memcmp(data.data(), "TCBC", 4) == 0;
  CHECK(header);
  if (!header) return;
  const uint32_t count = get32(data, 4);
  size_t at = 8;
  uint32_t formats_seen = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 20 > data.size()) { CHECK(false); return; }
    const uint32_t gl = get32(data, at), w = get32(data, at + 4), h = get32(data, at + 8), type = get32(data, at + 12), size = get32(data, at + 16);
    at += 20;
    if (at + size + 4 > data.size()) { CHECK(false); return; }
    const std::vector<uint8_t> blocks(data.begin() + at, data.begin() + at + size);
    at += size;
    const uint32_t pixel_size = get32(data, at);
    at += 4;
    if (at + pixel_size > data.size()) { CHECK(false); return; }
    const std::vector<uint8_t> ref(data.begin() + at, data.begin() + at + pixel_size);
    at += pixel_size;
    Format f;
    switch (gl) {
      case 0x8DBB: f = Format::Bc4Unorm; break;
      case 0x8DBC: f = Format::Bc4Snorm; break;
      case 0x8DBD: f = Format::Bc5Unorm; break;
      case 0x8DBE: f = Format::Bc5Snorm; break;
      case 0x8E8C: f = Format::Bc7Unorm; break;
      case 0x8E8D: f = Format::Bc7Srgb; break;
      case 0x8E8E: f = Format::Bc6hSfloat; break;
      case 0x8E8F: f = Format::Bc6hUfloat; break;
      default: CHECK(false); continue;
    }
    const uint64_t pixels = uint64_t{w} * h;
    const bool sizes = pixel_size == pixels * (type == 0x140B ? 8 : 4) && w % 4 == 0;
    CHECK(sizes);
    if (!sizes) continue;
    std::vector<graphics::Vec4> fx;
    std::vector<uint32_t> px;
    CHECK(decode_f(f, blocks, w, h, &fx));
    if (!graphics::decodes_to_floats(f)) CHECK(decode(f, blocks, w, h, &px));
    uint32_t mismatches = 0;
    for (uint64_t n = 0; n < pixels; ++n) {
      const float got[4] = {fx[n].x, fx[n].y, fx[n].z, fx[n].w};
      bool ok = true;
      if (type == 0x140B) {   // BC6H: the half bits exactly
        for (int c = 0; c < 4; ++c) ok = ok && half_of(got[c]) == (ref[n * 8 + 2 * c] | (ref[n * 8 + 2 * c + 1] << 8));
      } else if (type == 0x1400) {
        // RGTC signed: Mesa's bytes / 127, within 1 / 127 (Mesa truncates its 8-bit interpolation). Mesa interpolates an endpoint of -128 as it is and clamps the result;
        // the Direct3D specification reads it as -127 first (as this decoder does), which moves the interpolated values by up to one more step.
        const uint64_t block = (n / w / 4) * (w / 4) + (n % w) / 4, bs = graphics::compressed_block_bytes(f);
        for (int c = 0; c < 4; ++c) {
          const int8_t r = int8_t(ref[n * 4 + c]);
          const bool minus128 = c < 2 && uint64_t(c) * 8 < bs && (blocks[block * bs + c * 8] == 0x80 || blocks[block * bs + c * 8 + 1] == 0x80);
          ok = ok && near(got[c], r < -127 ? -1.0f : r / 127.0f, (minus128 ? 2 : 1) / 127.0f + 1e-6f);
        }
      } else if (f == Format::Bc7Unorm || f == Format::Bc7Srgb) {   // the bytes exactly
        const uint32_t want = (uint32_t(ref[n * 4 + 3]) << 24) | (ref[n * 4] << 16) | (ref[n * 4 + 1] << 8) | ref[n * 4 + 2];
        ok = px[n] == want;
        for (int c = 0; c < 4; ++c) ok = ok && got[c] == ref[n * 4 + c] / 255.0f;
      } else {   // RGTC unorm: within 1 / 255 (Mesa truncates its 8-bit interpolation)
        for (int c = 0; c < 4; ++c) ok = ok && near(got[c], ref[n * 4 + c] / 255.0f, 1 / 255.0f + 1e-6f);
        const uint32_t mine[4] = {(px[n] >> 16) & 255, (px[n] >> 8) & 255, px[n] & 255, px[n] >> 24};
        for (int c = 0; c < 4; ++c) ok = ok && (mine[c] > ref[n * 4 + c] ? mine[c] - ref[n * 4 + c] : ref[n * 4 + c] - mine[c]) <= 1;
      }
      if (!ok) {
        if (mismatches < 8) {
          const uint64_t block = (n / w / 4) * (w / 4) + (n % w) / 4;
          const uint32_t bs = graphics::compressed_block_bytes(f);
          printf("  format 0x%X pixel %llu (block %llu:", gl, (unsigned long long)n, (unsigned long long)block);
          for (uint32_t k = 0; k < bs; ++k) printf(" %02X", blocks[block * bs + k]);
          printf("): got %g %g %g %g, Mesa", got[0], got[1], got[2], got[3]);
          for (int c = 0; c < 4; ++c) {
            if (type == 0x140B) printf(" %04X", ref[n * 8 + 2 * c] | (ref[n * 8 + 2 * c + 1] << 8));
            else printf(" %d", type == 0x1400 ? int(int8_t(ref[n * 4 + c])) : int(ref[n * 4 + c]));
          }
          printf("\n");
        }
        ++mismatches;
      }
    }
    if (mismatches) printf("  format 0x%X: %u of %llu pixels differ from Mesa\n", gl, mismatches, (unsigned long long)pixels);
    CHECK(mismatches == 0);
    ++formats_seen;
  }
  CHECK(formats_seen == 8);
  printf("  %u formats compared with Mesa's decoder\n", formats_seen);
}
}  // namespace

int main() {
  test_bc1();
  test_bc2_bc3();
  test_sizes_and_edges();
  test_bits();
  test_bc4_bc5();
  test_bc7();
  test_bc6h();
  test_new_sizes();
  test_new_truncation_and_edges();
  test_new_bits();
  test_reference();
  if (g_failed) { printf("BlockCompressionTest: %d of %d checks FAILED\n", g_failed, g_checks); return 1; }
  printf("BlockCompressionTest: all %d checks passed\n", g_checks);
  return 0;
}
