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

// Block-compressed textures (S3TC/DXT, RGTC, ETC1): the blocks are decoded when they are uploaded, into an ordinary image.
#include "WebGl/GlCompressed.hpp"

namespace webgl {

namespace {

const CompressedFormat kFormats[] = {
    {0x83F0, 8, Codec::Bc1Rgb, 0x8058}, {0x83F1, 8, Codec::Bc1Rgba, 0x8058}, {0x83F2, 16, Codec::Bc2, 0x8058}, {0x83F3, 16, Codec::Bc3, 0x8058},
    {0x8C4C, 8, Codec::Bc1Rgb, 0x8C43}, {0x8C4D, 8, Codec::Bc1Rgba, 0x8C43}, {0x8C4E, 16, Codec::Bc2, 0x8C43}, {0x8C4F, 16, Codec::Bc3, 0x8C43},
    {0x8DBB, 8, Codec::Bc4U, 0x8229}, {0x8DBC, 8, Codec::Bc4S, 0x8F94}, {0x8DBD, 16, Codec::Bc5U, 0x822B}, {0x8DBE, 16, Codec::Bc5S, 0x8F95},
    {0x8D64, 8, Codec::Etc1, 0x8058},
};

inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

struct Rgba { uint8_t c[4]; };

void rgb565(uint32_t v, uint8_t out[3]) {
  const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
  out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
  out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

// The 16 pixels of a BC1 colour block (alpha 255 except for the transparent colour of the 3-colour mode when punch-through is on).
void bc1(const uint8_t* p, bool four_colour_only, bool punch, Rgba out[16]) {
  const uint32_t c0 = p[0] | (p[1] << 8), c1 = p[2] | (p[3] << 8);
  uint8_t pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
  if (c0 > c1 || four_colour_only) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
    if (punch) pal[3][3] = 0;
  }
  const uint32_t bits = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<uint32_t>(p[7]) << 24);
  for (int i = 0; i < 16; ++i)
    for (int k = 0; k < 4; ++k) out[i].c[k] = pal[(bits >> (2 * i)) & 3][k];
}

// BC4: eight values and a 3-bit index per pixel; the unsigned form works in 0..255, the signed one in -127..127 (as floats, -1..1).
void bc4(const uint8_t* p, bool is_signed, float out[16]) {
  float a[8];
  if (!is_signed) {
    const int a0 = p[0], a1 = p[1];
    a[0] = static_cast<float>(a0);
    a[1] = static_cast<float>(a1);
    if (a0 > a1) for (int i = 2; i < 8; ++i) a[i] = static_cast<float>(((8 - i) * a0 + (i - 1) * a1) / 7);
    else { for (int i = 2; i < 6; ++i) a[i] = static_cast<float>(((6 - i) * a0 + (i - 1) * a1) / 5); a[6] = 0; a[7] = 255; }
  } else {
    int a0 = static_cast<int8_t>(p[0]), a1 = static_cast<int8_t>(p[1]);
    if (a0 < -127) a0 = -127;
    if (a1 < -127) a1 = -127;
    a[0] = static_cast<float>(a0) / 127.0f;
    a[1] = static_cast<float>(a1) / 127.0f;
    if (a0 > a1) for (int i = 2; i < 8; ++i) a[i] = (static_cast<float>(8 - i) * a0 + static_cast<float>(i - 1) * a1) / 7.0f / 127.0f;
    else { for (int i = 2; i < 6; ++i) a[i] = (static_cast<float>(6 - i) * a0 + static_cast<float>(i - 1) * a1) / 5.0f / 127.0f; a[6] = -1; a[7] = 1; }
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t{p[2 + i]} << (8 * i);
  for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

const int kEtcTable[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42}, {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};

void etc1(const uint8_t* p, Rgba out[16]) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  const bool diff = (v >> 33) & 1, flip = (v >> 32) & 1;
  int base[2][3];
  if (diff) {
    for (int k = 0; k < 3; ++k) {
      const int hi = static_cast<int>((v >> (59 - 8 * k)) & 31);
      int d = static_cast<int>((v >> (56 - 8 * k)) & 7);
      if (d >= 4) d -= 8;
      int lo = hi + d;
      if (lo < 0) lo = 0;
      if (lo > 31) lo = 31;
      base[0][k] = (hi << 3) | (hi >> 2);
      base[1][k] = (lo << 3) | (lo >> 2);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      const int a = static_cast<int>((v >> (60 - 8 * k)) & 15), b = static_cast<int>((v >> (56 - 8 * k)) & 15);
      base[0][k] = a * 17;
      base[1][k] = b * 17;
    }
  }
  const int cw[2] = {static_cast<int>((v >> 37) & 7), static_cast<int>((v >> 34) & 7)};
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      const int pix = x * 4 + y;
      const int sub = flip ? (y >= 2) : (x >= 2);
      const int sel = static_cast<int>((((v >> (16 + pix)) & 1) << 1) | ((v >> pix) & 1));
      const int mod = kEtcTable[cw[sub]][sel];
      Rgba& o = out[y * 4 + x];
      for (int k = 0; k < 3; ++k) o.c[k] = clamp8(base[sub][k] + mod);
      o.c[3] = 255;
    }
}

}  // namespace

const CompressedFormat* find_compressed(uint32_t internal) {
  for (const CompressedFormat& f : kFormats)
    if (f.internal == internal) return &f;
  return nullptr;
}

uint64_t compressed_size(const CompressedFormat& f, uint32_t w, uint32_t h, uint32_t d) {
  return uint64_t{(w + 3) / 4} * ((h + 3) / 4) * d * f.block_bytes;
}

bool decode_compressed(const CompressedFormat& f, ConstByteView data, uint32_t w, uint32_t h, uint32_t d, Image* dst, uint32_t xo, uint32_t yo, uint32_t zo) {
  if (data.size() < compressed_size(f, w, h, d)) return false;
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  const bool is_float = dst->info->store == Store::F32x4;
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint8_t* p = data.data() + ((uint64_t{z} * bh + by) * bw + bx) * f.block_bytes;
        Rgba px[16] = {};
        float chan[2][16] = {};
        switch (f.codec) {
          case Codec::Bc1Rgb: bc1(p, false, false, px); break;
          case Codec::Bc1Rgba: bc1(p, false, true, px); break;
          case Codec::Bc2:
            bc1(p + 8, true, false, px);
            for (int i = 0; i < 16; ++i) { const int a = (p[i / 2] >> (4 * (i & 1))) & 15; px[i].c[3] = static_cast<uint8_t>(a * 17); }
            break;
          case Codec::Bc3: {
            float alpha[16];
            bc4(p, false, alpha);
            bc1(p + 8, true, false, px);
            for (int i = 0; i < 16; ++i) px[i].c[3] = clamp8(static_cast<int>(alpha[i] + 0.5f));
            break;
          }
          case Codec::Bc4U: case Codec::Bc4S: bc4(p, f.codec == Codec::Bc4S, chan[0]); break;
          case Codec::Bc5U: case Codec::Bc5S: bc4(p, f.codec == Codec::Bc5S, chan[0]); bc4(p + 8, f.codec == Codec::Bc5S, chan[1]); break;
          case Codec::Etc1: etc1(p, px); break;
        }
        const bool two = f.codec == Codec::Bc5U || f.codec == Codec::Bc5S, one = f.codec == Codec::Bc4U || f.codec == Codec::Bc4S;
        for (int i = 0; i < 16; ++i) {
          const uint32_t x = xo + bx * 4 + static_cast<uint32_t>(i % 4), y = yo + by * 4 + static_cast<uint32_t>(i / 4);
          if (x >= dst->w || y >= dst->h || bx * 4 + static_cast<uint32_t>(i % 4) >= w || by * 4 + static_cast<uint32_t>(i / 4) >= h) continue;
          uint32_t lanes[4];
          if (one || two) {
            if (is_float) { lanes[0] = f2u(chan[0][i]); lanes[1] = two ? f2u(chan[1][i]) : 0; lanes[2] = 0; lanes[3] = f2u(1.0f); }
            else { lanes[0] = clamp8(static_cast<int>(chan[0][i] + 0.5f)); lanes[1] = two ? clamp8(static_cast<int>(chan[1][i] + 0.5f)) : 0; lanes[2] = 0; lanes[3] = 255; }
          } else {
            for (int k = 0; k < 4; ++k) lanes[k] = px[i].c[k];
          }
          write_texel(*dst, x, y, zo + z, lanes);
        }
      }
  return true;
}

}  // namespace webgl
