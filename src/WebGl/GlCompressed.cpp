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
    {0x8E8C, 16, Codec::Bc7, 0x8058}, {0x8E8D, 16, Codec::Bc7, 0x8C43}, {0x8E8E, 16, Codec::Bc6S, 0x8814}, {0x8E8F, 16, Codec::Bc6U, 0x8814},
    // ASTC (LDR profile): 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12, linear then sRGB
    {0x93B0, 16, Codec::Astc, 0x8058, 4, 4}, {0x93B1, 16, Codec::Astc, 0x8058, 5, 4}, {0x93B2, 16, Codec::Astc, 0x8058, 5, 5}, {0x93B3, 16, Codec::Astc, 0x8058, 6, 5},
    {0x93B4, 16, Codec::Astc, 0x8058, 6, 6}, {0x93B5, 16, Codec::Astc, 0x8058, 8, 5}, {0x93B6, 16, Codec::Astc, 0x8058, 8, 6}, {0x93B7, 16, Codec::Astc, 0x8058, 8, 8},
    {0x93B8, 16, Codec::Astc, 0x8058, 10, 5}, {0x93B9, 16, Codec::Astc, 0x8058, 10, 6}, {0x93BA, 16, Codec::Astc, 0x8058, 10, 8}, {0x93BB, 16, Codec::Astc, 0x8058, 10, 10},
    {0x93BC, 16, Codec::Astc, 0x8058, 12, 10}, {0x93BD, 16, Codec::Astc, 0x8058, 12, 12},
    {0x93D0, 16, Codec::Astc, 0x8C43, 4, 4, true}, {0x93D1, 16, Codec::Astc, 0x8C43, 5, 4, true}, {0x93D2, 16, Codec::Astc, 0x8C43, 5, 5, true}, {0x93D3, 16, Codec::Astc, 0x8C43, 6, 5, true},
    {0x93D4, 16, Codec::Astc, 0x8C43, 6, 6, true}, {0x93D5, 16, Codec::Astc, 0x8C43, 8, 5, true}, {0x93D6, 16, Codec::Astc, 0x8C43, 8, 6, true}, {0x93D7, 16, Codec::Astc, 0x8C43, 8, 8, true},
    {0x93D8, 16, Codec::Astc, 0x8C43, 10, 5, true}, {0x93D9, 16, Codec::Astc, 0x8C43, 10, 6, true}, {0x93DA, 16, Codec::Astc, 0x8C43, 10, 8, true}, {0x93DB, 16, Codec::Astc, 0x8C43, 10, 10, true},
    {0x93DC, 16, Codec::Astc, 0x8C43, 12, 10, true}, {0x93DD, 16, Codec::Astc, 0x8C43, 12, 12, true},
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


// --- BC6H and BC7 -------------------------------------------------------------------------------------------------------------------------------------

#include "WebGl/GlBptcTables.inc"

const uint8_t kBptcFactors[3][16] = {
    {0, 21, 43, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 9, 18, 27, 37, 46, 55, 64, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64},
};

// Reads bit fields (least significant bit first) from a 16-byte block.
struct Bits {
  const uint8_t* d;
  uint32_t pos = 0;
  uint32_t read(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i, ++pos) v |= static_cast<uint32_t>(pos < 128 ? (d[pos >> 3] >> (pos & 7)) & 1 : 0) << i;
    return v;
  }
  uint32_t peek(uint32_t offset, uint32_t n) const {
    Bits b{d, pos + offset};
    return b.read(n);
  }
};

uint8_t expand_bits(uint8_t v, int bits) {
  v = static_cast<uint8_t>(v << (8 - bits));
  return static_cast<uint8_t>(v | (v >> bits));
}

// BC7. Output: RGBA bytes per texel; a block with a reserved mode is transparent black.
void bc7(const uint8_t* src, Rgba out[16]) {
  Bits bit{src};
  uint32_t mode = 0;
  while (mode < 8 && bit.read(1) == 0) ++mode;
  if (mode == 8) {
    for (int i = 0; i < 16; ++i) for (int k = 0; k < 4; ++k) out[i].c[k] = 0;
    return;
  }
  static const struct { uint8_t subsets, part_bits, rot_bits, idx_sel_bits, color_bits, alpha_bits, pbits, shared_pbits, index_bits[2]; } kModes[8] = {
      {3, 4, 0, 0, 4, 0, 1, 0, {3, 0}}, {2, 6, 0, 0, 6, 0, 0, 1, {3, 0}}, {3, 6, 0, 0, 5, 0, 0, 0, {2, 0}}, {2, 6, 0, 0, 7, 0, 1, 0, {2, 0}},
      {1, 0, 2, 1, 5, 6, 0, 0, {2, 3}}, {1, 0, 2, 0, 7, 8, 0, 0, {2, 2}}, {1, 0, 0, 0, 7, 7, 1, 0, {4, 0}}, {2, 6, 0, 0, 5, 5, 1, 0, {2, 0}}};
  const auto& mi = kModes[mode];
  const uint32_t pb = mi.pbits ? mi.pbits : mi.shared_pbits;
  const uint32_t partition = bit.read(mi.part_bits), rotation = bit.read(mi.rot_bits), idx_sel = bit.read(mi.idx_sel_bits);
  uint8_t ep[4][6];   // r g b a, two end points per subset
  for (int c = 0; c < 3; ++c)
    for (uint32_t i = 0; i < mi.subsets; ++i) {
      ep[c][i * 2] = static_cast<uint8_t>(bit.read(mi.color_bits) << pb);
      ep[c][i * 2 + 1] = static_cast<uint8_t>(bit.read(mi.color_bits) << pb);
    }
  for (uint32_t i = 0; i < 6; ++i) ep[3][i] = 255;
  if (mi.alpha_bits)
    for (uint32_t i = 0; i < mi.subsets; ++i) {
      ep[3][i * 2] = static_cast<uint8_t>(bit.read(mi.alpha_bits) << pb);
      ep[3][i * 2 + 1] = static_cast<uint8_t>(bit.read(mi.alpha_bits) << pb);
    }
  if (pb)
    for (uint32_t i = 0; i < mi.subsets; ++i) {
      const uint8_t pa = static_cast<uint8_t>(bit.read(pb)), pe = static_cast<uint8_t>(mi.shared_pbits == 0 ? bit.read(pb) : pa);
      for (int c = 0; c < 4; ++c) {
        ep[c][i * 2] = static_cast<uint8_t>(ep[c][i * 2] | pa);
        ep[c][i * 2 + 1] = static_cast<uint8_t>(ep[c][i * 2 + 1] | pe);
      }
    }
  for (uint32_t i = 0; i < mi.subsets * 2; ++i) {
    for (int c = 0; c < 3; ++c) ep[c][i] = expand_bits(ep[c][i], mi.color_bits + pb);
    if (mi.alpha_bits) ep[3][i] = expand_bits(ep[3][i], mi.alpha_bits + pb);
  }
  const bool two_sets = mi.index_bits[1] != 0;
  const uint8_t* fac[2] = {kBptcFactors[mi.index_bits[0] - 2], two_sets ? kBptcFactors[mi.index_bits[1] - 2] : kBptcFactors[mi.index_bits[0] - 2]};
  uint32_t offset[2] = {0, static_cast<uint32_t>(mi.subsets * (16 * mi.index_bits[0] - 1))};
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t subset = 0, anchor_texel = 0;
    if (mi.subsets == 2) {
      subset = (kBptcP2[partition] >> i) & 1;
      anchor_texel = subset ? kBptcA2[partition] : 0;
    } else if (mi.subsets == 3) {
      subset = (kBptcP3[partition] >> (2 * i)) & 3;
      anchor_texel = subset ? kBptcA3[(subset - 1) * 64 + partition] : 0;
    }
    const uint32_t is_anchor = i == anchor_texel;
    const uint32_t n0 = mi.index_bits[0] - is_anchor, n1 = two_sets ? mi.index_bits[1] - is_anchor : 0;
    const uint32_t idx0 = bit.peek(offset[0], n0), idx1 = two_sets ? bit.peek(offset[1], n1) : idx0;
    offset[0] += n0;
    offset[1] += n1;
    const uint32_t idx[2] = {idx0, idx1};
    const uint32_t fc = fac[idx_sel][idx[idx_sel]], fa = fac[!idx_sel][idx[!idx_sel]];
    const uint32_t s2 = subset * 2;
    uint8_t px[4];
    for (int c = 0; c < 3; ++c) px[c] = static_cast<uint8_t>((ep[c][s2] * (64 - fc) + ep[c][s2 + 1] * fc + 32) >> 6);
    px[3] = static_cast<uint8_t>((ep[3][s2] * (64 - fa) + ep[3][s2 + 1] * fa + 32) >> 6);
    if (rotation) { const uint8_t t = px[3]; px[3] = px[rotation - 1]; px[rotation - 1] = t; }
    for (int c = 0; c < 4; ++c) out[i].c[c] = px[c];
  }
}

// BC6H: half-float RGB; the modes and their bit fields are in the tables.
uint16_t bc6_unquantize(uint16_t v, bool is_signed, uint8_t bits) {
  const uint16_t max_value = static_cast<uint16_t>(1 << (bits - 1));
  if (is_signed) {
    if (bits >= 16) return v;
    const bool sign = (v & 0x8000) != 0;
    v &= 0x7fff;
    uint16_t unq;
    if (v == 0) unq = 0;
    else if (v >= max_value - 1) unq = 0x7fff;
    else unq = static_cast<uint16_t>(((v << 15) + 0x4000) >> (bits - 1));
    return sign ? static_cast<uint16_t>(-unq) : unq;
  }
  if (bits >= 15) return v;
  if (v == 0) return 0;
  if (v == max_value) return 0xFFFF;
  return static_cast<uint16_t>(((v << 15) + 0x4000) >> (bits - 1));
}
uint16_t bc6_finish(uint16_t v, bool is_signed) {
  if (is_signed) {
    const uint16_t sign = v & 0x8000;
    v &= 0x7fff;
    return static_cast<uint16_t>(((v * 31) >> 5) | sign);
  }
  return static_cast<uint16_t>((v * 31) >> 6);
}
uint16_t sign_extend16(uint16_t v, uint8_t bits) {
  const uint16_t mask = static_cast<uint16_t>(1 << (bits - 1));
  return static_cast<uint16_t>((v ^ mask) - mask);
}

void bc6(const uint8_t* src, bool is_signed, float out[16][3]) {
  static const struct { uint8_t transformed, part_bits, endpoint_bits, delta[3]; } kInfo[32] = {
      {1, 5, 10, {5, 5, 5}}, {1, 5, 7, {6, 6, 6}}, {1, 5, 11, {5, 4, 4}}, {0, 0, 10, {10, 10, 10}}, {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {1, 5, 11, {4, 5, 4}}, {1, 0, 11, {9, 9, 9}},
      {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {1, 5, 11, {4, 4, 5}}, {1, 0, 12, {8, 8, 8}}, {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {1, 5, 9, {5, 5, 5}}, {1, 0, 16, {4, 4, 4}},
      {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {1, 5, 8, {6, 5, 5}}, {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {1, 5, 8, {5, 6, 5}}, {0, 0, 0, {0, 0, 0}},
      {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {1, 5, 8, {5, 5, 6}}, {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {0, 0, 0, {0, 0, 0}}, {0, 5, 6, {6, 6, 6}}, {0, 0, 0, {0, 0, 0}}};
  Bits bit{src};
  uint32_t mode = bit.read(2);
  if (mode & 2) mode |= bit.read(3) << 2;
  const auto& mi = kInfo[mode];
  if (mi.endpoint_bits == 0) {
    for (int i = 0; i < 16; ++i) out[i][0] = out[i][1] = out[i][2] = 0;
    return;
  }
  uint16_t ep[3][4] = {};
  for (const Bc6Field& f : kBc6Fields)
    if (f.mode == mode) ep[f.comp][f.ep] = static_cast<uint16_t>(ep[f.comp][f.ep] | (bit.read(f.bits) << f.shift));
  if (is_signed)
    for (int c = 0; c < 3; ++c) ep[c][0] = sign_extend16(ep[c][0], mi.endpoint_bits);
  const uint32_t subsets = mi.part_bits ? 2 : 1;
  for (uint32_t i = 1; i < subsets * 2; ++i) {
    for (int c = 0; c < 3; ++c) {
      if (is_signed || mi.transformed) ep[c][i] = sign_extend16(ep[c][i], mi.delta[c]);
      if (mi.transformed) {
        const uint16_t mask = static_cast<uint16_t>((1 << mi.endpoint_bits) - 1);
        ep[c][i] = static_cast<uint16_t>((ep[c][i] + ep[c][0]) & mask);
        if (is_signed) ep[c][i] = sign_extend16(ep[c][i], mi.endpoint_bits);
      }
    }
  }
  for (uint32_t i = 0; i < subsets * 2; ++i)
    for (int c = 0; c < 3; ++c) ep[c][i] = bc6_unquantize(ep[c][i], is_signed, mi.endpoint_bits);
  const uint32_t partition = mi.part_bits ? bit.read(5) : 0;
  const uint32_t index_bits = mi.part_bits ? 3 : 4;
  const uint8_t* fac = kBptcFactors[index_bits - 2];
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t subset = 0, anchor_texel = 0;
    if (mi.part_bits) {
      subset = (kBptcP2[partition] >> i) & 1;
      anchor_texel = subset ? kBptcA2[partition] : 0;
    }
    const uint32_t n = index_bits - (i == anchor_texel ? 1 : 0);
    const uint32_t idx = bit.read(n);
    const int fc = fac[idx], fa = 64 - fc;
    for (int c = 0; c < 3; ++c) {
      const uint16_t v = bc6_finish(static_cast<uint16_t>((ep[c][subset * 2] * fa + ep[c][subset * 2 + 1] * fc + 32) >> 6), is_signed);
      out[i][c] = shader::half_to_float(v);
    }
  }
}


// --- ASTC (LDR profile) -----------------------------------------------------------------------------------------------------------------------------

// A value of the integer sequence encoding: its plain bits and its trit/quint digit.
struct IseValue {
  int bits = 0, digit = 0;
};

// The block as a bit string: bit i of `d` (least significant first). `Bits`-like reader over a 16 byte array that may be bit-reversed.
inline int block_bits(const uint8_t* d, int pos, int n) {
  int v = 0;
  for (int i = 0; i < n; ++i) {
    const int p = pos + i;
    if (p >= 0 && p < 128) v |= ((d[p >> 3] >> (p & 7)) & 1) << i;
  }
  return v;
}

// Decodes `count` values of a range given by (a: 0 plain bits, 3 trits, 5 quints) and b bits, from bit `pos` of the bit string d.
void decode_ise(const uint8_t* d, int pos, int a, int b, int count, IseValue* out) {
  if (a == 0) {
    for (int i = 0; i < count; ++i, pos += b) out[i].bits = block_bits(d, pos, b);
    return;
  }
  const int per = a == 3 ? 5 : 3;                           // values per group
  const int group_bits = a == 3 ? 8 + 5 * b : 7 + 3 * b;   // bits per group
  const int groups = (count + per - 1) / per;
  int n = 0;
  for (int g = 0; g < groups; ++g, pos += group_bits) {
    const int last = count - g * per < per ? count - g * per : per;
    // a partial last group stores only the bits its values need
    const int have = g == groups - 1 ? (group_bits * last + per - 1) / per : group_bits;
    const long long raw = block_bits(d, pos, have > 30 ? 30 : have) | (have > 30 ? static_cast<long long>(block_bits(d, pos + 30, have - 30)) << 30 : 0);
    int m[5], hi = 0;
    if (a == 3) {   // layout: m0 T[1:0] m1 T[3:2] m2 T[4] m3 T[6:5] m4 T[7]
      const int mask = (1 << b) - 1;
      int off = 0;
      m[0] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 3); off += 2;
      m[1] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 3) << 2; off += 2;
      m[2] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 1) << 4; off += 1;
      m[3] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 3) << 5; off += 2;
      m[4] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 1) << 7;
      int c, t4, t3, t2, t1, t0;
      if (((hi >> 2) & 7) == 7) { c = (((hi >> 5) & 7) << 2) | (hi & 3); t4 = t3 = 2; }
      else {
        c = hi & 31;
        if (((hi >> 5) & 3) == 3) { t4 = 2; t3 = (hi >> 7) & 1; }
        else { t4 = (hi >> 7) & 1; t3 = (hi >> 5) & 3; }
      }
      if ((c & 3) == 3) { t2 = 2; t1 = (c >> 4) & 1; t0 = (((c >> 3) & 1) << 1) | (((c >> 2) & 1) & ~((c >> 3) & 1) & 1); }
      else if (((c >> 2) & 3) == 3) { t2 = 2; t1 = 2; t0 = c & 3; }
      else { t2 = (c >> 4) & 1; t1 = (c >> 2) & 3; t0 = (((c >> 1) & 1) << 1) | ((c & 1) & ~((c >> 1) & 1) & 1); }
      const int t[5] = {t0, t1, t2, t3, t4};
      for (int j = 0; j < 5 && n < count; ++j, ++n) { out[n].bits = m[j]; out[n].digit = t[j]; }
    } else {         // layout: m0 Q[2:0] m1 Q[4:3] m2 Q[6:5]
      const int mask = (1 << b) - 1;
      int off = 0;
      m[0] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 7); off += 3;
      m[1] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 3) << 3; off += 2;
      m[2] = static_cast<int>((raw >> off) & mask); off += b; hi |= static_cast<int>((raw >> off) & 3) << 5;
      int q0, q1, q2;
      if (((hi >> 1) & 3) == 3 && ((hi >> 5) & 3) == 0) {
        q2 = ((hi & 1) << 2) | ((((hi >> 4) & 1) & ~(hi & 1) & 1) << 1) | (((hi >> 3) & 1) & ~(hi & 1) & 1);
        q1 = q0 = 4;
      } else {
        int c;
        if (((hi >> 1) & 3) == 3) { q2 = 4; c = (((hi >> 3) & 3) << 3) | ((~((hi >> 5) & 3) & 3) << 1) | (hi & 1); }
        else { q2 = (hi >> 5) & 3; c = hi & 31; }
        if ((c & 7) == 5) { q1 = 4; q0 = (c >> 3) & 3; }
        else { q1 = (c >> 3) & 3; q0 = c & 7; }
      }
      const int q[3] = {q0, q1, q2};
      for (int j = 0; j < 3 && n < count; ++j, ++n) { out[n].bits = m[j]; out[n].digit = q[j]; }
    }
  }
}

// How many bits `count` values of the range (a, b) take.
int ise_bits(int count, int a, int b) {
  if (a == 3) return count * b + (count * 8 + 4) / 5;
  if (a == 5) return count * b + (count * 7 + 2) / 3;
  return count * b;
}

const int kWeightA[16] = {0, 0, 0, 3, 0, 5, 3, 0, 0, 0, 5, 3, 0, 5, 3, 0};
const int kWeightB[16] = {0, 0, 1, 0, 2, 0, 1, 3, 0, 0, 1, 2, 4, 2, 3, 5};
const int kCemA[19] = {0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 0, 0};
const int kCemB[19] = {8, 6, 5, 7, 5, 4, 6, 4, 3, 5, 3, 2, 4, 2, 1, 3, 1, 2, 1};

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Moves the top bit of a into the top of b as the signed-delta encodings need.
void bit_transfer(int* a, int* b) {
  *b = (*b >> 1) | (*a & 0x80);
  *a = (*a >> 1) & 0x3f;
  if (*a & 0x20) *a -= 0x40;
}

// Decodes one ASTC block into RGBA bytes. Returns false for an invalid block (the caller shows magenta) and sets *hdr for blocks that need the HDR profile.
bool astc_block(const uint8_t* buf, int bw, int bh, bool srgb, Rgba* out, bool* hdr) {
  *hdr = false;
  const int nt = bw * bh;
  auto u16 = [&](int i) { return buf[i] | (buf[i + 1] << 8); };
  // void extent: one colour
  if (buf[0] == 0xfc && (buf[1] & 1) == 1) {
    if (buf[1] & 2) { *hdr = true; return false; }
    Rgba c;
    for (int k = 0; k < 4; ++k) c.c[k] = buf[9 + 2 * k];
    for (int i = 0; i < nt; ++i) out[i] = c;
    return true;
  }
  if (((buf[0] & 0xc3) == 0xc0 && (buf[1] & 1) == 1) || (buf[0] & 0xf) == 0) return false;
  // block mode
  const bool dual_in = (buf[1] & 4) != 0;
  bool dual = dual_in;
  int range = ((buf[0] >> 4) & 1) | ((buf[1] << 2) & 8);
  int gw = 0, gh = 0;
  if (buf[0] & 3) {
    range |= (buf[0] << 1) & 6;
    switch (buf[0] & 0xc) {
      case 0: gw = ((u16(0) >> 7) & 3) + 4; gh = ((buf[0] >> 5) & 3) + 2; break;
      case 4: gw = ((u16(0) >> 7) & 3) + 8; gh = ((buf[0] >> 5) & 3) + 2; break;
      case 8: gw = ((buf[0] >> 5) & 3) + 2; gh = ((u16(0) >> 7) & 3) + 8; break;
      default:
        if (buf[1] & 1) { gw = ((buf[0] >> 7) & 1) + 2; gh = ((buf[0] >> 5) & 3) + 2; }
        else { gw = ((buf[0] >> 5) & 3) + 2; gh = ((buf[0] >> 7) & 1) + 6; }
        break;
    }
  } else {
    range |= (buf[0] >> 1) & 6;
    switch (u16(0) & 0x180) {
      case 0: gw = 12; gh = ((buf[0] >> 5) & 3) + 2; break;
      case 0x80: gw = ((buf[0] >> 5) & 3) + 2; gh = 12; break;
      case 0x100: gw = ((buf[0] >> 5) & 3) + 6; gh = ((buf[1] >> 1) & 3) + 6; dual = false; range &= 7; break;
      default: gw = (buf[0] & 0x20) ? 10 : 6; gh = (buf[0] & 0x20) ? 6 : 10; break;
    }
  }
  const int parts = ((buf[1] >> 3) & 3) + 1;
  const int nweights = gw * gh * (dual ? 2 : 1);
  if (range < 2 || nweights > 64 || gw > bw || gh > bh || (dual && parts == 4)) return false;
  const int wa = kWeightA[range], wb = kWeightB[range];
  const int weight_bits = ise_bits(nweights, wa, wb);
  if (weight_bits < 24 || weight_bits > 96) return false;
  // colour end point modes
  int cem[4] = {0, 0, 0, 0}, config_bits, cem_base = 0;
  if (parts == 1) {
    cem[0] = (u16(1) >> 5) & 0xf;
    config_bits = 17;
  } else {
    cem_base = (u16(2) >> 7) & 3;
    if (cem_base == 0) {
      for (int i = 0; i < parts; ++i) cem[i] = (buf[3] >> 1) & 0xf;
      config_bits = 29;
    } else {
      for (int i = 0; i < parts; ++i) cem[i] = (((buf[3] >> (i + 1)) & 1) + cem_base - 1) << 2;
      switch (parts) {
        case 2:
          cem[0] |= (buf[3] >> 3) & 3;
          cem[1] |= block_bits(buf, 126 - weight_bits, 2);
          break;
        case 3:
          cem[0] |= (buf[3] >> 4) & 1;
          cem[0] |= block_bits(buf, 122 - weight_bits, 2) & 2;
          cem[1] |= block_bits(buf, 124 - weight_bits, 2);
          cem[2] |= block_bits(buf, 126 - weight_bits, 2);
          break;
        default:
          for (int i = 0; i < 4; ++i) cem[i] |= block_bits(buf, 120 + i * 2 - weight_bits, 2);
          break;
      }
      config_bits = 25 + parts * 3;
    }
  }
  int plane_sel = 0;
  if (dual) {
    config_bits += 2;
    plane_sel = block_bits(buf, cem_base ? 130 - weight_bits - parts * 3 : 126 - weight_bits, 2);
  }
  const int remain = 128 - config_bits - weight_bits;
  int nvalues = 0;
  for (int i = 0; i < parts; ++i) {
    nvalues += ((cem[i] >> 1) & 6) + 2;
    if (cem[i] == 2 || cem[i] == 3 || cem[i] == 7 || cem[i] == 11 || cem[i] >= 14) *hdr = true;
  }
  if (nvalues > 18) return false;
  int cem_range = -1;
  for (int i = 0; i < 19; ++i)
    if (ise_bits(nvalues, kCemA[i], kCemB[i]) <= remain) { cem_range = i; break; }
  if (cem_range < 0) return false;
  if (*hdr) return false;
  // colour values
  IseValue seq[32];
  decode_ise(buf, parts == 1 ? 17 : 29, kCemA[cem_range], kCemB[cem_range], nvalues, seq);
  int ev[32];
  const int ca = kCemA[cem_range], cb = kCemB[cem_range];
  for (int i = 0; i < nvalues; ++i) {
    const int bits = seq[i].bits;
    if (ca == 0) {
      switch (cb) {
        case 1: ev[i] = bits * 0xff; break;
        case 2: ev[i] = bits * 0x55; break;
        case 3: ev[i] = (bits << 5) | (bits << 2) | (bits >> 1); break;
        case 4: ev[i] = (bits << 4) | bits; break;
        case 5: ev[i] = (bits << 3) | (bits >> 2); break;
        case 6: ev[i] = (bits << 2) | (bits >> 4); break;
        case 7: ev[i] = (bits << 1) | (bits >> 6); break;
        default: ev[i] = bits; break;
      }
    } else {
      const int a = (bits & 1) * 0x1ff, x = bits >> 1;
      int b = 0, c;
      if (ca == 3) {
        static const int kC[7] = {0, 204, 93, 44, 22, 11, 5};
        c = kC[cb];
        switch (cb) {
          case 2: b = 0x116 * x; break;
          case 3: b = (x << 7) | (x << 2) | x; break;
          case 4: b = (x << 6) | x; break;
          case 5: b = (x << 5) | (x >> 2); break;
          case 6: b = (x << 4) | (x >> 4); break;
          default: break;
        }
      } else {
        static const int kC[6] = {0, 113, 54, 26, 13, 6};
        c = kC[cb];
        switch (cb) {
          case 2: b = 0x10c * x; break;
          case 3: b = (x << 7) | (x << 1) | (x >> 1); break;
          case 4: b = (x << 6) | (x >> 1); break;
          case 5: b = (x << 5) | (x >> 3); break;
          default: break;
        }
      }
      ev[i] = (a & 0x80) | (((seq[i].digit * c + b) ^ a) >> 2);
    }
  }
  int ends[4][8];
  const int* v = ev;
  auto set = [&](int p, int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2, bool clamp, bool blue) {
    if (blue) { r1 = (r1 + b1) >> 1; g1 = (g1 + b1) >> 1; r2 = (r2 + b2) >> 1; g2 = (g2 + b2) >> 1; }
    const int e[8] = {r1, g1, b1, a1, r2, g2, b2, a2};
    for (int k = 0; k < 8; ++k) ends[p][k] = clamp ? clamp255(e[k]) : e[k];
  };
  for (int p = 0; p < parts; v += ((cem[p] / 4) + 1) * 2, ++p) {
    int t[8];
    for (int k = 0; k < 8; ++k) t[k] = v[k];
    switch (cem[p]) {
      case 0: set(p, t[0], t[0], t[0], 255, t[1], t[1], t[1], 255, false, false); break;
      case 1: { const int l0 = (t[0] >> 2) | (t[1] & 0xc0), l1 = clamp255(l0 + (t[1] & 0x3f)); set(p, l0, l0, l0, 255, l1, l1, l1, 255, false, false); break; }
      case 4: set(p, t[0], t[0], t[0], t[2], t[1], t[1], t[1], t[3], false, false); break;
      case 5:
        bit_transfer(&t[1], &t[0]);
        bit_transfer(&t[3], &t[2]);
        t[1] += t[0];
        set(p, t[0], t[0], t[0], t[2], t[1], t[1], t[1], t[2] + t[3], true, false);
        break;
      case 6: set(p, (t[0] * t[3]) >> 8, (t[1] * t[3]) >> 8, (t[2] * t[3]) >> 8, 255, t[0], t[1], t[2], 255, false, false); break;
      case 8:
        if (t[0] + t[2] + t[4] <= t[1] + t[3] + t[5]) set(p, t[0], t[2], t[4], 255, t[1], t[3], t[5], 255, false, false);
        else set(p, t[1], t[3], t[5], 255, t[0], t[2], t[4], 255, false, true);
        break;
      case 9:
        bit_transfer(&t[1], &t[0]);
        bit_transfer(&t[3], &t[2]);
        bit_transfer(&t[5], &t[4]);
        if (t[1] + t[3] + t[5] >= 0) set(p, t[0], t[2], t[4], 255, t[0] + t[1], t[2] + t[3], t[4] + t[5], 255, true, false);
        else set(p, t[0] + t[1], t[2] + t[3], t[4] + t[5], 255, t[0], t[2], t[4], 255, true, true);
        break;
      case 10: set(p, (t[0] * t[3]) >> 8, (t[1] * t[3]) >> 8, (t[2] * t[3]) >> 8, t[4], t[0], t[1], t[2], t[5], false, false); break;
      case 12:
        if (t[0] + t[2] + t[4] <= t[1] + t[3] + t[5]) set(p, t[0], t[2], t[4], t[6], t[1], t[3], t[5], t[7], false, false);
        else set(p, t[1], t[3], t[5], t[7], t[0], t[2], t[4], t[6], false, true);
        break;
      case 13:
        bit_transfer(&t[1], &t[0]);
        bit_transfer(&t[3], &t[2]);
        bit_transfer(&t[5], &t[4]);
        bit_transfer(&t[7], &t[6]);
        if (t[1] + t[3] + t[5] >= 0) set(p, t[0], t[2], t[4], t[6], t[0] + t[1], t[2] + t[3], t[4] + t[5], t[6] + t[7], true, false);
        else set(p, t[0] + t[1], t[2] + t[3], t[4] + t[5], t[6] + t[7], t[0], t[2], t[4], t[6], true, true);
        break;
      default: return false;
    }
  }
  // weights: the bit string is read backwards from the end of the block
  uint8_t rev[16];
  for (int i = 0; i < 16; ++i) rev[i] = 0;
  for (int i = 0; i < 128; ++i) if ((buf[i >> 3] >> (i & 7)) & 1) rev[(127 - i) >> 3] = static_cast<uint8_t>(rev[(127 - i) >> 3] | (1 << ((127 - i) & 7)));
  IseValue wseq[128];
  decode_ise(rev, 0, wa, wb, nweights, wseq);
  int wv[128];
  for (int i = 0; i < nweights; ++i) {
    const int bits = wseq[i].bits;
    int w;
    if (wa == 0) {
      switch (wb) {
        case 1: w = bits ? 63 : 0; break;
        case 2: w = (bits << 4) | (bits << 2) | bits; break;
        case 3: w = (bits << 3) | bits; break;
        case 4: w = (bits << 2) | (bits >> 2); break;
        default: w = (bits << 1) | (bits >> 4); break;
      }
      if (w > 32) ++w;
    } else if (wb == 0) {
      const int d = wseq[i].digit;   // 0, 32, 63 (trits) or 0, 16, 32, 47, 63 (quints)
      w = wa == 3 ? (d == 2 ? 63 : d * 32) : (d >= 3 ? d * 16 - 1 : d * 16);
      if (w > 32) ++w;
    } else {
      const int d = wseq[i].digit;
      if (wa == 3) {
        switch (wb) {
          case 1: w = d * 50; break;
          case 2: w = d * 23 + ((bits & 2) ? 0x45 : 0); break;
          default: w = d * 11 + (((bits << 4) | (bits >> 1)) & 0x63); break;
        }
      } else {
        switch (wb) {
          case 1: w = d * 28; break;
          default: w = d * 13 + ((bits & 2) ? 0x42 : 0); break;
        }
      }
      const int a = (bits & 1) * 0x7f;
      w = (a & 0x20) | ((w ^ a) >> 2);
      if (w > 32) ++w;
    }
    wv[i] = w;
  }
  const int ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
  const int pn = dual ? 2 : 1;
  int weights[144][2];
  for (int t = 0, i = 0; t < bh; ++t)
    for (int s = 0; s < bw; ++s, ++i) {
      const int gs = (ds * s * (gw - 1) + 32) >> 6, gt = (dt * t * (gh - 1) + 32) >> 6;
      const int fs = gs & 0xf, ft = gt & 0xf;
      const int vi = (gs >> 4) + (gt >> 4) * gw;
      const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
      for (int pl = 0; pl < pn; ++pl) {
        const int p00 = wv[vi * pn + pl], p01 = wv[(vi + 1) * pn + pl], p10 = wv[(vi + gw) * pn + pl], p11 = wv[(vi + gw + 1) * pn + pl];
        weights[i][pl] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
      }
    }
  // partitions
  int part[144];
  for (int i = 0; i < nt; ++i) part[i] = 0;
  if (parts > 1) {
    const bool small = nt < 31;
    const int seed = ((buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)) >> 13 & 0x3ff) | ((parts - 1) << 10);
    uint32_t rnum = static_cast<uint32_t>(seed);
    rnum ^= rnum >> 15;
    rnum -= rnum << 17;
    rnum += rnum << 7;
    rnum += rnum << 4;
    rnum ^= rnum >> 5;
    rnum += rnum << 16;
    rnum ^= rnum >> 7;
    rnum ^= rnum >> 3;
    rnum ^= rnum << 6;
    rnum ^= rnum >> 17;
    int seeds[8];
    for (int i = 0; i < 8; ++i) {
      seeds[i] = static_cast<int>((rnum >> (i * 4)) & 0xF);
      seeds[i] *= seeds[i];
    }
    const int sh[2] = {(seed & 2) ? 4 : 5, parts == 3 ? 6 : 5};
    if (seed & 1) for (int i = 0; i < 8; ++i) seeds[i] >>= sh[i % 2];
    else for (int i = 0; i < 8; ++i) seeds[i] >>= sh[1 - i % 2];
    for (int y = 0, i = 0; y < bh; ++y)
      for (int x = 0; x < bw; ++x, ++i) {
        const int px = small ? x << 1 : x, py = small ? y << 1 : y;
        const int pa = (seeds[0] * px + seeds[1] * py + static_cast<int>(rnum >> 14)) & 0x3f;
        const int pb = (seeds[2] * px + seeds[3] * py + static_cast<int>(rnum >> 10)) & 0x3f;
        const int pc = parts < 3 ? 0 : (seeds[4] * px + seeds[5] * py + static_cast<int>(rnum >> 6)) & 0x3f;
        const int pd = parts < 4 ? 0 : (seeds[6] * px + seeds[7] * py + static_cast<int>(rnum >> 2)) & 0x3f;
        part[i] = (pa >= pb && pa >= pc && pa >= pd) ? 0 : (pb >= pc && pb >= pd) ? 1 : (pc >= pd) ? 2 : 3;
      }
  }
  int ps[4] = {0, 0, 0, 0};
  if (dual) ps[plane_sel] = 1;
  for (int i = 0; i < nt; ++i) {
    const int p = part[i];
    for (int k = 0; k < 4; ++k) {
      const int w = weights[i][dual ? ps[k] : 0];
      const int e0 = ends[p][k], e1 = ends[p][4 + k];
      if (srgb) out[i].c[k] = static_cast<uint8_t>((((e0 << 8) | 0x80) * (64 - w) + ((e1 << 8) | 0x80) * w + 32) >> 14);
      else out[i].c[k] = static_cast<uint8_t>((((((e0 << 8) | e0) * (64 - w) + ((e1 << 8) | e1) * w + 32) >> 6) * 255 + 32768) >> 16);
    }
  }
  return true;
}

}  // namespace

const CompressedFormat* find_compressed(uint32_t internal) {
  for (const CompressedFormat& f : kFormats)
    if (f.internal == internal) return &f;
  return nullptr;
}

uint64_t compressed_size(const CompressedFormat& f, uint32_t w, uint32_t h, uint32_t d) {
  return uint64_t{(w + f.bw - 1) / f.bw} * ((h + f.bh - 1) / f.bh) * d * f.block_bytes;
}

bool decode_compressed(const CompressedFormat& f, ConstByteView data, uint32_t w, uint32_t h, uint32_t d, Image* dst, uint32_t xo, uint32_t yo, uint32_t zo) {
  if (data.size() < compressed_size(f, w, h, d)) return false;
  const uint32_t bw = (w + f.bw - 1) / f.bw, bh = (h + f.bh - 1) / f.bh;
  const bool is_float = dst->info->store == Store::F32x4;
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint8_t* p = data.data() + ((uint64_t{z} * bh + by) * bw + bx) * f.block_bytes;
        Rgba px[144] = {};
        float chan[2][16] = {};
        float rgb[16][3] = {};
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
          case Codec::Bc7: bc7(p, px); break;
          case Codec::Bc6U: case Codec::Bc6S: bc6(p, f.codec == Codec::Bc6S, rgb); break;
          case Codec::Astc: {
            bool hdr;
            if (!astc_block(p, f.bw, f.bh, f.srgb, px, &hdr))
              for (int i = 0; i < f.bw * f.bh; ++i) { px[i].c[0] = 255; px[i].c[1] = 0; px[i].c[2] = 255; px[i].c[3] = 255; }   // an invalid block (or HDR) is magenta
            break;
          }
        }
        const bool two = f.codec == Codec::Bc5U || f.codec == Codec::Bc5S, one = f.codec == Codec::Bc4U || f.codec == Codec::Bc4S;
        const bool hdr6 = f.codec == Codec::Bc6U || f.codec == Codec::Bc6S;
        for (uint32_t i = 0; i < static_cast<uint32_t>(f.bw) * f.bh; ++i) {
          const uint32_t tx = bx * f.bw + i % f.bw, ty = by * f.bh + i / f.bw;
          const uint32_t x = xo + tx, y = yo + ty;
          if (x >= dst->w || y >= dst->h || tx >= w || ty >= h) continue;
          uint32_t lanes[4];
          if (hdr6) {
            for (int k = 0; k < 3; ++k) lanes[k] = f2u(rgb[i][k]);
            lanes[3] = f2u(1.0f);
          } else if (one || two) {
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
