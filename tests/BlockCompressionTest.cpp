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

// Graphics/BlockCompression.hpp: BC1 .. BC3 blocks with values worked out by hand (both BC1 modes, BC2's alphas, both BC3 ramps), images whose edges cut blocks, the sizes,
// views of the wrong size (shortened), and blocks with a bit turned or of random bytes, which always decode (every bit pattern is a block).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
}  // namespace

int main() {
  test_bc1();
  test_bc2_bc3();
  test_sizes_and_edges();
  test_bits();
  if (g_failed) { printf("BlockCompressionTest: %d of %d checks FAILED\n", g_failed, g_checks); return 1; }
  printf("BlockCompressionTest: all %d checks passed\n", g_checks);
  return 0;
}
