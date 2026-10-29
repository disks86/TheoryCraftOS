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
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTexture.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"

using namespace gcn;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// a flat block of memory at a base address
struct Block final : ITexelMemory {
  uint64_t base = 0x40000;
  uint32_t words[4096] = {};
  uint32_t reads = 0;
  bool read32(uint64_t address, uint32_t* out) override {
    if (address < base || (address & 3) != 0 || (address - base) / 4 >= 4096) return false;
    ++reads;
    *out = words[(address - base) / 4];
    return true;
  }
  void put(const TileLayout& l, TexelFormat f, uint32_t pitch, uint32_t x, uint32_t y, const uint8_t* texel) {
    const uint32_t bytes = texel_bytes(f);
    const uint64_t off = surface_offset(l, bytes, pitch, x, y);
    for (uint32_t k = 0; k < bytes; ++k) reinterpret_cast<uint8_t*>(words)[off + k] = texel[k];
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
      CHECK(surface_bytes(bytes, pitch, height) == 24ull * 16 * bytes);
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
  CHECK(!resolve_tile_index(0, &r) && !resolve_tile_index(10, &r) && !resolve_tile_index(31, &r) && !resolve_tile_index(40, &r));
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

int main() {
  test_layouts();
  test_descriptors();
  test_sampling();
  test_formats_and_tiling();
  test_bit_flips();
  if (g_failed == 0) printf("GcnTextureTest: all passed\n");
  return g_failed == 0 ? 0 : 1;
}
