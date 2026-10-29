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


// Surface layouts (linear, 1D tiled) and the image and sampler descriptors with the sampling the texture unit does: every pixel of a layout has its own place, the descriptors are
// the words the kernel headers' field positions give, the address modes and filters give the values worked out by hand, a tiled image reads like the same linear one, and
// descriptors with a bit flipped are refused or sampled without leaving the memory.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTexture.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"

using namespace gcn;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// a flat block of memory at a base address
struct Block final : ITexelMemory {
  uint64_t base = 0x40000;
  std::vector<uint32_t> words = std::vector<uint32_t>(1 << 17);
  uint32_t reads = 0;
  bool read32(uint64_t address, uint32_t* out) override {
    if (address < base || (address & 3) != 0 || (address - base) / 4 >= words.size()) return false;
    ++reads;
    *out = words[(address - base) / 4];
    return true;
  }
  void put(const TileLayout& l, TexelFormat f, uint32_t pitch, uint32_t x, uint32_t y, const uint8_t* texel, uint32_t height = 0) {
    const uint32_t bytes = texel_bytes(f);
    const uint64_t off = surface_offset(l, bytes, pitch, x, y, height);
    for (uint32_t k = 0; k < bytes; ++k) reinterpret_cast<uint8_t*>(words.data())[off + k] = texel[k];
  }
};

static void test_layouts() {
  const uint32_t sizes[] = {1, 2, 4, 8, 16};
  const TileLayout::Micro micros[] = {TileLayout::Micro::Display, TileLayout::Micro::Thin, TileLayout::Micro::Depth};
  for (const uint32_t bytes : sizes)
    for (const TileLayout::Micro micro : micros) {
      TileLayout l;
      l.kind = TileLayout::Kind::Tiled1D;
      l.micro = micro;
      const uint32_t pitch = 24, height = 16;
      // every pixel of the padded surface has its own slot, all of the surface's bytes are used, and a micro tile is 64 consecutive pixels
      bool seen[24 * 16] = {};
      bool ok = true;
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < pitch; ++x) {
          const uint64_t off = surface_offset(l, bytes, pitch, x, y);
          const uint64_t slot = off / bytes;
          if (off % bytes != 0 || slot >= 24 * 16 || seen[slot]) { ok = false; continue; }
          seen[slot] = true;
          const uint64_t tile = static_cast<uint64_t>(y / 8) * (pitch / 8) + x / 8;
          if (slot / 64 != tile) ok = false;
        }
      CHECK(ok);
      CHECK(surface_bytes(l, bytes, pitch, height) == 24ull * 16 * bytes);
    }
  // the 32 bit display order is x0 x1 y0 x2 y1 y2
  TileLayout d;
  d.kind = TileLayout::Kind::Tiled1D;
  CHECK(surface_offset(d, 4, 8, 1, 0) == 4 && surface_offset(d, 4, 8, 2, 0) == 4 * 2 && surface_offset(d, 4, 8, 0, 1) == 4 * 4 && surface_offset(d, 4, 8, 4, 0) == 4 * 8 &&
        surface_offset(d, 4, 8, 0, 2) == 4 * 16 && surface_offset(d, 4, 8, 0, 4) == 4 * 32);
  // and the z order is x0 y0 x1 y1 x2 y2
  d.micro = TileLayout::Micro::Thin;
  CHECK(surface_offset(d, 4, 8, 1, 0) == 4 && surface_offset(d, 4, 8, 0, 1) == 4 * 2 && surface_offset(d, 4, 8, 2, 0) == 4 * 4 && surface_offset(d, 4, 8, 0, 2) == 4 * 8);
  // the second tile of the row follows the first, the second row of tiles follows the row
  CHECK(surface_offset(d, 4, 16, 8, 0) == 64 * 4 && surface_offset(d, 4, 16, 0, 8) == 2 * 64 * 4);
  TileLayout lin;
  CHECK(surface_offset(lin, 4, 10, 3, 2) == (2 * 10 + 3) * 4);
  CHECK(!surface_valid(d, 4, 12) && surface_valid(lin, 4, 12) && !surface_valid(lin, 3, 12) && !surface_valid(lin, 4, 0));
  // the golden table: 8 is linear aligned, 9 and 13 are 1D tiled (display and thin), the depth entries are 2D tiled and refused
  TileLayout r;
  CHECK(resolve_tile_index(8, &r) && r.kind == TileLayout::Kind::Linear);
  CHECK(resolve_tile_index(9, &r) && r.kind == TileLayout::Kind::Tiled1D && r.micro == TileLayout::Micro::Display);
  CHECK(resolve_tile_index(13, &r) && r.kind == TileLayout::Kind::Tiled1D && r.micro == TileLayout::Micro::Thin);
  CHECK(resolve_tile_index(10, &r) && r.kind == TileLayout::Kind::Tiled2D && r.micro == TileLayout::Micro::Display);       // 2D tiled, display
  CHECK(resolve_tile_index(2, &r) && r.kind == TileLayout::Kind::Tiled2D && r.micro == TileLayout::Micro::Depth);          // 2D tiled, depth
  CHECK(!resolve_tile_index(18, &r) && !resolve_tile_index(31, &r) && !resolve_tile_index(40, &r));                        // 1D thick, the unused entry, beyond the table
}

static uint64_t align_to(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// 2D tiled layouts: the alignments the tile mode table gives, every pixel of the padded surface in its own place, and the offsets inside a macro tile
static void test_2d_layouts() {
  TileLayout l;
  CHECK(resolve_tile_index(10, &l));
  // entry 10 (2D, display, P16_32x32_16x16): 16 byte pixels fill a 1 KB micro tile, macro tile mode 4 (bank width 1, height 2, aspect 1, 8 banks): 8 * 1 * 16 * 1 pixels wide, 8 * 2 * 8 high;
  // 4 byte pixels a 256 byte tile, macro tile mode 2 (width 1, height 4, aspect 2, 8 banks): 256 by 128
  CHECK(surface_pitch_align(l, 16) == 128 && surface_height_align(l, 16) == 128);
  CHECK(surface_pitch_align(l, 4) == 256 && surface_height_align(l, 4) == 128);
  CHECK(surface_base_align(l, 4) == 16ull * 1 * 8 * 4 * 256);
  CHECK(surface_valid(l, 4, 256) && !surface_valid(l, 4, 128) && !surface_valid(l, 4, 264));
  const uint32_t entries[] = {0, 1, 2, 3, 10, 14};          // (11 and 16 are PRT modes: refused)
  const uint32_t sizes[] = {1, 4, 8, 16};
  for (const uint32_t entry : entries)
    for (const uint32_t bytes : sizes) {
      TileLayout t;
      if (!resolve_tile_index(entry, &t) || t.kind != TileLayout::Kind::Tiled2D) { CHECK(false); continue; }
      const uint32_t pitch = surface_pitch_align(t, bytes) * 2, height = surface_height_align(t, bytes) * 2;
      const uint64_t total = surface_bytes(t, bytes, pitch, height);
      CHECK(total == static_cast<uint64_t>(pitch) * height * bytes);
      std::vector<uint8_t> seen(total / bytes);
      uint32_t bad = 0;
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < pitch; ++x) {
          const uint64_t off = surface_offset(t, bytes, pitch, x, y, height);
          if (off % bytes != 0 || off / bytes >= seen.size() || seen[off / bytes]) { ++bad; continue; }
          seen[off / bytes] = 1;
        }
      CHECK(bad == 0);
    }
  // the 64 pixels of a micro tile are consecutive bytes' worth (a micro tile of the 4 byte layout is 256 bytes in one place)
  TileLayout d;
  resolve_tile_index(10, &d);
  uint64_t lo = ~0ull, hi = 0;
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x) {
      const uint64_t off = surface_offset(d, 4, 256, x, y, 128);
      if (off < lo) lo = off;
      if (off > hi) hi = off;
    }
  CHECK(hi - lo == 252 && lo % 256 == 0);
}

// The vectors computed by AMD's address library (scripts/gen_tiling_vectors.cpp), when the file is there
static void test_reference_vectors() {
  FILE* f = fopen("tests/data/gcn_tiling_vectors.txt", "r");
  if (!f) { printf("  (no reference vectors: tests/data/gcn_tiling_vectors.txt)\n"); return; }
  char line[256];
  uint32_t infos = 0, addrs = 0, wrong = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned idx, bpp, w, h, a, b;
    unsigned long long c, d;
    TileLayout t;
    if (sscanf(line, "info %u %u %u %u %u %u %llu %llu", &idx, &bpp, &w, &h, &a, &b, &c, &d) == 8) {
      ++infos;
      const uint32_t bytes = bpp / 8;
      if (!resolve_tile_index(idx, &t) || align_to(w, surface_pitch_align(t, bytes)) != a || align_to(h, surface_height_align(t, bytes)) != b ||
          surface_bytes(t, bytes, a, h) != c || surface_base_align(t, bytes) != d)
        ++wrong;
    } else if (sscanf(line, "addr %u %u %u %u %u %u %llu", &idx, &bpp, &w, &h, &a, &b, &c) == 7) {
      ++addrs;
      const uint32_t bytes = bpp / 8;
      if (!resolve_tile_index(idx, &t)) { ++wrong; continue; }
      const uint32_t pitch = static_cast<uint32_t>(align_to(w, surface_pitch_align(t, bytes)));
      if (surface_offset(t, bytes, pitch, a, b, h) != c) ++wrong;
    }
  }
  fclose(f);
  CHECK(infos > 0 && addrs > 0 && wrong == 0);
  // the mip chains and slices (when the file has them): the size of every level is the library's, as is the padded pitch and height; a level starts at a multiple of 256 and of the
  // alignment the library wants
  f = fopen("tests/data/gcn_tiling_vectors.txt", "r");
  uint32_t levels_seen = 0, level_wrong = 0;
  while (f && fgets(line, sizeof line, f)) {
    unsigned idx, bpp, type, w, h, depth, levels, k, pitch, height, slices, align;
    unsigned long long size;
    if (sscanf(line, "level %u %u %u %u %u %u %u %u %u %u %u %llu %u", &idx, &bpp, &type, &w, &h, &depth, &levels, &k, &pitch, &height, &slices, &size, &align) != 13) continue;
    ++levels_seen;
    ImageDescriptor d;
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.levels = levels;
    d.tile_index = idx;
    d.type = type == 1 ? ImageType::Image3D : type == 2 ? ImageType::Cube : type == 3 ? ImageType::Array2D : ImageType::Image2D;
    d.format = bpp == 8 ? TexelFormat::R8Unorm : bpp == 32 ? TexelFormat::Rgba8Unorm : TexelFormat::Rgba32Float;
    d.pitch_pixels = image_pitch(w, levels);
    LevelLayout l;
    if (!image_level(d, k, &l) || l.pitch != pitch || l.rows != height || l.bytes() != size || (256 % align) != 0 || l.offset % 256 != 0) ++level_wrong;
    if (k == 0 && d.pitch_pixels != pitch) ++level_wrong;
  }
  if (f) fclose(f);
  if (levels_seen == 0) printf("  (no mip chain vectors in the file)\n");
  CHECK(level_wrong == 0);
}

static void test_descriptors() {
  ImageDescriptor d;
  d.address = 0xAB12345600ull;
  d.width = 37;
  d.height = 11;
  d.pitch_pixels = 40;
  d.format = TexelFormat::Rgba8Unorm;
  d.tile_index = 9;
  uint32_t w[8];
  CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d));
  // the words from the kernel's field positions (SQ_IMG_RSRC_WORD*): base >> 8, base hi | data format 10 << 20 | num format 0, width - 1 | height - 1 << 14, xyzw selects 4 5 6 7 |
  // tiling 9 << 20 | type 9 << 28, pitch - 1 << 13
  CHECK(w[0] == static_cast<uint32_t>(d.address >> 8));
  CHECK(w[1] == (static_cast<uint32_t>(d.address >> 40) | (10u << 20)));
  CHECK(w[2] == (36u | (10u << 14)));
  CHECK(w[3] == (4u | (5u << 3) | (6u << 6) | (7u << 9) | (9u << 20) | (9u << 28)));
  CHECK(w[4] == (39u << 13) && w[5] == 0 && w[6] == 0 && w[7] == 0);
  ImageDescriptor back;
  CHECK(decode_image_descriptor(ArrayView<const uint32_t>(w, 8), &back));
  CHECK(back.address == d.address && back.width == 37 && back.height == 11 && back.pitch_pixels == 40 && back.format == d.format && back.tile_index == 9 && back.dst_sel[3] == 7);
  d.format = TexelFormat::Rgba32Float;
  d.tile_index = 8;
  CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d) && w[1] == (static_cast<uint32_t>(d.address >> 40) | (14u << 20) | (7u << 26)));
  d.format = TexelFormat::R8Unorm;
  CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d) && ((w[1] >> 20) & 0x3F) == 1);
  // refused: a misaligned address, no size, a pitch below the width, a tiled image whose pitch is not a multiple of 8, a tile mode that is not implemented, a bad select
  ImageDescriptor bad = d;
  bad.address += 4;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.width = 0;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.pitch_pixels = 36;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.tile_index = 9; bad.pitch_pixels = 44;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.tile_index = 10;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.dst_sel[2] = 2;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.address = 1ull << 40;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 7), d));

  SamplerDescriptor s;
  s.clamp_x = 1;
  s.clamp_y = 6;
  s.bilinear = true;
  s.border = 2;
  uint32_t sw[4];
  CHECK(make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), s));
  CHECK(sw[0] == (1u | (6u << 3)) && sw[1] == (0xFFFu << 12) && sw[2] == ((1u << 20) | (1u << 22)) && sw[3] == (2u << 30));
  SamplerDescriptor sb;
  CHECK(decode_sampler_descriptor(ArrayView<const uint32_t>(sw, 4), &sb) && sb.clamp_x == 1 && sb.clamp_y == 6 && sb.bilinear && sb.border == 2 && !sb.unnormalized);
  SamplerDescriptor badS = s;
  badS.clamp_x = 4;
  CHECK(!make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), badS));
  badS = s; badS.border = 3;
  CHECK(!make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), badS));
}

static ImageDescriptor desc(const Block& b, uint32_t w, uint32_t h, uint32_t pitch, TexelFormat f, uint32_t tile) {
  ImageDescriptor d;
  d.address = b.base;
  d.width = w;
  d.height = h;
  d.pitch_pixels = pitch;
  d.format = f;
  d.tile_index = tile;
  return d;
}

static SampleStatus run(Block& b, const ImageDescriptor& d, const SamplerDescriptor& s, float u, float v, float out[4]) {
  uint32_t iw[8], sw[4];
  if (!make_image_descriptor(ArrayView<uint32_t>(iw, 8), d) || !make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), s)) return SampleStatus::Unsupported;
  return sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(sw, 4), u, v, out);
}

static bool near4(const float a[4], float r, float g, float bl, float al) {
  return fabsf(a[0] - r) < 1e-5f && fabsf(a[1] - g) < 1e-5f && fabsf(a[2] - bl) < 1e-5f && fabsf(a[3] - al) < 1e-5f;
}

// a 4 by 2 image: texel (x, y) is (x * 40, y * 100, 7, 255) in bytes
static void fill_rgba8(Block& b, const TileLayout& l, uint32_t pitch) {
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      const uint8_t t[4] = {static_cast<uint8_t>(x * 40), static_cast<uint8_t>(y * 100), 7, 255};
      b.put(l, TexelFormat::Rgba8Unorm, pitch, x, y, t);
    }
}

static void test_sampling() {
  Block b;
  TileLayout lin;
  fill_rgba8(b, lin, 8);
  const ImageDescriptor d = desc(b, 4, 2, 8, TexelFormat::Rgba8Unorm, 8);
  SamplerDescriptor point;
  float o[4];
  // point sampling: the texel the coordinate is in
  CHECK(run(b, d, point, 0.125f, 0.25f, o) == SampleStatus::Ok && near4(o, 0, 0, 7 / 255.0f, 1));
  CHECK(run(b, d, point, 0.6f, 0.75f, o) == SampleStatus::Ok && near4(o, 80 / 255.0f, 100 / 255.0f, 7 / 255.0f, 1));
  // address modes outside [0, 1)
  SamplerDescriptor wrap;
  CHECK(run(b, d, wrap, 1.125f, 0.25f, o) == SampleStatus::Ok && near4(o, 0, 0, 7 / 255.0f, 1));            // wraps to texel 0
  CHECK(run(b, d, wrap, -0.125f, 0.25f, o) == SampleStatus::Ok && near4(o, 120 / 255.0f, 0, 7 / 255.0f, 1));   // to texel 3
  SamplerDescriptor mirror;
  mirror.clamp_x = mirror.clamp_y = 1;
  CHECK(run(b, d, mirror, 1.125f, 0.25f, o) == SampleStatus::Ok && near4(o, 120 / 255.0f, 0, 7 / 255.0f, 1));   // the texel 4 mirrors to 3
  CHECK(run(b, d, mirror, -0.125f, 0.25f, o) == SampleStatus::Ok && near4(o, 0, 0, 7 / 255.0f, 1));            // -1 mirrors to 0
  SamplerDescriptor clamp;
  clamp.clamp_x = clamp.clamp_y = 2;
  CHECK(run(b, d, clamp, 5.0f, -3.0f, o) == SampleStatus::Ok && near4(o, 120 / 255.0f, 0, 7 / 255.0f, 1));
  SamplerDescriptor border;
  border.clamp_x = border.clamp_y = 6;
  CHECK(run(b, d, border, 1.5f, 0.25f, o) == SampleStatus::Ok && near4(o, 0, 0, 0, 0));
  border.border = 2;
  CHECK(run(b, d, border, 1.5f, 0.25f, o) == SampleStatus::Ok && near4(o, 1, 1, 1, 1));
  border.border = 1;
  CHECK(run(b, d, border, 0.5f, -0.5f, o) == SampleStatus::Ok && near4(o, 0, 0, 0, 1));
  CHECK(run(b, d, border, 0.5f, 0.25f, o) == SampleStatus::Ok && near4(o, 80 / 255.0f, 0, 7 / 255.0f, 1));      // inside: the image
  // unnormalized coordinates are in texels
  SamplerDescriptor un;
  un.unnormalized = true;
  CHECK(run(b, d, un, 2.5f, 1.5f, o) == SampleStatus::Ok && near4(o, 80 / 255.0f, 100 / 255.0f, 7 / 255.0f, 1));
  // bilinear: at the centre of a texel the texel, between two the mean, at the corner of four the mean of four
  SamplerDescriptor bi;
  bi.bilinear = true;
  bi.clamp_x = bi.clamp_y = 2;
  CHECK(run(b, d, bi, 1.5f / 4, 0.25f, o) == SampleStatus::Ok && near4(o, 40 / 255.0f, 0, 7 / 255.0f, 1));
  CHECK(run(b, d, bi, 2.0f / 4, 0.25f, o) == SampleStatus::Ok && near4(o, 60 / 255.0f, 0, 7 / 255.0f, 1));
  CHECK(run(b, d, bi, 2.0f / 4, 0.5f, o) == SampleStatus::Ok && near4(o, 60 / 255.0f, 50 / 255.0f, 7 / 255.0f, 1));
  CHECK(run(b, d, bi, 2.0f / 4 + 1.0f / 16, 0.25f, o) == SampleStatus::Ok && near4(o, (60 + 10) / 255.0f, 0, 7 / 255.0f, 1));      // a quarter of a texel on
  // with the border mode the taps beyond the edge are the border
  SamplerDescriptor bb = bi;
  bb.clamp_x = bb.clamp_y = 6;
  bb.border = 2;
  CHECK(run(b, d, bb, 0.0f, 0.25f, o) == SampleStatus::Ok && near4(o, 0.5f, 0.5f, (7 / 255.0f + 1) / 2, 1));
  // the component selects: swap x and z, a constant one and zero
  ImageDescriptor sw = d;
  sw.dst_sel[0] = 6; sw.dst_sel[1] = 0; sw.dst_sel[2] = 4; sw.dst_sel[3] = 1;
  CHECK(run(b, sw, point, 0.6f, 0.75f, o) == SampleStatus::Ok && near4(o, 7 / 255.0f, 0, 80 / 255.0f, 1));
}

static void test_formats_and_tiling() {
  // R8: the byte, then 0, 0, 1
  Block r8;
  TileLayout lin;
  const uint8_t texels[3] = {0, 128, 255};
  for (uint32_t i = 0; i < 3; ++i) r8.put(lin, TexelFormat::R8Unorm, 8, i, 0, &texels[i]);
  float o[4];
  SamplerDescriptor point;
  CHECK(run(r8, desc(r8, 3, 1, 8, TexelFormat::R8Unorm, 8), point, 0.5f, 0.5f, o) == SampleStatus::Ok && near4(o, 128 / 255.0f, 0, 0, 1));
  // float RGBA
  Block f;
  const float px[8] = {0.25f, -2.0f, 3.5f, 0.75f, 9, 8, 7, 6};
  for (uint32_t i = 0; i < 8; ++i) memcpy(&f.words[i], &px[i], 4);
  CHECK(run(f, desc(f, 2, 1, 8, TexelFormat::Rgba32Float, 8), point, 0.25f, 0.5f, o) == SampleStatus::Ok && near4(o, 0.25f, -2.0f, 3.5f, 0.75f));
  CHECK(run(f, desc(f, 2, 1, 8, TexelFormat::Rgba32Float, 8), point, 0.75f, 0.5f, o) == SampleStatus::Ok && near4(o, 9, 8, 7, 6));
  // the same image tiled (display order, index 9) reads like the linear one at every coordinate, and a pitch of 16 with a width of 11 and height 13 works
  Block lb, tb;
  TileLayout tiled;
  tiled.kind = TileLayout::Kind::Tiled1D;
  const uint32_t W = 11, H = 13, P = 16;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const uint8_t t[4] = {static_cast<uint8_t>(x * 20), static_cast<uint8_t>(y * 19), static_cast<uint8_t>(x ^ y), 200};
      lb.put(lin, TexelFormat::Rgba8Unorm, P, x, y, t);
      tb.put(tiled, TexelFormat::Rgba8Unorm, P, x, y, t);
    }
  SamplerDescriptor bi;
  bi.bilinear = true;
  bi.clamp_x = 1;
  bi.clamp_y = 2;
  uint32_t mismatches = 0, samples = 0;
  for (float v = -0.3f; v < 1.3f; v += 0.0937f)
    for (float u = -0.3f; u < 1.3f; u += 0.0711f) {
      float a[4], c[4];
      const SampleStatus sa = run(lb, desc(lb, W, H, P, TexelFormat::Rgba8Unorm, 8), bi, u, v, a);
      const SampleStatus sc = run(tb, desc(tb, W, H, P, TexelFormat::Rgba8Unorm, 9), bi, u, v, c);
      ++samples;
      if (sa != SampleStatus::Ok || sc != SampleStatus::Ok || memcmp(a, c, sizeof a) != 0) ++mismatches;
    }
  CHECK(samples > 200 && mismatches == 0);
  // and 2D tiled (entry 10: the pitch is 256 pixels for 4 byte texels, the rows 128)
  {
    TileLayout t2;
    CHECK(resolve_tile_index(10, &t2));
    Block t2b;
    const uint32_t P2 = 256;
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        const uint8_t t[4] = {static_cast<uint8_t>(x * 20), static_cast<uint8_t>(y * 19), static_cast<uint8_t>(x ^ y), 200};
        t2b.put(t2, TexelFormat::Rgba8Unorm, P2, x, y, t, H);
      }
    uint32_t bad2 = 0, n2 = 0;
    for (float v = -0.3f; v < 1.3f; v += 0.0937f)
      for (float u = -0.3f; u < 1.3f; u += 0.0711f) {
        float a[4], c[4];
        const SampleStatus sa = run(lb, desc(lb, W, H, P, TexelFormat::Rgba8Unorm, 8), bi, u, v, a);
        const SampleStatus sc = run(t2b, desc(t2b, W, H, P2, TexelFormat::Rgba8Unorm, 10), bi, u, v, c);
        ++n2;
        if (sa != SampleStatus::Ok || sc != SampleStatus::Ok || memcmp(a, c, sizeof a) != 0) ++bad2;
      }
    CHECK(n2 > 200 && bad2 == 0);
    // the base of a 2D tiled image must be aligned for the layout
    ImageDescriptor dd = desc(t2b, W, H, P2, TexelFormat::Rgba8Unorm, 10);
    uint32_t w8[8];
    CHECK(make_image_descriptor(ArrayView<uint32_t>(w8, 8), dd));
    dd.address += 256;
    CHECK(!make_image_descriptor(ArrayView<uint32_t>(w8, 8), dd));
  }
  // a texel the memory does not have is a fault, not a value
  Block small;
  small.base = 0x40000;
  ImageDescriptor far = desc(small, 4, 2, 8, TexelFormat::Rgba8Unorm, 8);
  far.address = 0x10000;
  CHECK(run(small, far, point, 0.5f, 0.5f, o) == SampleStatus::Fault);
  // not an image the unit implements
  uint32_t iw[8], sw[4];
  make_image_descriptor(ArrayView<uint32_t>(iw, 8), desc(small, 4, 2, 8, TexelFormat::Rgba8Unorm, 8));
  make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), point);
  iw[3] = (iw[3] & 0x0FFFFFFFu) | (10u << 28);                 // a 3D image
  CHECK(sample_image(small, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(sw, 4), 0.5f, 0.5f, o) == SampleStatus::Unsupported);
}

// Every bit of a descriptor flipped, and the descriptor cut short: the sampler either refuses or reads only addresses the memory has, and the sanitizers stay quiet.
static void test_bit_flips() {
  Block b;
  TileLayout lin;
  fill_rgba8(b, lin, 8);
  uint32_t iw[8], sw[4];
  SamplerDescriptor s;
  s.bilinear = true;
  make_image_descriptor(ArrayView<uint32_t>(iw, 8), desc(b, 4, 2, 8, TexelFormat::Rgba8Unorm, 8));
  make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), s);
  float o[4];
  uint32_t refused = 0, accepted = 0;
  for (uint32_t bit = 0; bit < 8 * 32; ++bit) {
    uint32_t f[8];
    memcpy(f, iw, sizeof f);
    f[bit / 32] ^= 1u << (bit % 32);
    const SampleStatus st = sample_image(b, ArrayView<const uint32_t>(f, 8), ArrayView<const uint32_t>(sw, 4), 0.37f, 0.61f, o);
    if (st == SampleStatus::Ok) ++accepted; else ++refused;
  }
  for (uint32_t bit = 0; bit < 4 * 32; ++bit) {
    uint32_t f[4];
    memcpy(f, sw, sizeof f);
    f[bit / 32] ^= 1u << (bit % 32);
    const SampleStatus st = sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(f, 4), 0.37f, 0.61f, o);
    if (st == SampleStatus::Ok) ++accepted; else ++refused;
  }
  CHECK(refused > 100);
  (void)accepted;
  for (uint32_t n = 0; n < 8; ++n) CHECK(sample_image(b, ArrayView<const uint32_t>(iw, n), ArrayView<const uint32_t>(sw, 4), 0.5f, 0.5f, o) == SampleStatus::Unsupported);
  for (uint32_t n = 0; n < 4; ++n) CHECK(sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(sw, n), 0.5f, 0.5f, o) == SampleStatus::Unsupported);
  // awkward coordinates
  const float odd[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 0.0f, -0.0f};
  for (const float u : odd)
    for (const float v : odd) CHECK(sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(sw, 4), u, v, o) != SampleStatus::Unsupported);
}

// writes `value` (red; green, blue 0, alpha 255) into every texel of every slice of a level
static void fill_level(Block& b, const ImageDescriptor& d, uint32_t level, uint8_t red) {
  LevelLayout l;
  CHECK(image_level(d, level, &l));
  for (uint32_t z = 0; z < l.depth; ++z)
    for (uint32_t y = 0; y < l.height; ++y)
      for (uint32_t x = 0; x < l.width; ++x) {
        const uint64_t off = image_texel_offset(d, l, x, y, z);
        const uint8_t t[4] = {red, 0, 0, 255};
        memcpy(reinterpret_cast<uint8_t*>(b.words.data()) + off, t, 4);
      }
}

static SampleStatus run_req(Block& b, const ImageDescriptor& d, const SamplerDescriptor& s, const SampleRequest& r, float out[4]) {
  uint32_t iw[8], sw[4];
  if (!make_image_descriptor(ArrayView<uint32_t>(iw, 8), d) || !make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), s)) return SampleStatus::Unsupported;
  return sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(sw, 4), r, out);
}

static ImageDescriptor base_image(const Block& b, uint32_t w, uint32_t h, uint32_t levels, ImageType type, uint32_t depth) {
  ImageDescriptor d;
  d.address = b.base;
  d.width = w;
  d.height = h;
  d.pitch_pixels = image_pitch(w, levels);
  d.format = TexelFormat::Rgba8Unorm;
  d.tile_index = 9;
  d.type = type;
  d.depth = depth;
  d.levels = levels;
  return d;
}

static void test_mips() {
  Block b;
  ImageDescriptor d = base_image(b, 8, 8, 4, ImageType::Image2D, 1);
  // the chain: 8 by 8, 4 by 4, 2 by 2, 1 by 1, each padded to 8 by 8 pixels (256 bytes) and started at a multiple of 256
  LevelLayout l;
  CHECK(image_level(d, 0, &l) && l.offset == 0 && l.width == 8 && l.pitch == 8 && l.rows == 8 && l.slice_bytes == 256 && l.slices == 1);
  CHECK(image_level(d, 1, &l) && l.offset == 256 && l.width == 4 && l.height == 4 && l.pitch == 8 && l.rows == 8);
  CHECK(image_level(d, 2, &l) && l.offset == 512 && l.width == 2);
  CHECK(image_level(d, 3, &l) && l.offset == 768 && l.width == 1 && l.height == 1);
  CHECK(!image_level(d, 4, &l) && image_bytes(d) == 1024);
  // a bigger one: 20 by 12 pads to 32 by 16, 16 by 8 ... (pow2 padding of the base level, shifted down and padded to 8)
  ImageDescriptor e = base_image(b, 20, 12, 5, ImageType::Image2D, 1);
  CHECK(e.pitch_pixels == 32);
  CHECK(image_level(e, 0, &l) && l.pitch == 32 && l.rows == 16 && l.slice_bytes == 32 * 16 * 4 && l.offset == 0);
  CHECK(image_level(e, 1, &l) && l.pitch == 16 && l.width == 10 && l.height == 6 && l.rows == 8 && l.offset == 2048);        // 6 pads to 8
  CHECK(image_level(e, 2, &l) && l.pitch == 8 && l.width == 5 && l.height == 3 && l.rows == 4 + 4 && l.offset == 2048 + 16 * 8 * 4);
  uint32_t w[8];
  CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d));
  CHECK(((w[3] >> 16) & 0xF) == 3 && (w[3] >> 28) == 9 && ((w[3] >> 12) & 0xF) == 0);           // LAST_LEVEL 3, BASE_LEVEL 0
  ImageDescriptor back;
  CHECK(decode_image_descriptor(ArrayView<const uint32_t>(w, 8), &back) && back.levels == 4 && back.type == ImageType::Image2D);

  // the level r is filled with red = 60 * r
  for (uint32_t k = 0; k < 4; ++k) fill_level(b, d, k, static_cast<uint8_t>(60 * k));
  SamplerDescriptor s;
  float o[4];
  SampleRequest r;
  r.coord[0] = r.coord[1] = 0.5f;
  r.has_lod = true;
  auto red = [&](const SamplerDescriptor& smp, float lod) { r.lod = lod; return run_req(b, d, smp, r, o) == SampleStatus::Ok ? o[0] * 255.0f : -1.0f; };
  auto near_to = [](float a, float want) { return a > want - 0.01f && a < want + 0.01f; };
  // no mip filter: the base level whatever the lod
  CHECK(near_to(red(s, 0), 0) && near_to(red(s, 2.5f), 0));
  s.mip_filter = 1;
  CHECK(near_to(red(s, 0), 0) && near_to(red(s, 1.4f), 60) && near_to(red(s, 1.6f), 120) && near_to(red(s, 10.0f), 180) && near_to(red(s, -3.0f), 0));
  s.mip_filter = 2;
  CHECK(near_to(red(s, 0), 0) && near_to(red(s, 1.0f), 60) && near_to(red(s, 1.5f), 90) && near_to(red(s, 2.25f), 135) && near_to(red(s, 3.0f), 180) && near_to(red(s, 9.0f), 180));
  // the level of detail is clamped to the sampler's range
  s.min_lod = 1.0f;
  CHECK(near_to(red(s, 0), 60));
  s.min_lod = 0.0f;
  s.max_lod = 2.0f;
  CHECK(near_to(red(s, 7), 120));
  s.max_lod = 4095.0f / 256.0f;
  // the sampler's bias and the instruction's bias add to the lod
  s.lod_bias = 1.0f;
  CHECK(near_to(red(s, 0), 60));
  s.lod_bias = -0.5f;
  CHECK(near_to(red(s, 2.0f), 90));
  s.lod_bias = 0.0f;
  r.has_lod = false;
  r.bias = 2.0f;
  r.lod = 0.0f;
  CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 120));
  // the minify filter applies when the lod is above 0, the magnify filter at 0: a ramp in the base level, point magnify and bilinear minify
  {
    for (uint32_t x = 0; x < 8; ++x)
      for (uint32_t y = 0; y < 8; ++y) {
        LevelLayout l0;
        image_level(d, 0, &l0);
        const uint8_t t[4] = {static_cast<uint8_t>(x * 30), 0, 0, 255};
        memcpy(reinterpret_cast<uint8_t*>(b.words.data()) + image_texel_offset(d, l0, x, y, 0), t, 4);
      }
    SamplerDescriptor f;
    f.mip_filter = 1;
    f.bilinear = false;
    f.min_bilinear_set = true;
    f.min_bilinear = true;
    SampleRequest q;
    q.coord[0] = 0.5f;
    q.coord[1] = 0.5f;
    q.has_lod = true;
    q.lod = 0.0f;
    CHECK(run_req(b, d, f, q, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 120));             // texel 4 of the ramp, nearest
    f.mip_filter = 0;
    f.min_bilinear = true;
    q.lod = 0.4f;
    CHECK(run_req(b, d, f, q, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 105));             // bilinear at the edge of texels 3 and 4: (90 + 120) / 2
  }
  // an unnormalized lookup has no levels
  SamplerDescriptor un;
  un.unnormalized = true;
  CHECK(run_req(b, d, un, r, o) == SampleStatus::Unsupported);
  // refusals: more levels than the size has, the levels in a layout that has none, a pitch that is not the padded one
  ImageDescriptor bad = d;
  bad.levels = 5;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.tile_index = 8;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = d; bad.tile_index = 10;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  bad = e; bad.pitch_pixels = 24;
  CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
}

static void test_slices() {
  Block b;
  float o[4];
  auto near_to = [](float a, float want) { return a > want - 0.01f && a < want + 0.01f; };
  // an array of 3 layers (4 by 4), red = 80 * layer
  {
    ImageDescriptor d = base_image(b, 4, 4, 1, ImageType::Array2D, 3);
    uint32_t w[8];
    CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d));
    CHECK((w[3] >> 28) == 13 && (w[4] & 0x1FFF) == 2 && (w[5] >> 13) == 2 && (w[5] & 0x1FFF) == 0);
    LevelLayout l;
    CHECK(image_level(d, 0, &l) && l.slices == 3 && l.slice_bytes == 8 * 8 * 4 && image_bytes(d) == 3 * 256);
    for (uint32_t z = 0; z < 3; ++z) {
      const uint8_t t[4] = {static_cast<uint8_t>(80 * z), 0, 0, 255};
      for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) memcpy(reinterpret_cast<uint8_t*>(b.words.data()) + image_texel_offset(d, l, x, y, z), t, 4);
    }
    SamplerDescriptor s;
    SampleRequest r;
    r.coord[0] = r.coord[1] = 0.3f;
    const float layers[] = {0, 1, 2, 1.4f, 1.6f, -3, 99};
    const float want[] = {0, 80, 160, 80, 160, 0, 160};
    for (int k = 0; k < 7; ++k) {
      r.coord[2] = layers[k];
      CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, want[k]));
    }
  }
  // a volume of 4 slices (4 by 4), red = 60 * z; nearest, trilinear and the z address modes
  {
    ImageDescriptor d = base_image(b, 4, 4, 1, ImageType::Image3D, 4);
    uint32_t w[8];
    CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d) && (w[3] >> 28) == 10 && (w[4] & 0x1FFF) == 3 && w[5] == 0);
    LevelLayout l;
    CHECK(image_level(d, 0, &l) && l.slices == 4 && l.depth == 4);
    for (uint32_t z = 0; z < 4; ++z) {
      const uint8_t t[4] = {static_cast<uint8_t>(60 * z), 0, 0, 255};
      for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) memcpy(reinterpret_cast<uint8_t*>(b.words.data()) + image_texel_offset(d, l, x, y, z), t, 4);
    }
    SampleRequest r;
    r.coord[0] = r.coord[1] = 0.4f;
    SamplerDescriptor s;
    r.coord[2] = 0.5f;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 120));                    // slice floor(2.0) = 2
    r.coord[2] = 0.9f;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 180));
    s.z_filter = 2;
    r.coord[2] = 0.5f;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 90));                     // between slices 1 and 2
    r.coord[2] = 0.0f;
    s.clamp_z = 2;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 0));                      // clamped to slice 0 on both taps
    s.clamp_z = 0;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 90));                     // wrapped: slices 3 and 0 (mean of 180 and 0)
    s.clamp_z = 6;
    s.border = 2;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[3], 1.0f) && near_to(o[0] * 255.0f, (255.0f + 0.0f) / 2));        // the slice before 0 is the (opaque white) border
    // 3D with mips: a volume 4 by 4 by 4 with 3 levels: the slices of the levels count 4, 2, 1
    ImageDescriptor m = base_image(b, 4, 4, 3, ImageType::Image3D, 4);
    LevelLayout l0, l1, l2;
    CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), m) && image_level(m, 0, &l0) && image_level(m, 1, &l1) && image_level(m, 2, &l2));
    CHECK(l0.slices == 4 && l1.slices == 2 && l1.depth == 2 && l2.slices == 1 && l1.offset == 1024 && l2.offset == 1024 + 2 * 256);
  }
  // a cube of 4 by 4 faces, red = 40 * face; the face coordinates are (s / |ma| + 1.5) and the face is the third coordinate
  {
    ImageDescriptor d = base_image(b, 4, 4, 1, ImageType::Cube, 6);
    uint32_t w[8];
    CHECK(make_image_descriptor(ArrayView<uint32_t>(w, 8), d) && (w[3] >> 28) == 11 && (w[4] & 0x1FFF) == 5 && (w[5] >> 13) == 5);
    LevelLayout l;
    CHECK(image_level(d, 0, &l) && l.slices == 6);
    for (uint32_t z = 0; z < 6; ++z) {
      const uint8_t t[4] = {static_cast<uint8_t>(40 * z), 0, 0, 255};
      for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) memcpy(reinterpret_cast<uint8_t*>(b.words.data()) + image_texel_offset(d, l, x, y, z), t, 4);
    }
    SamplerDescriptor s;
    SampleRequest r;
    r.coord[0] = r.coord[1] = 1.5f;
    for (uint32_t face = 0; face < 6; ++face) {
      r.coord[2] = static_cast<float>(face);
      CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok && near_to(o[0] * 255.0f, 40.0f * face));
    }
    // bilinear filtering across the edges of faces is not done: it needs DISABLE_CUBE_WRAP (clamping within the face)
    s.bilinear = true;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Unsupported);
    s.disable_cube_wrap = true;
    CHECK(run_req(b, d, s, r, o) == SampleStatus::Ok);
    // refused: not square, not 6 faces
    ImageDescriptor bad = d;
    bad.height = 2;
    CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
    bad = d; bad.depth = 5;
    CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
    // slices need the 1D tiled layout
    bad = d; bad.tile_index = 8;
    CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
    bad = base_image(b, 4, 4, 1, ImageType::Array2D, 2); bad.tile_index = 10;
    CHECK(!make_image_descriptor(ArrayView<uint32_t>(w, 8), bad));
  }
}

// every bit of descriptors of the new kinds flipped: refused, or read without leaving the memory
static void test_new_bit_flips() {
  Block b;
  const ImageDescriptor kinds[] = {base_image(b, 8, 8, 4, ImageType::Image2D, 1), base_image(b, 4, 4, 3, ImageType::Image3D, 4), base_image(b, 4, 4, 1, ImageType::Cube, 6),
                                   base_image(b, 5, 3, 2, ImageType::Array2D, 3)};
  SamplerDescriptor smp;
  smp.mip_filter = 2;
  smp.z_filter = 2;
  smp.bilinear = true;
  smp.disable_cube_wrap = true;
  uint32_t sw[4];
  CHECK(make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), smp));
  for (const ImageDescriptor& d : kinds) {
    uint32_t iw[8];
    CHECK(make_image_descriptor(ArrayView<uint32_t>(iw, 8), d));
    float o[4];
    SampleRequest r;
    r.coord[0] = r.coord[1] = r.coord[2] = 0.4f;
    r.has_lod = true;
    r.lod = 1.3f;
    for (uint32_t bit = 0; bit < 8 * 32; ++bit) {
      uint32_t f[8];
      memcpy(f, iw, sizeof f);
      f[bit / 32] ^= 1u << (bit % 32);
      (void)sample_image(b, ArrayView<const uint32_t>(f, 8), ArrayView<const uint32_t>(sw, 4), r, o);
    }
    for (uint32_t bit = 0; bit < 4 * 32; ++bit) {
      uint32_t f[4];
      memcpy(f, sw, sizeof f);
      f[bit / 32] ^= 1u << (bit % 32);
      (void)sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(f, 4), r, o);
    }
  }
  // awkward lods and coordinates
  uint32_t iw[8];
  make_image_descriptor(ArrayView<uint32_t>(iw, 8), kinds[1]);
  const float odd[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f};
  float o[4];
  for (const float a : odd) {
    SampleRequest r;
    r.coord[0] = a; r.coord[1] = a; r.coord[2] = a;
    r.has_lod = true;
    r.lod = a;
    r.bias = a;
    (void)sample_image(b, ArrayView<const uint32_t>(iw, 8), ArrayView<const uint32_t>(sw, 4), r, o);
  }
}

int main() {
  test_layouts();
  test_2d_layouts();
  test_reference_vectors();
  test_descriptors();
  test_sampling();
  test_formats_and_tiling();
  test_bit_flips();
  test_mips();
  test_slices();
  test_new_bit_flips();
  if (g_failed == 0) printf("GcnTextureTest: all passed\n");
  return g_failed == 0 ? 0 : 1;
}
