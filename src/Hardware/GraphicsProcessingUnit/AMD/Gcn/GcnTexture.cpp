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

constexpr uint32_t kMaxDimension = 16384, kMaxDepth = 2048, kMaxLevels = 15;
constexpr uint32_t kMaxAddressBits = 40;
constexpr uint32_t kDstSelX = 4;
constexpr uint32_t kLodMax = 0xFFF;                // 4.8 fixed point
constexpr uint64_t kMaxImageBytes = 1ull << 32;
constexpr uint32_t kLevelAlign = 256;

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

uint32_t next_pow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v && p < (1u << 31)) p <<= 1;
  return p;
}
uint32_t align8(uint32_t v) { return (v + 7) / 8 * 8; }
uint64_t align_to(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

uint32_t type_code(ImageType t) {
  switch (t) {
    case ImageType::Image3D: return kSQ_RSRC_IMG_3D;
    case ImageType::Cube: return kSQ_RSRC_IMG_CUBE;
    case ImageType::Array2D: return kSQ_RSRC_IMG_2D_ARRAY;
    default: return kSQ_RSRC_IMG_2D;
  }
}

bool has_slices(ImageType t) { return t == ImageType::Array2D || t == ImageType::Cube || t == ImageType::Image3D; }

uint32_t max_levels(const ImageDescriptor& d) {
  uint32_t m = d.width > d.height ? d.width : d.height;
  if (d.type == ImageType::Image3D && d.depth > m) m = d.depth;
  uint32_t n = 1;
  while (m > 1) { m >>= 1; ++n; }
  return n;
}

}  // namespace

uint32_t texel_bytes(TexelFormat f) { return f == TexelFormat::R8Unorm ? 1 : f == TexelFormat::Rgba8Unorm ? 4 : 16; }

namespace {

// The pitch of a slice of `rows` rows: widened by 8 until the slice is a multiple of the base alignment (SiLib::HwlGetSizeAdjustmentMicroTiled)
uint32_t widen(uint32_t pitch, uint32_t rows, uint32_t bytes) {
  while ((static_cast<uint64_t>(pitch) * rows * bytes) % kLevelAlign != 0 && pitch < 65536) pitch += 8;
  return pitch;
}

}  // namespace

uint32_t image_pitch(uint32_t width, uint32_t height, uint32_t levels, uint32_t bytes) {
  const uint32_t rows = align8(levels > 1 ? next_pow2(height) : height);
  return widen(align8(levels > 1 ? next_pow2(width) : width), rows, bytes);
}

bool image_level(const ImageDescriptor& d, uint32_t level, LevelLayout* out) {
  TileLayout layout;
  if (level >= d.levels || !resolve_tile_index(d.tile_index, &layout)) return false;
  const uint32_t bytes = texel_bytes(d.format);
  if (d.levels == 1 && d.type == ImageType::Image2D) {
    if (!surface_valid(layout, bytes, d.pitch_pixels)) return false;
    out->offset = 0;
    out->width = d.width;
    out->height = d.height;
    out->depth = 1;
    out->pitch = d.pitch_pixels;
    const uint32_t align = surface_height_align(layout, bytes);
    out->rows = static_cast<uint32_t>(align_to(d.height, align));
    out->slice_bytes = surface_bytes(layout, bytes, d.pitch_pixels, d.height);
    out->slices = 1;
    return true;
  }
  if (layout.kind != TileLayout::Kind::Tiled1D || !surface_valid(layout, bytes, d.pitch_pixels)) return false;
  const bool pow2 = d.levels > 1;
  uint64_t offset = 0;
  for (uint32_t k = 0; k <= level; ++k) {
    LevelLayout l;
    l.offset = offset;
    l.width = (d.width >> k) > 0 ? d.width >> k : 1;
    l.height = (d.height >> k) > 0 ? d.height >> k : 1;
    l.depth = d.type == ImageType::Image3D ? ((d.depth >> k) > 0 ? d.depth >> k : 1) : d.depth;
    l.rows = align8(pow2 ? next_pow2(l.height) : l.height);
    l.pitch = widen(align8((d.pitch_pixels >> k) > 0 ? d.pitch_pixels >> k : 1), l.rows, bytes);
    l.slice_bytes = static_cast<uint64_t>(l.pitch) * l.rows * bytes;
    l.slices = pow2 ? next_pow2(l.depth) : l.depth;
    if (k == level) { *out = l; return true; }
    offset = align_to(offset + l.bytes(), kLevelAlign);
  }
  return false;
}

uint64_t image_bytes(const ImageDescriptor& d) {
  LevelLayout last;
  if (d.levels == 0 || !image_level(d, d.levels - 1, &last)) return 0;
  return last.offset + last.bytes();
}

uint64_t image_texel_offset(const ImageDescriptor& d, const LevelLayout& l, uint32_t x, uint32_t y, uint32_t slice) {
  TileLayout layout;
  if (!resolve_tile_index(d.tile_index, &layout)) return 0;
  return l.offset + slice * l.slice_bytes + surface_offset(layout, texel_bytes(d.format), l.pitch, x, y, d.levels == 1 && d.type == ImageType::Image2D ? d.height : l.rows);
}

bool make_image_descriptor(ArrayView<uint32_t> out, const ImageDescriptor& d) {
  TileLayout layout;
  if (out.size() < 8 || d.width == 0 || d.height == 0 || d.width > kMaxDimension || d.height > kMaxDimension || d.pitch_pixels < d.width || d.pitch_pixels > kMaxDimension ||
      (d.address & 0xFF) != 0 || (d.address >> kMaxAddressBits) != 0 || d.tile_index >= 32 || !resolve_tile_index(d.tile_index, &layout) ||
      !surface_valid(layout, texel_bytes(d.format), d.pitch_pixels) || d.address % surface_base_align(layout, texel_bytes(d.format)) != 0)
    return false;
  for (const uint8_t s : d.dst_sel)
    if (!dst_sel_ok(s)) return false;
  if (d.depth == 0 || d.depth > kMaxDepth || d.levels == 0 || d.levels > kMaxLevels) return false;
  if ((d.type == ImageType::Image2D && d.depth != 1) || (d.type == ImageType::Cube && (d.depth != 6 || d.width != d.height))) return false;
  if (d.levels > max_levels(d)) return false;
  if (d.levels > 1 || has_slices(d.type)) {
    if (layout.kind != TileLayout::Kind::Tiled1D || d.pitch_pixels != image_pitch(d.width, d.height, d.levels, texel_bytes(d.format))) return false;
  }
  if (image_bytes(d) == 0 || image_bytes(d) > kMaxImageBytes) return false;
  uint32_t data = 0, number = 0;
  for (const FormatCodes& f : kFormats)
    if (f.format == d.format) { data = f.data; number = f.number; }
  for (uint32_t k = 0; k < 8; ++k) out[k] = 0;
  out[0] = static_cast<uint32_t>(d.address >> 8);
  out[1] = (static_cast<uint32_t>(d.address >> 40) << SQ_IMG_RSRC_WORD1__BASE_ADDRESS_HI__SHIFT) | (data << SQ_IMG_RSRC_WORD1__DATA_FORMAT__SHIFT) | (number << SQ_IMG_RSRC_WORD1__NUM_FORMAT__SHIFT);
  out[2] = ((d.width - 1) << SQ_IMG_RSRC_WORD2__WIDTH__SHIFT) | ((d.height - 1) << SQ_IMG_RSRC_WORD2__HEIGHT__SHIFT);
  out[3] = (static_cast<uint32_t>(d.dst_sel[0]) << SQ_IMG_RSRC_WORD3__DST_SEL_X__SHIFT) | (static_cast<uint32_t>(d.dst_sel[1]) << SQ_IMG_RSRC_WORD3__DST_SEL_Y__SHIFT) |
           (static_cast<uint32_t>(d.dst_sel[2]) << SQ_IMG_RSRC_WORD3__DST_SEL_Z__SHIFT) | (static_cast<uint32_t>(d.dst_sel[3]) << SQ_IMG_RSRC_WORD3__DST_SEL_W__SHIFT) |
           ((d.levels - 1) << SQ_IMG_RSRC_WORD3__LAST_LEVEL__SHIFT) | (d.tile_index << SQ_IMG_RSRC_WORD3__TILING_INDEX__SHIFT) | (type_code(d.type) << SQ_IMG_RSRC_WORD3__TYPE__SHIFT);
  out[4] = ((d.type == ImageType::Image2D ? 0 : d.depth - 1) << SQ_IMG_RSRC_WORD4__DEPTH__SHIFT) | ((d.pitch_pixels - 1) << SQ_IMG_RSRC_WORD4__PITCH__SHIFT);
  if (d.type == ImageType::Array2D || d.type == ImageType::Cube) out[5] = (d.depth - 1) << SQ_IMG_RSRC_WORD5__LAST_ARRAY__SHIFT;
  return true;
}

bool decode_image_descriptor(ArrayView<const uint32_t> w, ImageDescriptor* d) {
  if (w.size() < 8) return false;
  const uint32_t type = w[3] >> SQ_IMG_RSRC_WORD3__TYPE__SHIFT;
  if (type == kSQ_RSRC_IMG_2D) d->type = ImageType::Image2D;
  else if (type == kSQ_RSRC_IMG_3D) d->type = ImageType::Image3D;
  else if (type == kSQ_RSRC_IMG_CUBE) d->type = ImageType::Cube;
  else if (type == kSQ_RSRC_IMG_2D_ARRAY) d->type = ImageType::Array2D;
  else return false;
  // the base level is 0, no metadata
  if (((w[3] >> SQ_IMG_RSRC_WORD3__BASE_LEVEL__SHIFT) & 0xF) != 0 || w[6] != 0 || w[7] != 0) return false;
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
  d->levels = ((w[3] >> SQ_IMG_RSRC_WORD3__LAST_LEVEL__SHIFT) & 0xF) + 1;
  d->depth = d->type == ImageType::Image2D ? 1 : (w[4] & 0x1FFF) + 1;
  d->dst_sel[0] = w[3] & 7;
  d->dst_sel[1] = (w[3] >> SQ_IMG_RSRC_WORD3__DST_SEL_Y__SHIFT) & 7;
  d->dst_sel[2] = (w[3] >> SQ_IMG_RSRC_WORD3__DST_SEL_Z__SHIFT) & 7;
  d->dst_sel[3] = (w[3] >> SQ_IMG_RSRC_WORD3__DST_SEL_W__SHIFT) & 7;
  for (const uint8_t s : d->dst_sel)
    if (!dst_sel_ok(s)) return false;
  // the fields the descriptor holds that no implemented image uses (3D padding, the memory type, ATC, the minimum level of detail) must be as make_image_descriptor writes them
  const uint32_t used3 = 0xFFFu | (0xFu << SQ_IMG_RSRC_WORD3__LAST_LEVEL__SHIFT) | (0x1Fu << SQ_IMG_RSRC_WORD3__TILING_INDEX__SHIFT) | (0xFu << SQ_IMG_RSRC_WORD3__TYPE__SHIFT);
  if ((w[3] & ~used3) != 0 || (w[1] & 0x000FFF00u) != 0 || (w[1] >> 30) != 0 || (w[2] >> 28) != 0 || (w[4] >> 27) != 0) return false;
  const uint32_t want5 = d->type == ImageType::Array2D || d->type == ImageType::Cube ? (d->depth - 1) << SQ_IMG_RSRC_WORD5__LAST_ARRAY__SHIFT : 0;
  if (w[5] != want5) return false;
  TileLayout layout;
  if (!resolve_tile_index(d->tile_index, &layout) || d->pitch_pixels < d->width || (d->address >> kMaxAddressBits) != 0 || d->levels > max_levels(*d)) return false;
  if ((d->levels > 1 || has_slices(d->type)) && (layout.kind != TileLayout::Kind::Tiled1D || d->pitch_pixels != image_pitch(d->width, d->height, d->levels, texel_bytes(d->format)))) return false;
  if (d->type == ImageType::Cube && (d->depth != 6 || d->width != d->height)) return false;
  return image_bytes(*d) != 0 && image_bytes(*d) <= kMaxImageBytes;
}

bool make_sampler_descriptor(ArrayView<uint32_t> out, const SamplerDescriptor& s) {
  if (out.size() < 4 || !mode_ok(s.clamp_x) || !mode_ok(s.clamp_y) || !mode_ok(s.clamp_z) || s.border > 2 || s.mip_filter > 2 || s.z_filter > 2) return false;
  if (!(s.min_lod >= 0.0f && s.min_lod <= 15.99f) || !(s.max_lod >= 0.0f && s.max_lod <= 15.999f) || !(s.lod_bias > -32.0f && s.lod_bias < 32.0f)) return false;
  const uint32_t mag = s.bilinear ? kSQ_TEX_XY_FILTER_BILINEAR : kSQ_TEX_XY_FILTER_POINT;
  const bool min_b = s.min_bilinear_set ? s.min_bilinear : s.bilinear;
  const uint32_t min_f = min_b ? kSQ_TEX_XY_FILTER_BILINEAR : kSQ_TEX_XY_FILTER_POINT;
  uint32_t min_lod = static_cast<uint32_t>(s.min_lod * 256.0f + 0.5f), max_lod = static_cast<uint32_t>(s.max_lod * 256.0f + 0.5f);
  if (min_lod > kLodMax) min_lod = kLodMax;
  if (max_lod > kLodMax) max_lod = kLodMax;
  const int32_t bias = static_cast<int32_t>(s.lod_bias * 256.0f + (s.lod_bias < 0 ? -0.5f : 0.5f));
  out[0] = (static_cast<uint32_t>(s.clamp_x) << SQ_IMG_SAMP_WORD0__CLAMP_X__SHIFT) | (static_cast<uint32_t>(s.clamp_y) << SQ_IMG_SAMP_WORD0__CLAMP_Y__SHIFT) |
           (static_cast<uint32_t>(s.clamp_z) << SQ_IMG_SAMP_WORD0__CLAMP_Z__SHIFT) | ((s.unnormalized ? 1u : 0u) << SQ_IMG_SAMP_WORD0__FORCE_UNNORMALIZED__SHIFT) |
           ((s.disable_cube_wrap ? 1u : 0u) << SQ_IMG_SAMP_WORD0__DISABLE_CUBE_WRAP__SHIFT);
  out[1] = (min_lod << SQ_IMG_SAMP_WORD1__MIN_LOD__SHIFT) | (max_lod << SQ_IMG_SAMP_WORD1__MAX_LOD__SHIFT);
  out[2] = (static_cast<uint32_t>(bias) & 0x3FFF) | (mag << SQ_IMG_SAMP_WORD2__XY_MAG_FILTER__SHIFT) | (min_f << SQ_IMG_SAMP_WORD2__XY_MIN_FILTER__SHIFT) |
           (static_cast<uint32_t>(s.z_filter) << SQ_IMG_SAMP_WORD2__Z_FILTER__SHIFT) | (static_cast<uint32_t>(s.mip_filter) << SQ_IMG_SAMP_WORD2__MIP_FILTER__SHIFT);
  out[3] = static_cast<uint32_t>(s.border) << SQ_IMG_SAMP_WORD3__BORDER_COLOR_TYPE__SHIFT;
  return true;
}

bool decode_sampler_descriptor(ArrayView<const uint32_t> w, SamplerDescriptor* s) {
  if (w.size() < 4) return false;
  s->clamp_x = w[0] & 7;
  s->clamp_y = (w[0] >> SQ_IMG_SAMP_WORD0__CLAMP_Y__SHIFT) & 7;
  s->clamp_z = (w[0] >> SQ_IMG_SAMP_WORD0__CLAMP_Z__SHIFT) & 7;
  s->unnormalized = (w[0] >> SQ_IMG_SAMP_WORD0__FORCE_UNNORMALIZED__SHIFT & 1) != 0;
  s->disable_cube_wrap = (w[0] >> SQ_IMG_SAMP_WORD0__DISABLE_CUBE_WRAP__SHIFT & 1) != 0;
  const uint32_t mag = (w[2] >> SQ_IMG_SAMP_WORD2__XY_MAG_FILTER__SHIFT) & 3, min_f = (w[2] >> SQ_IMG_SAMP_WORD2__XY_MIN_FILTER__SHIFT) & 3;
  s->bilinear = mag == kSQ_TEX_XY_FILTER_BILINEAR;
  s->min_bilinear_set = min_f != mag;
  s->min_bilinear = min_f == kSQ_TEX_XY_FILTER_BILINEAR;
  s->z_filter = (w[2] >> SQ_IMG_SAMP_WORD2__Z_FILTER__SHIFT) & 3;
  s->mip_filter = (w[2] >> SQ_IMG_SAMP_WORD2__MIP_FILTER__SHIFT) & 3;
  s->border = static_cast<uint8_t>(w[3] >> SQ_IMG_SAMP_WORD3__BORDER_COLOR_TYPE__SHIFT);
  s->min_lod = static_cast<float>((w[1] >> SQ_IMG_SAMP_WORD1__MIN_LOD__SHIFT) & 0xFFF) / 256.0f;
  s->max_lod = static_cast<float>((w[1] >> SQ_IMG_SAMP_WORD1__MAX_LOD__SHIFT) & 0xFFF) / 256.0f;
  int32_t bias = static_cast<int32_t>(w[2] & 0x3FFF);
  if (bias & 0x2000) bias -= 0x4000;
  s->lod_bias = static_cast<float>(bias) / 256.0f;
  // anisotropy, depth compare, degamma, the filter mode, the secondary bias and the performance fields are as make_sampler_descriptor leaves them
  const uint32_t used0 = (7u << SQ_IMG_SAMP_WORD0__CLAMP_X__SHIFT) | (7u << SQ_IMG_SAMP_WORD0__CLAMP_Y__SHIFT) | (7u << SQ_IMG_SAMP_WORD0__CLAMP_Z__SHIFT) |
                         (1u << SQ_IMG_SAMP_WORD0__FORCE_UNNORMALIZED__SHIFT) | (1u << SQ_IMG_SAMP_WORD0__DISABLE_CUBE_WRAP__SHIFT);
  const uint32_t used2 = 0x3FFFu | (3u << SQ_IMG_SAMP_WORD2__XY_MAG_FILTER__SHIFT) | (3u << SQ_IMG_SAMP_WORD2__XY_MIN_FILTER__SHIFT) | (3u << SQ_IMG_SAMP_WORD2__Z_FILTER__SHIFT) |
                         (3u << SQ_IMG_SAMP_WORD2__MIP_FILTER__SHIFT);
  return mode_ok(s->clamp_x) && mode_ok(s->clamp_y) && mode_ok(s->clamp_z) && (mag == kSQ_TEX_XY_FILTER_POINT || mag == kSQ_TEX_XY_FILTER_BILINEAR) &&
         (min_f == kSQ_TEX_XY_FILTER_POINT || min_f == kSQ_TEX_XY_FILTER_BILINEAR) && (w[0] & ~used0) == 0 && (w[2] & ~used2) == 0 && s->border <= 2 && (w[3] & 0x3FFFFFFFu) == 0 &&
         (w[1] >> 24) == 0 && s->mip_filter <= 2 && s->z_filter <= 2;
}

namespace {

SampleStatus fetch(ITexelMemory& memory, const ImageDescriptor& d, const LevelLayout& level, uint32_t x, uint32_t y, uint32_t slice, float rgba[4]) {
  const uint64_t at = d.address + image_texel_offset(d, level, x, y, slice);
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

struct Lookup {
  ITexelMemory& memory;
  const ImageDescriptor& d;
  float border[4];
  SampleStatus status = SampleStatus::Ok;
  // one texel of a plane (slice < 0: the border)
  void tap(const LevelLayout& level, int64_t ix, int64_t iy, int64_t slice, uint32_t mode_x, uint32_t mode_y, float rgba[4]) {
    int64_t x = 0, y = 0;
    if (slice < 0 || !address(mode_x, ix, level.width, &x) || !address(mode_y, iy, level.height, &y)) {
      for (uint32_t c = 0; c < 4; ++c) rgba[c] = border[c];
      return;
    }
    const SampleStatus st = fetch(memory, d, level, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(slice), rgba);
    if (st != SampleStatus::Ok) status = st;
  }
  // a filtered lookup in one plane at (fu, fv) texels of the level
  void plane(const LevelLayout& level, float fu, float fv, int64_t slice, bool bilinear, uint32_t mode_x, uint32_t mode_y, float rgba[4]) {
    if (!bilinear) {
      tap(level, floor_i(fu), floor_i(fv), slice, mode_x, mode_y, rgba);
      return;
    }
    const float sx = fu - 0.5f, sy = fv - 0.5f;
    const int64_t x0 = floor_i(sx), y0 = floor_i(sy);
    const float tx = sx - static_cast<float>(x0), ty = sy - static_cast<float>(y0);
    float t[4][4];
    tap(level, x0, y0, slice, mode_x, mode_y, t[0]);
    tap(level, x0 + 1, y0, slice, mode_x, mode_y, t[1]);
    tap(level, x0, y0 + 1, slice, mode_x, mode_y, t[2]);
    tap(level, x0 + 1, y0 + 1, slice, mode_x, mode_y, t[3]);
    for (uint32_t k = 0; k < 4; ++k) {
      const float top = t[0][k] + (t[1][k] - t[0][k]) * tx, bottom = t[2][k] + (t[3][k] - t[2][k]) * tx;
      rgba[k] = top + (bottom - top) * ty;
    }
  }
};

}  // namespace

SampleStatus sample_image(ITexelMemory& memory, ArrayView<const uint32_t> image, ArrayView<const uint32_t> sampler, const SampleRequest& r, float out[4]) {
  ImageDescriptor d;
  SamplerDescriptor s;
  if (!decode_image_descriptor(image, &d) || !decode_sampler_descriptor(sampler, &s)) return SampleStatus::Unsupported;
  const bool cube = d.type == ImageType::Cube;
  if (cube && !s.disable_cube_wrap && (s.bilinear || (s.min_bilinear_set && s.min_bilinear))) return SampleStatus::Unsupported;       // (filtering across the faces is not done)
  if (s.unnormalized && (d.levels > 1 || d.type != ImageType::Image2D)) return SampleStatus::Unsupported;
  static const float kBorders[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
  Lookup lookup{memory, d, {kBorders[s.border][0], kBorders[s.border][1], kBorders[s.border][2], kBorders[s.border][3]}};

  // the level of detail and the levels it picks
  float lod = (r.has_lod ? r.lod : 0.0f) + r.bias + s.lod_bias;
  if (!(lod == lod)) lod = 0.0f;
  if (lod < s.min_lod) lod = s.min_lod;
  if (lod > s.max_lod) lod = s.max_lod;
  const uint32_t last = d.levels - 1;
  const bool magnify = lod <= 0.0f;
  const bool bilinear = magnify ? s.bilinear : (s.min_bilinear_set ? s.min_bilinear : s.bilinear);
  uint32_t l0 = 0, l1 = 0;
  float blend = 0.0f;
  if (d.levels > 1 && s.mip_filter != 0) {
    if (s.mip_filter == 1) {
      const int64_t p = floor_i(lod + 0.5f);
      l0 = l1 = p < 0 ? 0 : p > static_cast<int64_t>(last) ? last : static_cast<uint32_t>(p);
    } else {
      const int64_t p = floor_i(lod);
      l0 = p < 0 ? 0 : p > static_cast<int64_t>(last) ? last : static_cast<uint32_t>(p);
      l1 = l0 < last ? l0 + 1 : l0;
      blend = l1 == l0 ? 0.0f : lod - static_cast<float>(l0);
    }
  }

  float texel[4] = {0, 0, 0, 0};
  const uint32_t levels_used = l1 != l0 ? 2 : 1;
  for (uint32_t pass = 0; pass < levels_used; ++pass) {
    LevelLayout level;
    if (!image_level(d, pass == 0 ? l0 : l1, &level)) return SampleStatus::Unsupported;
    float c[4] = {0, 0, 0, 0};
    const float u = cube ? r.coord[0] - 1.0f : r.coord[0], v = cube ? r.coord[1] - 1.0f : r.coord[1];
    const float fu = s.unnormalized ? u : u * static_cast<float>(level.width), fv = s.unnormalized ? v : v * static_cast<float>(level.height);
    const uint32_t mode_x = cube ? kSQ_TEX_CLAMP_LAST_TEXEL : s.clamp_x, mode_y = cube ? kSQ_TEX_CLAMP_LAST_TEXEL : s.clamp_y;
    if (d.type == ImageType::Image3D) {
      const float fw = r.coord[2] * static_cast<float>(level.depth);
      if (s.z_filter == 2) {
        const float sz = fw - 0.5f;
        const int64_t z0 = floor_i(sz);
        const float tz = sz - static_cast<float>(z0);
        float a[4], b[4];
        int64_t za = 0, zb = 0;
        lookup.plane(level, fu, fv, address(s.clamp_z, z0, level.depth, &za) ? za : -1, bilinear, mode_x, mode_y, a);
        lookup.plane(level, fu, fv, address(s.clamp_z, z0 + 1, level.depth, &zb) ? zb : -1, bilinear, mode_x, mode_y, b);
        for (uint32_t k = 0; k < 4; ++k) c[k] = a[k] + (b[k] - a[k]) * tz;
      } else {
        int64_t z = 0;
        lookup.plane(level, fu, fv, address(s.clamp_z, floor_i(fw), level.depth, &z) ? z : -1, bilinear, mode_x, mode_y, c);
      }
    } else {
      int64_t slice = 0;
      if (cube || d.type == ImageType::Array2D) {
        slice = floor_i(r.coord[2] + 0.5f);
        slice = slice < 0 ? 0 : slice >= static_cast<int64_t>(d.depth) ? d.depth - 1 : slice;
      }
      lookup.plane(level, fu, fv, slice, bilinear, mode_x, mode_y, c);
    }
    if (lookup.status != SampleStatus::Ok) return lookup.status;
    for (uint32_t k = 0; k < 4; ++k) texel[k] = pass == 0 ? c[k] : texel[k] + (c[k] - texel[k]) * blend;
  }
  for (uint32_t k = 0; k < 4; ++k) out[k] = d.dst_sel[k] == 0 ? 0.0f : d.dst_sel[k] == 1 ? 1.0f : texel[d.dst_sel[k] - kDstSelX];
  return SampleStatus::Ok;
}

SampleStatus sample_image(ITexelMemory& memory, ArrayView<const uint32_t> image, ArrayView<const uint32_t> sampler, float u, float v, float out[4]) {
  SampleRequest r;
  r.coord[0] = u;
  r.coord[1] = v;
  return sample_image(memory, image, sampler, r, out);
}

}  // namespace gcn
