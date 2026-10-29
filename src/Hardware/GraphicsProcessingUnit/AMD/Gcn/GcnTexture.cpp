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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTexture.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnPm4.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"

namespace gcn {

using namespace pm4;

namespace {

constexpr uint32_t kMaxDimension = 16384;
constexpr uint32_t kMaxAddressBits = 40;
constexpr uint32_t kDstSelX = 4;

struct FormatCodes { TexelFormat format; uint32_t data, number; };
constexpr FormatCodes kFormats[] = {
    {TexelFormat::R8Unorm, kIMG_DATA_FORMAT_8, kIMG_NUM_FORMAT_UNORM},
    {TexelFormat::Rgba8Unorm, kIMG_DATA_FORMAT_8_8_8_8, kIMG_NUM_FORMAT_UNORM},
    {TexelFormat::Rgba32Float, kIMG_DATA_FORMAT_32_32_32_32, kIMG_NUM_FORMAT_FLOAT},
};

bool dst_sel_ok(uint32_t s) { return s == 0 || s == 1 || (s >= kDstSelX && s <= kDstSelX + 3); }

float bits_float(uint32_t b) {
  union { uint32_t u; float f; } c;
  c.u = b;
  return c.f;
}

// floor() of a float as an int64, without leaving the range of one (NaN is 0)
int64_t floor_i(float v) {
  if (!(v == v)) return 0;
  if (v > 1e9f) v = 1e9f;
  if (v < -1e9f) v = -1e9f;
  const int64_t t = static_cast<int64_t>(v);
  return static_cast<float>(t) > v ? t - 1 : t;
}

// The texel an address mode picks for coordinate i of n; false for the border.
bool address(uint32_t mode, int64_t i, int64_t n, int64_t* out) {
  switch (mode) {
    case kSQ_TEX_WRAP: *out = ((i % n) + n) % n; return true;
    case kSQ_TEX_MIRROR: {
      const int64_t m = ((i % (2 * n)) + 2 * n) % (2 * n);
      *out = m < n ? m : 2 * n - 1 - m;
      return true;
    }
    case kSQ_TEX_CLAMP_LAST_TEXEL: *out = i < 0 ? 0 : i >= n ? n - 1 : i; return true;
    default:                                                 // CLAMP_BORDER
      if (i < 0 || i >= n) return false;
      *out = i;
      return true;
  }
}

bool mode_ok(uint32_t mode) { return mode == kSQ_TEX_WRAP || mode == kSQ_TEX_MIRROR || mode == kSQ_TEX_CLAMP_LAST_TEXEL || mode == kSQ_TEX_CLAMP_BORDER; }

}  // namespace

uint32_t texel_bytes(TexelFormat f) { return f == TexelFormat::R8Unorm ? 1 : f == TexelFormat::Rgba8Unorm ? 4 : 16; }

bool make_image_descriptor(ArrayView<uint32_t> out, const ImageDescriptor& d) {
  TileLayout layout;
  if (out.size() < 8 || d.width == 0 || d.height == 0 || d.width > kMaxDimension || d.height > kMaxDimension || d.pitch_pixels < d.width || d.pitch_pixels > kMaxDimension ||
      (d.address & 0xFF) != 0 || (d.address >> kMaxAddressBits) != 0 || d.tile_index >= 32 || !resolve_tile_index(d.tile_index, &layout) ||
      !surface_valid(layout, texel_bytes(d.format), d.pitch_pixels) || d.address % surface_base_align(layout, texel_bytes(d.format)) != 0)
    return false;
  for (const uint8_t s : d.dst_sel)
    if (!dst_sel_ok(s)) return false;
  uint32_t data = 0, number = 0;
  for (const FormatCodes& f : kFormats)
    if (f.format == d.format) { data = f.data; number = f.number; }
  for (uint32_t k = 0; k < 8; ++k) out[k] = 0;
  out[0] = static_cast<uint32_t>(d.address >> 8);
  out[1] = (static_cast<uint32_t>(d.address >> 40) << SQ_IMG_RSRC_WORD1__BASE_ADDRESS_HI__SHIFT) | (data << SQ_IMG_RSRC_WORD1__DATA_FORMAT__SHIFT) | (number << SQ_IMG_RSRC_WORD1__NUM_FORMAT__SHIFT);
  out[2] = ((d.width - 1) << SQ_IMG_RSRC_WORD2__WIDTH__SHIFT) | ((d.height - 1) << SQ_IMG_RSRC_WORD2__HEIGHT__SHIFT);
  out[3] = (static_cast<uint32_t>(d.dst_sel[0]) << SQ_IMG_RSRC_WORD3__DST_SEL_X__SHIFT) | (static_cast<uint32_t>(d.dst_sel[1]) << SQ_IMG_RSRC_WORD3__DST_SEL_Y__SHIFT) |
           (static_cast<uint32_t>(d.dst_sel[2]) << SQ_IMG_RSRC_WORD3__DST_SEL_Z__SHIFT) | (static_cast<uint32_t>(d.dst_sel[3]) << SQ_IMG_RSRC_WORD3__DST_SEL_W__SHIFT) |
           (d.tile_index << SQ_IMG_RSRC_WORD3__TILING_INDEX__SHIFT) | (kSQ_RSRC_IMG_2D << SQ_IMG_RSRC_WORD3__TYPE__SHIFT);
  out[4] = (d.pitch_pixels - 1) << SQ_IMG_RSRC_WORD4__PITCH__SHIFT;
  return true;
}

bool decode_image_descriptor(ArrayView<const uint32_t> w, ImageDescriptor* d) {
  if (w.size() < 8) return false;
  // 2D, one level, one layer, no metadata
  if ((w[3] >> SQ_IMG_RSRC_WORD3__TYPE__SHIFT) != kSQ_RSRC_IMG_2D || ((w[3] >> SQ_IMG_RSRC_WORD3__BASE_LEVEL__SHIFT) & 0xFF) != 0 || (w[4] & ((1u << SQ_IMG_RSRC_WORD4__PITCH__SHIFT) - 1)) != 0 ||
      w[5] != 0 || w[7] != 0)
    return false;
  const uint32_t data = (w[1] >> SQ_IMG_RSRC_WORD1__DATA_FORMAT__SHIFT) & 0x3F, number = (w[1] >> SQ_IMG_RSRC_WORD1__NUM_FORMAT__SHIFT) & 0xF;
  bool found = false;
  for (const FormatCodes& f : kFormats)
    if (f.data == data && f.number == number) { d->format = f.format; found = true; }
  if (!found) return false;
  d->address = (static_cast<uint64_t>(w[1] & 0xFF) << 40) | (static_cast<uint64_t>(w[0]) << 8);
  d->width = (w[2] & 0x3FFF) + 1;
  d->height = ((w[2] >> SQ_IMG_RSRC_WORD2__HEIGHT__SHIFT) & 0x3FFF) + 1;
  d->pitch_pixels = ((w[4] >> SQ_IMG_RSRC_WORD4__PITCH__SHIFT) & 0x3FFF) + 1;
  d->tile_index = (w[3] >> SQ_IMG_RSRC_WORD3__TILING_INDEX__SHIFT) & 0x1F;
  d->dst_sel[0] = w[3] & 7;
  d->dst_sel[1] = (w[3] >> SQ_IMG_RSRC_WORD3__DST_SEL_Y__SHIFT) & 7;
  d->dst_sel[2] = (w[3] >> SQ_IMG_RSRC_WORD3__DST_SEL_Z__SHIFT) & 7;
  d->dst_sel[3] = (w[3] >> SQ_IMG_RSRC_WORD3__DST_SEL_W__SHIFT) & 7;
  for (const uint8_t s : d->dst_sel)
    if (!dst_sel_ok(s)) return false;
  // the fields the descriptor holds that no implemented image uses (the level range, 3D padding, the memory type, ATC) must be as make_image_descriptor writes them
  if ((w[3] & ~(0x0FFFu | (0x1Fu << SQ_IMG_RSRC_WORD3__TILING_INDEX__SHIFT) | (0xFu << SQ_IMG_RSRC_WORD3__TYPE__SHIFT))) != 0 || (w[1] & 0x000FFF00u) != 0 || (w[1] >> 30) != 0 ||
      (w[2] >> 28) != 0)
    return false;
  TileLayout layout;
  return resolve_tile_index(d->tile_index, &layout) && d->pitch_pixels >= d->width && (d->address >> kMaxAddressBits) == 0;
}

bool make_sampler_descriptor(ArrayView<uint32_t> out, const SamplerDescriptor& s) {
  if (out.size() < 4 || !mode_ok(s.clamp_x) || !mode_ok(s.clamp_y) || s.border > 2) return false;
  const uint32_t filter = s.bilinear ? kSQ_TEX_XY_FILTER_BILINEAR : kSQ_TEX_XY_FILTER_POINT;
  out[0] = (static_cast<uint32_t>(s.clamp_x) << SQ_IMG_SAMP_WORD0__CLAMP_X__SHIFT) | (static_cast<uint32_t>(s.clamp_y) << SQ_IMG_SAMP_WORD0__CLAMP_Y__SHIFT) |
           ((s.unnormalized ? 1u : 0u) << SQ_IMG_SAMP_WORD0__FORCE_UNNORMALIZED__SHIFT);
  out[1] = 0xFFFu << SQ_IMG_SAMP_WORD1__MAX_LOD__SHIFT;
  out[2] = (filter << SQ_IMG_SAMP_WORD2__XY_MAG_FILTER__SHIFT) | (filter << SQ_IMG_SAMP_WORD2__XY_MIN_FILTER__SHIFT);
  out[3] = static_cast<uint32_t>(s.border) << SQ_IMG_SAMP_WORD3__BORDER_COLOR_TYPE__SHIFT;
  return true;
}

bool decode_sampler_descriptor(ArrayView<const uint32_t> w, SamplerDescriptor* s) {
  if (w.size() < 4) return false;
  s->clamp_x = w[0] & 7;
  s->clamp_y = (w[0] >> SQ_IMG_SAMP_WORD0__CLAMP_Y__SHIFT) & 7;
  s->unnormalized = (w[0] >> SQ_IMG_SAMP_WORD0__FORCE_UNNORMALIZED__SHIFT & 1) != 0;
  const uint32_t mag = (w[2] >> SQ_IMG_SAMP_WORD2__XY_MAG_FILTER__SHIFT) & 3;
  s->bilinear = mag == kSQ_TEX_XY_FILTER_BILINEAR;
  s->border = static_cast<uint8_t>(w[3] >> SQ_IMG_SAMP_WORD3__BORDER_COLOR_TYPE__SHIFT);
  // the clamp of Z, anisotropy, depth compare, degamma, the filter mode and the rest of the first dword are as written by make_sampler_descriptor; so are the level of detail words
  const uint32_t used0 = (7u << SQ_IMG_SAMP_WORD0__CLAMP_X__SHIFT) | (7u << SQ_IMG_SAMP_WORD0__CLAMP_Y__SHIFT) | (1u << SQ_IMG_SAMP_WORD0__FORCE_UNNORMALIZED__SHIFT);
  return mode_ok(s->clamp_x) && mode_ok(s->clamp_y) && (mag == kSQ_TEX_XY_FILTER_POINT || mag == kSQ_TEX_XY_FILTER_BILINEAR) && ((w[2] >> SQ_IMG_SAMP_WORD2__XY_MIN_FILTER__SHIFT) & 3) == mag &&
         (w[0] & ~used0) == 0 && (w[2] & ~((3u << SQ_IMG_SAMP_WORD2__XY_MAG_FILTER__SHIFT) | (3u << SQ_IMG_SAMP_WORD2__XY_MIN_FILTER__SHIFT))) == 0 && s->border <= 2 &&
         (w[3] & 0x3FFFFFFFu) == 0 && w[1] == (0xFFFu << SQ_IMG_SAMP_WORD1__MAX_LOD__SHIFT);
}

namespace {

SampleStatus fetch(ITexelMemory& memory, const ImageDescriptor& d, const TileLayout& layout, uint32_t x, uint32_t y, float rgba[4]) {
  const uint32_t bytes = texel_bytes(d.format);
  const uint64_t at = d.address + surface_offset(layout, bytes, d.pitch_pixels, x, y, d.height);
  if (d.format == TexelFormat::Rgba32Float) {
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t v;
      if (!memory.read32(at + 4ull * c, &v)) return SampleStatus::Fault;
      rgba[c] = bits_float(v);
    }
    return SampleStatus::Ok;
  }
  uint32_t word;
  if (!memory.read32(at & ~3ull, &word)) return SampleStatus::Fault;
  if (d.format == TexelFormat::R8Unorm) {
    rgba[0] = static_cast<float>((word >> ((at & 3) * 8)) & 0xFF) / 255.0f;
    rgba[1] = rgba[2] = 0.0f;
    rgba[3] = 1.0f;
    return SampleStatus::Ok;
  }
  for (uint32_t c = 0; c < 4; ++c) rgba[c] = static_cast<float>((word >> (c * 8)) & 0xFF) / 255.0f;
  return SampleStatus::Ok;
}

}  // namespace

SampleStatus sample_image(ITexelMemory& memory, ArrayView<const uint32_t> image, ArrayView<const uint32_t> sampler, float u, float v, float out[4]) {
  ImageDescriptor d;
  SamplerDescriptor s;
  TileLayout layout;
  if (!decode_image_descriptor(image, &d) || !decode_sampler_descriptor(sampler, &s) || !resolve_tile_index(d.tile_index, &layout)) return SampleStatus::Unsupported;
  const float border[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
  const float fu = s.unnormalized ? u : u * static_cast<float>(d.width), fv = s.unnormalized ? v : v * static_cast<float>(d.height);
  float texel[4] = {0, 0, 0, 0};
  auto tap = [&](int64_t ix, int64_t iy, float rgba[4]) -> SampleStatus {
    int64_t x, y;
    if (!address(s.clamp_x, ix, d.width, &x) || !address(s.clamp_y, iy, d.height, &y)) {
      for (uint32_t c = 0; c < 4; ++c) rgba[c] = border[s.border][c];
      return SampleStatus::Ok;
    }
    return fetch(memory, d, layout, static_cast<uint32_t>(x), static_cast<uint32_t>(y), rgba);
  };
  if (!s.bilinear) {
    const SampleStatus st = tap(floor_i(fu), floor_i(fv), texel);
    if (st != SampleStatus::Ok) return st;
  } else {
    const float sx = fu - 0.5f, sy = fv - 0.5f;
    const int64_t x0 = floor_i(sx), y0 = floor_i(sy);
    const float tx = sx - static_cast<float>(x0), ty = sy - static_cast<float>(y0);
    float t[4][4];
    const SampleStatus a = tap(x0, y0, t[0]), b = tap(x0 + 1, y0, t[1]), c = tap(x0, y0 + 1, t[2]), e = tap(x0 + 1, y0 + 1, t[3]);
    if (a != SampleStatus::Ok) return a;
    if (b != SampleStatus::Ok) return b;
    if (c != SampleStatus::Ok) return c;
    if (e != SampleStatus::Ok) return e;
    for (uint32_t k = 0; k < 4; ++k) {
      const float top = t[0][k] + (t[1][k] - t[0][k]) * tx, bottom = t[2][k] + (t[3][k] - t[2][k]) * tx;
      texel[k] = top + (bottom - top) * ty;
    }
  }
  for (uint32_t k = 0; k < 4; ++k) out[k] = d.dst_sel[k] == 0 ? 0.0f : d.dst_sel[k] == 1 ? 1.0f : texel[d.dst_sel[k] - kDstSelX];
  return SampleStatus::Ok;
}

}  // namespace gcn
