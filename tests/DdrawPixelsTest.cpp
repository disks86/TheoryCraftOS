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
// Host-side tests of src/Windows/ddraw/ddraw_pixels.h, the formats and the blitter of ddraw.dll:
//   - the sizes and pitches of every format, the raw pixels of 1/2/4/8-bit palettes and the byte formats;
//   - colour conversions (5-6-5, 1-5-5-5, 4-4-4-4, 3-3-2, luminance, alpha, YUY2/UYVY/YV12/NV12, DXT1-5) against values worked out by hand;
//   - fills, plain copies (overlapping ones in both directions), stretching, shrinking, mirroring, turning, source and destination colour keys,
//     conversion between formats, depth blits, and the A8R8G8B8 read-out with the colour key as alpha;
//   - refusals: images whose memory is too short for their rows (every format, cut one byte short), rectangles outside, and random blits and fills of
//     random formats and sizes with exactly-sized buffers (the test runs under ASan and UBSan: any read or write outside the given bytes fails it).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "../src/Windows/ddraw/ddraw_pixels.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    ++g_checks;                                                               \
    if (!(x)) {                                                               \
      ++g_failed;                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);   \
    }                                                                         \
  } while (0)

struct Image {
  std::vector<uint8_t> bytes;
  DdImage im;
  uint32_t palette[256];
  Image(uint32_t fmt, uint32_t w, uint32_t h, uint32_t align = 4, int64_t cut = 0) {
    memset(&im, 0, sizeof im);
    im.fmt = fmt;
    im.width = w;
    im.height = h;
    im.pitch = dd_format_is_blocks(fmt) ? dd_min_pitch(fmt, w) : dd_aligned_pitch(fmt, w, align);
    const uint64_t n = dd_image_bytes(fmt, w, h, im.pitch);
    bytes.assign((size_t)(n - (uint64_t)cut), 0);
    im.data = bytes.data();
    im.size = bytes.size();
    for (int i = 0; i < 256; ++i) palette[i] = 0xFF000000u | (uint32_t)(i * 0x010101);
    im.palette = palette;
    im.palette_count = 256;
  }
};
static DdRect R(int l, int t, int r, int b) { DdRect x = {l, t, r, b}; return x; }

static void test_sizes() {
  CHECK(dd_min_pitch(DD_FMT_X8R8G8B8, 63) == 252);
  CHECK(dd_aligned_pitch(DD_FMT_X8R8G8B8, 63, 8) == 256);
  CHECK(dd_aligned_pitch(DD_FMT_X8R8G8B8, 63, 4) == 252);
  CHECK(dd_min_pitch(DD_FMT_R8G8B8, 5) == 15 && dd_aligned_pitch(DD_FMT_R8G8B8, 5, 4) == 16);
  CHECK(dd_min_pitch(DD_FMT_P1, 9) == 2 && dd_min_pitch(DD_FMT_P2, 5) == 2 && dd_min_pitch(DD_FMT_P4, 3) == 2 && dd_min_pitch(DD_FMT_P8, 3) == 3);
  CHECK(dd_min_pitch(DD_FMT_YUY2, 3) == 8);      // (pairs of pixels)
  CHECK(dd_min_pitch(DD_FMT_DXT1, 5) == 16 && dd_min_pitch(DD_FMT_DXT5, 5) == 32 && dd_rows(DD_FMT_DXT1, 5) == 2);
  CHECK(dd_image_bytes(DD_FMT_DXT1, 8, 8, dd_min_pitch(DD_FMT_DXT1, 8)) == 32);
  CHECK(dd_image_bytes(DD_FMT_YV12, 4, 4, 4) == 16 + 2 * 2 * 2);
  CHECK(dd_image_bytes(DD_FMT_NV12, 4, 4, 4) == 16 + 4 * 2);
  CHECK(dd_image_bytes(DD_FMT_X8R8G8B8, 4, 4, 15) == 0);      // (a pitch shorter than a row)
  CHECK(dd_image_bytes(DD_FMT_UNKNOWN, 4, 4, 16) == 0);
  CHECK(dd_aligned_pitch(DD_FMT_X8R8G8B8, 0x40000000u, 8) == 0);      // (a pitch beyond 31 bits)
  CHECK(dd_aligned_pitch(DD_FMT_X8R8G8B8, 4, 3) == 0);      // (an alignment that is no power of two)
}

static void test_raw_pixels() {
  Image p1(DD_FMT_P1, 16, 2), p2(DD_FMT_P2, 8, 2), p4(DD_FMT_P4, 4, 2);
  dd_put_raw(&p1.im, 0, 0, 1);
  dd_put_raw(&p1.im, 9, 0, 1);
  CHECK(p1.bytes[0] == 0x80 && p1.bytes[1] == 0x40);      // (the leftmost pixel in the high bit, as in a DIB)
  CHECK(dd_get_raw(&p1.im, 9, 0) == 1 && dd_get_raw(&p1.im, 8, 0) == 0);
  dd_put_raw(&p2.im, 1, 0, 3);
  CHECK(p2.bytes[0] == 0x30 && dd_get_raw(&p2.im, 1, 0) == 3);
  dd_put_raw(&p4.im, 1, 1, 0xA);
  CHECK(p4.bytes[p4.im.pitch] == 0x0A && dd_get_raw(&p4.im, 1, 1) == 0xA);
  Image rgb24(DD_FMT_R8G8B8, 2, 1);
  dd_put_raw(&rgb24.im, 1, 0, 0x123456);
  CHECK(rgb24.bytes[3] == 0x56 && rgb24.bytes[4] == 0x34 && rgb24.bytes[5] == 0x12 && dd_get_raw(&rgb24.im, 1, 0) == 0x123456);
}

static void test_colours() {
  Image a(DD_FMT_R5G6B5, 1, 1);
  CHECK(dd_raw_to_argb(&a.im, 0xF800) == 0xFFFF0000u && dd_raw_to_argb(&a.im, 0x07E0) == 0xFF00FF00u && dd_raw_to_argb(&a.im, 0x001F) == 0xFF0000FFu);
  CHECK(dd_argb_to_raw(&a.im, 0xFFFF0000u) == 0xF800 && dd_argb_to_raw(&a.im, 0xFF808080u) == 0x8410);
  Image b(DD_FMT_A1R5G5B5, 1, 1);
  CHECK(dd_raw_to_argb(&b.im, 0x7FFF) == 0x00FFFFFFu && dd_raw_to_argb(&b.im, 0x8000) == 0xFF000000u);
  Image c(DD_FMT_A4R4G4B4, 1, 1);
  CHECK(dd_raw_to_argb(&c.im, 0xF8C4) == 0xFF88CC44u && dd_argb_to_raw(&c.im, 0x80FF0000u) == 0x8F00);
  Image d(DD_FMT_R3G3B2, 1, 1);
  CHECK(dd_raw_to_argb(&d.im, 0xE0) == 0xFFFF0000u && dd_raw_to_argb(&d.im, 0x03) == 0xFF0000FFu);
  Image l(DD_FMT_A8L8, 1, 1);
  CHECK(dd_raw_to_argb(&l.im, 0x80FF) == 0x80FFFFFFu && dd_argb_to_raw(&l.im, 0xFF00FF00u) == (0xFF00u | 149u));      // (green's luminance: 255 * 150 / 256)
  Image al(DD_FMT_A8, 1, 1);
  CHECK(dd_raw_to_argb(&al.im, 0x40) == 0x40000000u && dd_argb_to_raw(&al.im, 0x7F123456u) == 0x7F);
  Image p(DD_FMT_P8, 1, 1);
  p.palette[7] = 0xFF102030u;
  CHECK(dd_raw_to_argb(&p.im, 7) == 0xFF102030u && dd_argb_to_raw(&p.im, 0xFF102031u) == 7);
  p.im.palette = NULL;
  CHECK(dd_raw_to_argb(&p.im, 7) == 0xFF000000u);      // (no palette: black)
  // YUV: white, black and red of BT.601 studio range
  CHECK(dd_yuv_to_argb(235, 128, 128) == 0xFFFFFFFFu && dd_yuv_to_argb(16, 128, 128) == 0xFF000000u);
  Image y(DD_FMT_YUY2, 2, 1);
  y.bytes[0] = 235; y.bytes[1] = 128; y.bytes[2] = 16; y.bytes[3] = 128;
  CHECK(dd_read_argb(&y.im, 0, 0) == 0xFFFFFFFFu && dd_read_argb(&y.im, 1, 0) == 0xFF000000u);
  Image u(DD_FMT_UYVY, 2, 1);
  u.bytes[0] = 128; u.bytes[1] = 16; u.bytes[2] = 128; u.bytes[3] = 235;
  CHECK(dd_read_argb(&u.im, 0, 0) == 0xFF000000u && dd_read_argb(&u.im, 1, 0) == 0xFFFFFFFFu);
  uint32_t yy, uu, vv;
  dd_argb_to_yuv(0xFFFF0000u, &yy, &uu, &vv);
  const uint32_t back = dd_yuv_to_argb(yy, uu, vv);
  CHECK(((back >> 16) & 0xFF) >= 250 && ((back >> 8) & 0xFF) <= 6 && (back & 0xFF) <= 6);
  Image v12(DD_FMT_YV12, 4, 2);
  memset(v12.bytes.data(), 235, 8);      // (the luminance: white; V then U planes stay 0 -> a colour far from grey)
  v12.bytes[8] = 128; v12.bytes[9] = 128; v12.bytes[10] = 128; v12.bytes[11] = 128;
  CHECK(dd_read_argb(&v12.im, 0, 0) == 0xFFFFFFFFu && dd_read_argb(&v12.im, 3, 1) == 0xFFFFFFFFu);
  Image n12(DD_FMT_NV12, 2, 2);
  n12.bytes[0] = 16; n12.bytes[n12.im.pitch * 2] = 128; n12.bytes[n12.im.pitch * 2 + 1] = 128;      // (the chroma plane after the luminance's rows)
  CHECK(dd_read_argb(&n12.im, 0, 0) == 0xFF000000u);
  // DXT1: colour 0 red, colour 1 blue, codes 0, 1, 2 (2/3 red + 1/3 blue), 3 (1/3 + 2/3)
  Image x1(DD_FMT_DXT1, 4, 4);
  const uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0};
  memcpy(x1.bytes.data(), block, 8);
  CHECK(dd_read_argb(&x1.im, 0, 0) == 0xFFFF0000u && dd_read_argb(&x1.im, 1, 0) == 0xFF0000FFu);
  CHECK(dd_read_argb(&x1.im, 2, 0) == 0xFFAA0055u && dd_read_argb(&x1.im, 3, 0) == 0xFF5500AAu);
  // DXT1 with colour 0 <= colour 1: code 3 is transparent black
  const uint8_t punch[8] = {0x1F, 0x00, 0x00, 0xF8, 0xC0, 0, 0, 0};
  memcpy(x1.bytes.data(), punch, 8);
  CHECK(dd_read_argb(&x1.im, 3, 0) == 0x00000000u && dd_read_argb(&x1.im, 0, 1) == 0xFF0000FFu);
  // DXT3: explicit alpha 4 bits
  Image x3(DD_FMT_DXT3, 4, 4);
  x3.bytes[0] = 0xF0;      // (pixel 1 alpha 15, pixel 0 alpha 0)
  memcpy(x3.bytes.data() + 8, block, 8);
  CHECK(dd_read_argb(&x3.im, 0, 0) == 0x00FF0000u && dd_read_argb(&x3.im, 1, 0) == 0xFF0000FFu);
  // DXT5: alpha 255 and 0 interpolated in 7 steps
  Image x5(DD_FMT_DXT5, 4, 4);
  x5.bytes[0] = 255; x5.bytes[1] = 0;
  x5.bytes[2] = 0x08 | (0x02 << 3);      // (pixel 0 code 0 -> 255, pixel 1 code 1 -> 0 ... written as bits)
  x5.bytes[2] = (uint8_t)(0 | (1 << 3) | (2 << 6));
  memcpy(x5.bytes.data() + 8, block, 8);
  CHECK((dd_read_argb(&x5.im, 0, 0) >> 24) == 255 && (dd_read_argb(&x5.im, 1, 0) >> 24) == 0 && (dd_read_argb(&x5.im, 2, 0) >> 24) == (6 * 255) / 7);
  // depth
  CHECK(dd_depth_to_u32(DD_FMT_D16, 0xFFFF) == 0xFFFFFFFFu && dd_depth_from_u32(DD_FMT_D24S8, 0xFFFFFFFFu) == 0x00FFFFFFu);
  CHECK(dd_depth_from_u32(DD_FMT_D16, dd_depth_to_u32(DD_FMT_D24X8, 0x00800000u)) == 0x8000);
  CHECK(dd_colour_mask(DD_FMT_A8R8G8B8) == 0x00FFFFFFu && dd_colour_mask(DD_FMT_P8) == 0xFF && dd_colour_mask(DD_FMT_D16) == 0xFFFF);
}

static void test_fills() {
  Image a(DD_FMT_X8R8G8B8, 8, 8);
  DdRect r = R(2, 2, 5, 4);
  CHECK(dd_fill(&a.im, &r, 0x00ABCDEF) == DD_PIX_OK);
  CHECK(dd_get_raw(&a.im, 2, 2) == 0xABCDEF && dd_get_raw(&a.im, 4, 3) == 0xABCDEF && dd_get_raw(&a.im, 5, 3) == 0 && dd_get_raw(&a.im, 2, 4) == 0);
  r = R(0, 0, 9, 1);
  CHECK(dd_fill(&a.im, &r, 1) == DD_PIX_BAD_RECT);
  r = R(3, 3, 3, 4);
  CHECK(dd_fill(&a.im, &r, 1) == DD_PIX_BAD_RECT);      // (empty)
  r = R(-1, 0, 1, 1);
  CHECK(dd_fill(&a.im, &r, 1) == DD_PIX_BAD_RECT);
  Image p4(DD_FMT_P4, 5, 2);
  r = R(1, 0, 4, 2);
  CHECK(dd_fill(&p4.im, &r, 0x1F) == DD_PIX_OK);      // (only the 4 bits of a pixel)
  CHECK(dd_get_raw(&p4.im, 0, 0) == 0 && dd_get_raw(&p4.im, 1, 1) == 0xF && dd_get_raw(&p4.im, 3, 0) == 0xF && dd_get_raw(&p4.im, 4, 0) == 0);
  Image y(DD_FMT_YUY2, 4, 1);
  r = R(1, 0, 3, 1);
  CHECK(dd_fill(&y.im, &r, 0x80EB80EBu) == DD_PIX_OK);      // (whole pairs: pixels 0 .. 3)
  CHECK(y.bytes[0] == 0xEB && y.bytes[4] == 0xEB);
  Image d(DD_FMT_DXT1, 8, 8);
  r = R(0, 0, 8, 8);
  CHECK(dd_fill(&d.im, &r, 0) == DD_PIX_UNSUPPORTED);
  Image s16(DD_FMT_R5G6B5, 3, 3);
  r = R(0, 0, 3, 3);
  CHECK(dd_fill(&s16.im, &r, 0x1234) == DD_PIX_OK && dd_get_raw(&s16.im, 2, 2) == 0x1234);
}

static void test_blits() {
  // a plain copy and its overlapping forms
  Image a(DD_FMT_X8R8G8B8, 8, 8);
  for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) dd_put_raw(&a.im, x, y, y * 8 + x);
  DdRect d = R(1, 1, 5, 5), s = R(0, 0, 4, 4);
  CHECK(dd_blit(&a.im, &d, &a.im, &s, NULL) == DD_PIX_OK);
  CHECK(dd_get_raw(&a.im, 1, 1) == 0 && dd_get_raw(&a.im, 4, 4) == 3 * 8 + 3);      // (down and right: copied from the far end)
  for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) dd_put_raw(&a.im, x, y, y * 8 + x);
  d = R(0, 0, 4, 4); s = R(1, 1, 5, 5);
  CHECK(dd_blit(&a.im, &d, &a.im, &s, NULL) == DD_PIX_OK);
  CHECK(dd_get_raw(&a.im, 0, 0) == 9 && dd_get_raw(&a.im, 3, 3) == 4 * 8 + 4);
  // stretching overlapping rectangles of one image needs a copy of the source
  d = R(0, 0, 8, 8); s = R(0, 0, 4, 4);
  CHECK(dd_blit(&a.im, &d, &a.im, &s, NULL) == DD_PIX_OVERLAP);
  // stretch 2x and shrink
  Image src(DD_FMT_X8R8G8B8, 2, 2), dst(DD_FMT_X8R8G8B8, 4, 4);
  dd_put_raw(&src.im, 0, 0, 1); dd_put_raw(&src.im, 1, 0, 2); dd_put_raw(&src.im, 0, 1, 3); dd_put_raw(&src.im, 1, 1, 4);
  d = R(0, 0, 4, 4); s = R(0, 0, 2, 2);
  CHECK(dd_blit(&dst.im, &d, &src.im, &s, NULL) == DD_PIX_OK);
  CHECK(dd_get_raw(&dst.im, 0, 0) == 1 && dd_get_raw(&dst.im, 1, 1) == 1 && dd_get_raw(&dst.im, 2, 0) == 2 && dd_get_raw(&dst.im, 3, 3) == 4 && dd_get_raw(&dst.im, 0, 3) == 3);
  Image small(DD_FMT_X8R8G8B8, 2, 2);
  d = R(0, 0, 2, 2); s = R(0, 0, 4, 4);
  CHECK(dd_blit(&small.im, &d, &dst.im, &s, NULL) == DD_PIX_OK);
  CHECK(dd_get_raw(&small.im, 0, 0) == 1 && dd_get_raw(&small.im, 1, 1) == 4);
  // mirroring and turning
  DdBlit b;
  memset(&b, 0, sizeof b);
  b.flags = DD_BLIT_MIRROR_LR;
  d = R(0, 0, 2, 2); s = R(0, 0, 2, 2);
  CHECK(dd_blit(&small.im, &d, &src.im, &s, &b) == DD_PIX_OK && dd_get_raw(&small.im, 0, 0) == 2 && dd_get_raw(&small.im, 1, 1) == 3);
  b.flags = DD_BLIT_MIRROR_UD;
  CHECK(dd_blit(&small.im, &d, &src.im, &s, &b) == DD_PIX_OK && dd_get_raw(&small.im, 0, 0) == 3 && dd_get_raw(&small.im, 1, 0) == 4);
  b.flags = DD_BLIT_ROTATE180;
  CHECK(dd_blit(&small.im, &d, &src.im, &s, &b) == DD_PIX_OK && dd_get_raw(&small.im, 0, 0) == 4 && dd_get_raw(&small.im, 1, 1) == 1);
  b.flags = DD_BLIT_ROTATE90;      // (clockwise: the source's bottom-left goes to the top-left)
  CHECK(dd_blit(&small.im, &d, &src.im, &s, &b) == DD_PIX_OK && dd_get_raw(&small.im, 0, 0) == 3 && dd_get_raw(&small.im, 1, 0) == 1 && dd_get_raw(&small.im, 1, 1) == 2);
  b.flags = DD_BLIT_ROTATE270;
  CHECK(dd_blit(&small.im, &d, &src.im, &s, &b) == DD_PIX_OK && dd_get_raw(&small.im, 0, 0) == 2 && dd_get_raw(&small.im, 0, 1) == 1 && dd_get_raw(&small.im, 1, 1) == 3);
  // colour keys
  Image ks(DD_FMT_X8R8G8B8, 3, 1), kd(DD_FMT_X8R8G8B8, 3, 1);
  dd_put_raw(&ks.im, 0, 0, 0xFF00FF); dd_put_raw(&ks.im, 1, 0, 0x112233); dd_put_raw(&ks.im, 2, 0, 0xFFFF00FFu);      // (the key with alpha bits: the colour bits match)
  for (int x = 0; x < 3; ++x) dd_put_raw(&kd.im, (uint32_t)x, 0, 0x555555);
  memset(&b, 0, sizeof b);
  b.flags = DD_BLIT_SRC_KEY; b.src_key_low = b.src_key_high = 0xFF00FF;
  d = R(0, 0, 3, 1); s = R(0, 0, 3, 1);
  CHECK(dd_blit(&kd.im, &d, &ks.im, &s, &b) == DD_PIX_OK);
  CHECK(dd_get_raw(&kd.im, 0, 0) == 0x555555 && dd_get_raw(&kd.im, 1, 0) == 0x112233 && dd_get_raw(&kd.im, 2, 0) == 0x555555);
  dd_put_raw(&kd.im, 0, 0, 0xAA); dd_put_raw(&kd.im, 1, 0, 0xBB); dd_put_raw(&kd.im, 2, 0, 0xAA);
  b.flags = DD_BLIT_DST_KEY; b.dst_key_low = b.dst_key_high = 0xAA;
  CHECK(dd_blit(&kd.im, &d, &ks.im, &s, &b) == DD_PIX_OK);
  CHECK(dd_get_raw(&kd.im, 0, 0) == 0xFF00FF && dd_get_raw(&kd.im, 1, 0) == 0xBB && dd_get_raw(&kd.im, 2, 0) == 0xFFFF00FFu);
  // conversion: 5-6-5 into x8r8g8b8, a palette into 5-6-5, x8r8g8b8 into a palette (nearest entry)
  Image c16(DD_FMT_R5G6B5, 2, 1), c32(DD_FMT_X8R8G8B8, 2, 1);
  dd_put_raw(&c16.im, 0, 0, 0xF800); dd_put_raw(&c16.im, 1, 0, 0x07E0);
  d = R(0, 0, 2, 1); s = R(0, 0, 2, 1);
  CHECK(dd_blit(&c32.im, &d, &c16.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&c32.im, 0, 0) == 0xFF0000 && dd_get_raw(&c32.im, 1, 0) == 0x00FF00);
  Image pal(DD_FMT_P8, 2, 1);
  pal.palette[1] = 0xFFFF0000u; pal.palette[2] = 0xFF0000FFu;
  // (P8 into a colour format: the index in every channel, as Windows' blitter - not the palette's colour)
  dd_put_raw(&pal.im, 0, 0, 0xFF); dd_put_raw(&pal.im, 1, 0, 0x00);
  CHECK(dd_blit(&c16.im, &d, &pal.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&c16.im, 0, 0) == 0xFFFF && dd_get_raw(&c16.im, 1, 0) == 0x0000);
  dd_put_raw(&pal.im, 0, 0, 0x10);
  CHECK(dd_blit(&c32.im, &d, &pal.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&c32.im, 0, 0) == 0x101010);
  dd_put_raw(&c32.im, 0, 0, 0x0000F0); dd_put_raw(&c32.im, 1, 0, 0xF00000);
  CHECK(dd_blit(&pal.im, &d, &c32.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&pal.im, 0, 0) == 2 && dd_get_raw(&pal.im, 1, 0) == 1);
  // YUY2 into x8r8g8b8 (a video frame onto the screen) and stretched
  Image yuv(DD_FMT_YUY2, 2, 1), out(DD_FMT_X8R8G8B8, 4, 2);
  yuv.bytes[0] = 235; yuv.bytes[1] = 128; yuv.bytes[2] = 16; yuv.bytes[3] = 128;
  d = R(0, 0, 4, 2); s = R(0, 0, 2, 1);
  CHECK(dd_blit(&out.im, &d, &yuv.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&out.im, 1, 1) == 0xFFFFFF && dd_get_raw(&out.im, 2, 0) == 0);
  // blocks: whole blocks of one format only
  Image b1(DD_FMT_DXT1, 8, 8), b2(DD_FMT_DXT1, 8, 8);
  for (size_t i = 0; i < b1.bytes.size(); ++i) b1.bytes[i] = (uint8_t)i;
  d = R(4, 0, 8, 4); s = R(0, 4, 4, 8);
  CHECK(dd_blit(&b2.im, &d, &b1.im, &s, NULL) == DD_PIX_OK && memcmp(b2.bytes.data() + 8, b1.bytes.data() + 16, 8) == 0);
  // blocks: anything else re-encodes the blocks it touches (the pixels outside the rectangle kept)
  {
    // a solid blue block stretched from 4x4 to 8x8, and its corner 2x2 into the middle of another surface
    Image sb(DD_FMT_DXT1, 4, 4), big(DD_FMT_DXT1, 8, 8), argb(DD_FMT_A8R8G8B8, 8, 8);
    const uint8_t blue[8] = {0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0};
    memcpy(sb.bytes.data(), blue, 8);
    d = R(0, 0, 8, 8); s = R(0, 0, 4, 4);
    CHECK(dd_blit(&big.im, &d, &sb.im, &s, NULL) == DD_PIX_OK);
    CHECK(dd_blit(&argb.im, &d, &big.im, &d, NULL) == DD_PIX_OK && dd_get_raw(&argb.im, 7, 7) == 0xFF0000FFu && dd_get_raw(&argb.im, 0, 0) == 0xFF0000FFu);
    Image red(DD_FMT_R5G6B5, 4, 4);
    d = R(0, 0, 4, 4);
    CHECK(dd_fill(&red.im, &d, 0xF800) == DD_PIX_OK);
    d = R(2, 2, 6, 6); s = R(0, 0, 4, 4);
    CHECK(dd_blit(&big.im, &d, &red.im, &s, NULL) == DD_PIX_OK);      // (four blocks each partly written)
    d = R(0, 0, 8, 8);
    CHECK(dd_blit(&argb.im, &d, &big.im, &d, NULL) == DD_PIX_OK);
    CHECK(dd_get_raw(&argb.im, 2, 2) == 0xFFFF0000u && dd_get_raw(&argb.im, 5, 5) == 0xFFFF0000u && dd_get_raw(&argb.im, 1, 1) == 0xFF0000FFu &&
          dd_get_raw(&argb.im, 6, 6) == 0xFF0000FFu && dd_get_raw(&argb.im, 6, 2) == 0xFF0000FFu);
    // a DXT1 block into DXT3 (a format change) and back
    Image d3(DD_FMT_DXT3, 8, 8);
    CHECK(dd_blit(&d3.im, &d, &big.im, &d, NULL) == DD_PIX_OK && d3.bytes[0] == 0xFF && d3.bytes[7] == 0xFF);
    Image back(DD_FMT_A8R8G8B8, 8, 8);
    CHECK(dd_blit(&back.im, &d, &d3.im, &d, NULL) == DD_PIX_OK && memcmp(back.bytes.data(), argb.bytes.data(), back.bytes.size()) == 0);
    // what Wine's test_surface_format_conversion_alpha records for Windows: green and blue as DXT1 colour 0 0x07e0, colour 1 0x001f, codes 0x14445154
    static const uint32_t rgba[16] = {0xff00ff00, 0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff00ff00, 0xff0000ff, 0xff0000ff,
                                      0xff00ff00, 0xff0000ff, 0xff00ff00, 0xff0000ff, 0xff00ff00, 0xff0000ff, 0xff0000ff, 0xff00ff00};
    Image src(DD_FMT_X8R8G8B8, 4, 4), x1(DD_FMT_DXT1, 4, 4), x2(DD_FMT_DXT2, 4, 4);
    for (uint32_t i = 0; i < 16; ++i) dd_put_raw(&src.im, i & 3, i / 4, rgba[i] & 0xFFFFFF);
    d = R(0, 0, 4, 4);
    CHECK(dd_blit(&x1.im, &d, &src.im, &d, NULL) == DD_PIX_OK);
    const uint8_t want1[8] = {0xe0, 0x07, 0x1f, 0x00, 0x54, 0x51, 0x44, 0x14};
    CHECK(memcmp(x1.bytes.data(), want1, 8) == 0);
    CHECK(dd_blit(&x2.im, &d, &x1.im, &d, NULL) == DD_PIX_OK);
    const uint8_t want2[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xe0, 0x07, 0x1f, 0x00, 0x54, 0x51, 0x44, 0x14};
    CHECK(memcmp(x2.bytes.data(), want2, 16) == 0);
    // DXT into X8R8G8B8: the alpha byte written (0xff), and X1R5G5B5 into A1R5G5B5 copied as it is
    Image x8(DD_FMT_X8R8G8B8, 4, 4);
    CHECK(dd_blit(&x8.im, &d, &x1.im, &d, NULL) == DD_PIX_OK && dd_get_raw(&x8.im, 0, 0) == 0xff00ff00u && dd_get_raw(&x8.im, 1, 0) == 0xff0000ffu);
    Image x555(DD_FMT_X1R5G5B5, 4, 4), a555(DD_FMT_A1R5G5B5, 4, 4);
    dd_put_raw(&x555.im, 0, 0, 0x03e0);
    CHECK(dd_blit(&a555.im, &d, &x555.im, &d, NULL) == DD_PIX_OK && dd_get_raw(&a555.im, 0, 0) == 0x03e0);
    // DXT1 transparency survives (three-colour blocks), DXT5 alpha ramps are close
    Image a8(DD_FMT_A8R8G8B8, 4, 4), x5(DD_FMT_DXT5, 4, 4), out8(DD_FMT_A8R8G8B8, 4, 4);
    for (uint32_t i = 0; i < 16; ++i) dd_put_raw(&a8.im, i & 3, i / 4, ((i * 17u) << 24) | 0x00406080u);
    CHECK(dd_blit(&x1.im, &d, &a8.im, &d, NULL) == DD_PIX_OK && dd_blit(&out8.im, &d, &x1.im, &d, NULL) == DD_PIX_OK);
    CHECK((dd_get_raw(&out8.im, 0, 0) >> 24) == 0 && (dd_get_raw(&out8.im, 3, 3) >> 24) == 0xFF);
    CHECK(dd_blit(&x5.im, &d, &a8.im, &d, NULL) == DD_PIX_OK && dd_blit(&out8.im, &d, &x5.im, &d, NULL) == DD_PIX_OK);
    int worst = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      const int a = (int)(dd_get_raw(&out8.im, i & 3, i / 4) >> 24), want = (int)(i * 17u);
      if (abs(a - want) > worst) worst = abs(a - want);
    }
    CHECK(worst <= 18);
    CHECK((dd_get_raw(&out8.im, 2, 2) & 0xFFFFFF) == 0x00426184u || (dd_get_raw(&out8.im, 2, 2) & 0xFFFFFF) == 0x00406080u ||
          dd_rgb_distance(dd_get_raw(&out8.im, 2, 2), 0x406080u) < 64);
  }
  // a block source decoded into a colour destination
  Image col(DD_FMT_X8R8G8B8, 4, 4);
  const uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0, 0, 0};
  memcpy(b1.bytes.data(), block, 8);
  d = R(0, 0, 4, 4); s = R(0, 0, 4, 4);
  CHECK(dd_blit(&col.im, &d, &b1.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&col.im, 3, 3) == 0xFFFF0000u);      // (the alpha byte written, as Windows)
  // depth into depth, and the refusal of depth into colour
  Image z16(DD_FMT_D16, 1, 1), z24(DD_FMT_D24S8, 1, 1);
  dd_put_raw(&z16.im, 0, 0, 0xFFFF);
  d = R(0, 0, 1, 1); s = R(0, 0, 1, 1);
  CHECK(dd_blit(&z24.im, &d, &z16.im, &s, NULL) == DD_PIX_OK && dd_get_raw(&z24.im, 0, 0) == 0x00FFFFFF);
  CHECK(dd_blit(&col.im, &d, &z16.im, &s, NULL) == DD_PIX_UNSUPPORTED);
  // out of bounds
  d = R(0, 0, 5, 1);
  CHECK(dd_blit(&col.im, &d, &z16.im, &s, NULL) == DD_PIX_BAD_RECT);
}

static void test_to_argb() {
  Image p(DD_FMT_P8, 2, 1);
  p.palette[3] = 0xFF112233u;
  dd_put_raw(&p.im, 0, 0, 3); dd_put_raw(&p.im, 1, 0, 4);
  uint8_t out[8];
  DdRect r = R(0, 0, 2, 1);
  CHECK(dd_to_argb(&p.im, &r, out, sizeof out, 8, 1, 3, 3) == DD_PIX_OK);
  CHECK(out[0] == 0x33 && out[1] == 0x22 && out[2] == 0x11 && out[3] == 0x00 && out[7] == 0xFF);      // (keyed: alpha 0; the other opaque)
  CHECK(dd_to_argb(&p.im, &r, out, 7, 8, 0, 0, 0) == DD_PIX_BAD_IMAGE);      // (an output a byte short)
  // a key of a format with alpha compares the alpha too (Wine's test_ck_rgba); an unkeyed texel of alpha 0 becomes 1, so alpha 0 means keyed
  {
    Image a(DD_FMT_A8R8G8B8, 3, 1);
    dd_put_raw(&a.im, 0, 0, 0xFF00FF00u); dd_put_raw(&a.im, 1, 0, 0x7F00FF00u); dd_put_raw(&a.im, 2, 0, 0x00123456u);
    uint8_t keyed[12];
    DdRect ar = R(0, 0, 3, 1);
    CHECK(dd_to_argb(&a.im, &ar, keyed, sizeof keyed, 12, 1, 0xFF00FF00u, 0xFF00FF00u) == DD_PIX_OK);
    CHECK(keyed[3] == 0x00 && keyed[1] == 0xFF && keyed[7] == 0x7F && keyed[11] == 0x01 && keyed[8] == 0x56);
    CHECK(dd_to_argb(&a.im, &ar, keyed, sizeof keyed, 12, 0, 0, 0) == DD_PIX_OK && keyed[11] == 0x00);      // (no key: alpha as it is)
    Image x(DD_FMT_X8R8G8B8, 1, 1);
    dd_put_raw(&x.im, 0, 0, 0x0000FF00u);
    DdRect xr = R(0, 0, 1, 1);
    CHECK(dd_to_argb(&x.im, &xr, keyed, sizeof keyed, 4, 1, 0xFF00FF00u, 0xFF00FF00u) == DD_PIX_OK && keyed[3] == 0x00);      // (no alpha bits: they are not compared)
  }
  Image z(DD_FMT_D16, 1, 1);
  r = R(0, 0, 1, 1);
  CHECK(dd_to_argb(&z.im, &r, out, sizeof out, 4, 0, 0, 0) == DD_PIX_UNSUPPORTED);
}

static uint32_t g_seed = 12345;
static uint32_t rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }

static void test_truncation_and_random() {
  // every format: an image one byte too short is refused by fills and blits, never read past
  for (uint32_t f = 1; f < DD_FMT_COUNT; ++f) {
    Image good(f, 8, 8), cut(f, 8, 8, 4, 1);
    DdRect r = R(0, 0, 8, 8);
    CHECK(dd_image_check(&good.im));
    CHECK(!dd_image_check(&cut.im));
    CHECK(dd_fill(&cut.im, &r, 0) == DD_PIX_BAD_IMAGE);
    CHECK(dd_blit(&cut.im, &r, &good.im, &r, NULL) == DD_PIX_BAD_IMAGE);
    CHECK(dd_blit(&good.im, &r, &cut.im, &r, NULL) == DD_PIX_BAD_IMAGE);
    uint8_t out[8 * 8 * 4];
    CHECK(dd_to_argb(&cut.im, &r, out, sizeof out, 32, 0, 0, 0) == DD_PIX_BAD_IMAGE);
  }
  // random blits between random formats, sizes, rectangles and effects into exactly-sized memory (ASan watches every access)
  int ok = 0;
  for (int i = 0; i < 20000; ++i) {
    const uint32_t f1 = 1 + rnd() % (DD_FMT_COUNT - 1), f2 = 1 + rnd() % (DD_FMT_COUNT - 1);
    const uint32_t w1 = 1 + rnd() % 19, h1 = 1 + rnd() % 19, w2 = 1 + rnd() % 19, h2 = 1 + rnd() % 19;
    if (dd_format(f1)->kind == DD_KIND_YUV_PLANAR && ((w1 | h1) & 1)) continue;
    if (dd_format(f2)->kind == DD_KIND_YUV_PLANAR && ((w2 | h2) & 1)) continue;
    Image a(f1, w1, h1, 1u << (rnd() % 4)), b(f2, w2, h2, 1u << (rnd() % 4));
    for (auto &x : a.bytes) x = (uint8_t)rnd();
    for (auto &x : b.bytes) x = (uint8_t)rnd();
    DdRect dr, sr;
    if (rnd() % 4) {      // (mostly rectangles inside the images, some anywhere)
      const int dl = (int)(rnd() % w1), dt = (int)(rnd() % h1), sl = (int)(rnd() % w2), st = (int)(rnd() % h2);
      dr = R(dl, dt, dl + 1 + (int)(rnd() % (w1 - (uint32_t)dl)), dt + 1 + (int)(rnd() % (h1 - (uint32_t)dt)));
      sr = R(sl, st, sl + 1 + (int)(rnd() % (w2 - (uint32_t)sl)), st + 1 + (int)(rnd() % (h2 - (uint32_t)st)));
    } else {
      const int dl = (int)(rnd() % (w1 + 2)) - 1, dt = (int)(rnd() % (h1 + 2)) - 1, sl = (int)(rnd() % (w2 + 2)) - 1, st = (int)(rnd() % (h2 + 2)) - 1;
      dr = R(dl, dt, dl + 1 + (int)(rnd() % (w1 + 1)), dt + 1 + (int)(rnd() % (h1 + 1)));
      sr = R(sl, st, sl + 1 + (int)(rnd() % (w2 + 1)), st + 1 + (int)(rnd() % (h2 + 1)));
    }
    DdBlit bl;
    memset(&bl, 0, sizeof bl);
    bl.flags = rnd() & 0x7F;
    bl.src_key_low = rnd(); bl.src_key_high = bl.src_key_low + (rnd() & 0xFF);
    bl.dst_key_low = rnd(); bl.dst_key_high = bl.dst_key_low + (rnd() & 0xFF);
    const int r = (rnd() & 1) ? dd_blit(&a.im, &dr, &b.im, &sr, &bl) : dd_blit(&a.im, &dr, &a.im, &sr, &bl);
    if (r == DD_PIX_OK) ++ok;
    if (rnd() % 4 == 0) dd_fill(&a.im, &dr, rnd());
    if (dd_format_has_colour(f2) && rnd() % 4 == 0) {
      std::vector<uint8_t> out((size_t)w2 * h2 * 4);
      dd_to_argb(&b.im, &sr, out.data(), out.size(), w2 * 4, (int)(rnd() & 1), bl.src_key_low, bl.src_key_high);
    }
  }
  CHECK(ok > 1000);      // (a good part of them were blits that happened, not refusals)
  // bit-flipped images: the pitch, size and dimensions of a valid image changed at random are refused or handled inside the memory
  for (int i = 0; i < 20000; ++i) {
    const uint32_t f = 1 + rnd() % (DD_FMT_COUNT - 1);
    Image a(f, 1 + rnd() % 17, 2 + 2 * (rnd() % 8));
    Image b(f, 1 + rnd() % 17, 2 + 2 * (rnd() % 8));
    DdImage bad = a.im;
    switch (rnd() % 4) {
      case 0: bad.pitch ^= 1u << (rnd() % 32); break;
      case 1: bad.width ^= 1u << (rnd() % 32); break;
      case 2: bad.height ^= 1u << (rnd() % 32); break;
      default: bad.size ^= (size_t)1 << (rnd() % 16); if (bad.size > a.bytes.size()) bad.size = a.bytes.size(); break;
    }
    DdRect r = R(0, 0, (int)(rnd() % 20), (int)(rnd() % 20));
    dd_fill(&bad, &r, rnd());
    dd_blit(&bad, &r, &b.im, &r, NULL);
    dd_blit(&b.im, &r, &bad, &r, NULL);
  }
}

int main() {
  test_sizes();
  test_raw_pixels();
  test_colours();
  test_fills();
  test_blits();
  test_to_argb();
  test_truncation_and_random();
  printf("DdrawPixelsTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
