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

#include "PipeWire/VideoConvert.hpp"

namespace pipewire {

namespace {

// SPA_VIDEO_FORMAT_*
enum : uint32_t {
  kI420 = 2, kYV12 = 3, kYUY2 = 4, kUYVY = 5, kAYUV = 6, kRGBx = 7, kBGRx = 8, kxRGB = 9, kxBGR = 10, kRGBA = 11, kBGRA = 12, kARGB = 13, kABGR = 14,
  kRGB = 15, kBGR = 16, kY41B = 17, kY42B = 18, kYVYU = 19, kY444 = 20, kNV12 = 23, kNV21 = 24, kGRAY8 = 25, kGRAY16_BE = 26, kGRAY16_LE = 27,
  kRGB16 = 29, kBGR16 = 30, kRGB15 = 31, kBGR15 = 32, kARGB64 = 39, kAYUV64 = 40, kI420_10LE = 43, kI422_10LE = 45, kY444_10LE = 47, kNV16 = 51,
  kNV24 = 52, kNV61 = 60, kP010_10LE = 62, kVYUY = 64, kI420_12LE = 73, kRGBA_F16 = 78, kRGBA_F32 = 79, kxRGB_210LE = 80, kxBGR_210LE = 81,
  kRGBx_102LE = 82, kBGRx_102LE = 83, kARGB_210LE = 84, kABGR_210LE = 85, kRGBA_102LE = 86, kBGRA_102LE = 87
};

// Where a packed 8-bit RGB format keeps red, green, blue and alpha in a pixel (alpha -1: none, opaque), and the pixel's bytes.
struct RgbLayout {
  int8_t r, g, b, a;
  uint8_t bytes;
};
bool rgb_layout(uint32_t format, RgbLayout* out) {
  switch (format) {
    case kRGBx: *out = {0, 1, 2, -1, 4}; return true;
    case kBGRx: *out = {2, 1, 0, -1, 4}; return true;
    case kxRGB: *out = {1, 2, 3, -1, 4}; return true;
    case kxBGR: *out = {3, 2, 1, -1, 4}; return true;
    case kRGBA: *out = {0, 1, 2, 3, 4}; return true;
    case kBGRA: *out = {2, 1, 0, 3, 4}; return true;
    case kARGB: *out = {1, 2, 3, 0, 4}; return true;
    case kABGR: *out = {3, 2, 1, 0, 4}; return true;
    case kRGB: *out = {0, 1, 2, -1, 3}; return true;
    case kBGR: *out = {2, 1, 0, -1, 3}; return true;
    default: return false;
  }
}
// A 32-bit little-endian word of 10-bit components: where red, green, blue and alpha start (alpha -1: none) and alpha's width (2 bits).
struct Rgb10Layout {
  int8_t r, g, b, a;
};
bool rgb10_layout(uint32_t format, Rgb10Layout* out) {
  switch (format) {
    case kxRGB_210LE: *out = {20, 10, 0, -1}; return true;
    case kxBGR_210LE: *out = {0, 10, 20, -1}; return true;
    case kRGBx_102LE: *out = {22, 12, 2, -1}; return true;
    case kBGRx_102LE: *out = {2, 12, 22, -1}; return true;
    case kARGB_210LE: *out = {20, 10, 0, 30}; return true;
    case kABGR_210LE: *out = {0, 10, 20, 30}; return true;
    case kRGBA_102LE: *out = {22, 12, 2, 0}; return true;
    case kBGRA_102LE: *out = {2, 12, 22, 0}; return true;
    default: return false;
  }
}
// Where a packed 4:2:2 format keeps its first and second luma and its U and V in each four bytes.
struct PackedYuv {
  uint8_t y0, u, y1, v;
};
bool packed_yuv(uint32_t format, PackedYuv* out) {
  switch (format) {
    case kYUY2: *out = {0, 1, 2, 3}; return true;
    case kUYVY: *out = {1, 0, 3, 2}; return true;
    case kYVYU: *out = {0, 3, 2, 1}; return true;
    case kVYUY: *out = {1, 2, 3, 0}; return true;
    default: return false;
  }
}
// A planar or semi-planar YUV format: its chroma's subsampling, a sample's bytes (1, or 2 little-endian), how far a sample is shifted from 8 bits
// (10-bit: 2; P010's top ten bits of 16: 8, written as value << 8), whether U and V are one interleaved plane, and whether V comes first.
struct PlanarYuv {
  uint8_t sx, sy, bytes, shift;
  bool semi, v_first;
};
bool planar_yuv(uint32_t format, PlanarYuv* out) {
  switch (format) {
    case kI420: *out = {2, 2, 1, 0, false, false}; return true;
    case kYV12: *out = {2, 2, 1, 0, false, true}; return true;
    case kY41B: *out = {4, 1, 1, 0, false, false}; return true;
    case kY42B: *out = {2, 1, 1, 0, false, false}; return true;
    case kY444: *out = {1, 1, 1, 0, false, false}; return true;
    case kNV12: *out = {2, 2, 1, 0, true, false}; return true;
    case kNV21: *out = {2, 2, 1, 0, true, true}; return true;
    case kNV16: *out = {2, 1, 1, 0, true, false}; return true;
    case kNV61: *out = {2, 1, 1, 0, true, true}; return true;
    case kNV24: *out = {1, 1, 1, 0, true, false}; return true;
    case kI420_10LE: *out = {2, 2, 2, 2, false, false}; return true;
    case kI422_10LE: *out = {2, 1, 2, 2, false, false}; return true;
    case kY444_10LE: *out = {1, 1, 2, 2, false, false}; return true;
    case kI420_12LE: *out = {2, 2, 2, 4, false, false}; return true;
    case kP010_10LE: *out = {2, 2, 2, 8, true, false}; return true;
    default: return false;
  }
}
bool known(uint32_t format) {
  RgbLayout l;
  Rgb10Layout l10;
  PackedYuv p;
  PlanarYuv q;
  if (rgb_layout(format, &l) || rgb10_layout(format, &l10) || packed_yuv(format, &p) || planar_yuv(format, &q)) return true;
  switch (format) {
    case kAYUV: case kAYUV64: case kGRAY8: case kGRAY16_BE: case kGRAY16_LE: case kRGB16: case kBGR16: case kRGB15: case kBGR15: case kARGB64:
    case kRGBA_F16: case kRGBA_F32: return true;
    default: return false;
  }
}

uint8_t clamp8(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
// BT.601, limited range
void yuv_to_rgb(int32_t y, int32_t u, int32_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
  const int32_t c = 298 * (y - 16), d = u - 128, e = v - 128;
  *r = clamp8((c + 409 * e + 128) >> 8);
  *g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
  *b = clamp8((c + 516 * d + 128) >> 8);
}
uint8_t luma(int32_t r, int32_t g, int32_t b) { return clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
uint8_t chroma_u(int32_t r, int32_t g, int32_t b) { return clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
uint8_t chroma_v(int32_t r, int32_t g, int32_t b) { return clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

uint32_t le16(ConstByteView v, uint64_t at) { return uint32_t(v[at]) | uint32_t(v[at + 1]) << 8; }
uint32_t le32(ConstByteView v, uint64_t at) { return le16(v, at) | le16(v, at + 2) << 16; }
void put16(ByteView v, uint64_t at, uint32_t x) {
  v[at] = static_cast<uint8_t>(x);
  v[at + 1] = static_cast<uint8_t>(x >> 8);
}
void put32(ByteView v, uint64_t at, uint32_t x) {
  put16(v, at, x & 0xFFFF);
  put16(v, at + 2, x >> 16);
}
// IEEE half and single floats to and from 0..255 (clamped)
float half_to_float(uint32_t h) {
  const uint32_t sign = h >> 15, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v;
  if (exponent == 0) v = static_cast<float>(mantissa) / 16777216.0f;      // (subnormal: mantissa * 2^-24)
  else if (exponent == 31) v = 65504.0f;
  else v = (1.0f + static_cast<float>(mantissa) / 1024.0f) * static_cast<float>(1u << exponent) / 32768.0f;
  return sign ? -v : v;
}
uint32_t float_to_half(float f) {      // (0 .. 1 only: what a colour is)
  if (!(f > 0.0f)) return 0;
  if (f >= 1.0f) return 0x3C00;
  int exponent = 15;
  while (f < 1.0f && exponent > 1) {
    f *= 2.0f;
    --exponent;
  }
  return static_cast<uint32_t>(exponent) << 10 | (static_cast<uint32_t>((f - 1.0f) * 1024.0f + 0.5f) & 0x3FF);
}
uint8_t unit_to8(float f) { return clamp8(static_cast<int32_t>(f * 255.0f + 0.5f)); }

// One pixel as 8-bit red, green, blue and alpha, from a frame of a format that is not YUV.
struct Rgba {
  uint8_t r, g, b, a;
};
Rgba read_rgb(const VideoFormat& f, ConstByteView in, uint32_t x, uint32_t y) {
  const uint64_t row = f.plane(0).row_bytes, at0 = uint64_t{y} * row;
  RgbLayout l;
  Rgb10Layout l10;
  if (rgb_layout(f.format, &l)) {
    const ConstByteView px = in.subview(at0 + uint64_t{x} * l.bytes, l.bytes);
    return {px[uint64_t(l.r)], px[uint64_t(l.g)], px[uint64_t(l.b)], l.a >= 0 ? px[uint64_t(l.a)] : uint8_t(0xFF)};
  }
  if (rgb10_layout(f.format, &l10)) {
    const uint32_t w = le32(in, at0 + 4ull * x);
    const uint8_t a = l10.a >= 0 ? static_cast<uint8_t>(((w >> l10.a) & 3) * 85) : 0xFF;
    return {static_cast<uint8_t>((w >> l10.r) >> 2 & 0xFF), static_cast<uint8_t>((w >> l10.g) >> 2 & 0xFF), static_cast<uint8_t>((w >> l10.b) >> 2 & 0xFF), a};
  }
  switch (f.format) {
    case kGRAY8: { const uint8_t v = in[at0 + x]; return {v, v, v, 0xFF}; }
    case kGRAY16_LE: case kGRAY16_BE: {
      const uint8_t v = f.format == kGRAY16_LE ? in[at0 + 2ull * x + 1] : in[at0 + 2ull * x];      // (the high byte)
      return {v, v, v, 0xFF};
    }
    case kRGB16: case kBGR16: {      // 5:6:5 in a little-endian word, red (or blue) on top
      const uint32_t w = le16(in, at0 + 2ull * x);
      const uint8_t hi = static_cast<uint8_t>(((w >> 11) & 31) * 255 / 31), mid = static_cast<uint8_t>(((w >> 5) & 63) * 255 / 63), lo = static_cast<uint8_t>((w & 31) * 255 / 31);
      return f.format == kRGB16 ? Rgba{hi, mid, lo, 0xFF} : Rgba{lo, mid, hi, 0xFF};
    }
    case kRGB15: case kBGR15: {      // x:5:5:5
      const uint32_t w = le16(in, at0 + 2ull * x);
      const uint8_t hi = static_cast<uint8_t>(((w >> 10) & 31) * 255 / 31), mid = static_cast<uint8_t>(((w >> 5) & 31) * 255 / 31), lo = static_cast<uint8_t>((w & 31) * 255 / 31);
      return f.format == kRGB15 ? Rgba{hi, mid, lo, 0xFF} : Rgba{lo, mid, hi, 0xFF};
    }
    case kARGB64: {      // A, R, G, B of 16 bits each (little-endian): their high bytes
      const uint64_t at = at0 + 8ull * x;
      return {in[at + 3], in[at + 5], in[at + 7], in[at + 1]};
    }
    case kRGBA_F16: {
      const uint64_t at = at0 + 8ull * x;
      return {unit_to8(half_to_float(le16(in, at))), unit_to8(half_to_float(le16(in, at + 2))), unit_to8(half_to_float(le16(in, at + 4))),
              unit_to8(half_to_float(le16(in, at + 6)))};
    }
    case kRGBA_F32: {
      const uint64_t at = at0 + 16ull * x;
      return {unit_to8(__builtin_bit_cast(float, le32(in, at))), unit_to8(__builtin_bit_cast(float, le32(in, at + 4))),
              unit_to8(__builtin_bit_cast(float, le32(in, at + 8))), unit_to8(__builtin_bit_cast(float, le32(in, at + 12)))};
    }
    default: return {0, 0, 0, 0xFF};
  }
}
void write_rgb(const VideoFormat& f, ByteView out, uint32_t x, uint32_t y, Rgba c) {
  const uint64_t row = f.plane(0).row_bytes, at0 = uint64_t{y} * row;
  RgbLayout l;
  Rgb10Layout l10;
  if (rgb_layout(f.format, &l)) {
    ByteView o = out.subview(at0 + uint64_t{x} * l.bytes, l.bytes);
    if (l.bytes == 4) o.fill(0xFF);      // (the x byte)
    o[uint64_t(l.r)] = c.r;
    o[uint64_t(l.g)] = c.g;
    o[uint64_t(l.b)] = c.b;
    if (l.a >= 0) o[uint64_t(l.a)] = c.a;
    return;
  }
  auto ten = [](uint8_t v) { return uint32_t(v) << 2 | uint32_t(v) >> 6; };
  if (rgb10_layout(f.format, &l10)) {
    uint32_t w = ten(c.r) << l10.r | ten(c.g) << l10.g | ten(c.b) << l10.b;
    w |= (l10.a >= 0 ? uint32_t(c.a) >> 6 : 3u) << (l10.a >= 0 ? l10.a : (l10.r == 22 || l10.b == 22 ? 0 : 30));      // (alpha, or the x bits all set)
    put32(out, at0 + 4ull * x, w);
    return;
  }
  switch (f.format) {
    case kGRAY8: out[at0 + x] = clamp8((77 * c.r + 150 * c.g + 29 * c.b + 128) >> 8); return;      // (GRAY8 is full range)
    case kGRAY16_LE: case kGRAY16_BE: {
      const uint32_t v = uint32_t(clamp8((77 * c.r + 150 * c.g + 29 * c.b + 128) >> 8)) * 257;
      if (f.format == kGRAY16_LE) put16(out, at0 + 2ull * x, v);
      else { out[at0 + 2ull * x] = static_cast<uint8_t>(v >> 8); out[at0 + 2ull * x + 1] = static_cast<uint8_t>(v); }
      return;
    }
    case kRGB16: case kBGR16: {
      const uint32_t hi = (f.format == kRGB16 ? c.r : c.b) >> 3, lo = (f.format == kRGB16 ? c.b : c.r) >> 3;
      put16(out, at0 + 2ull * x, hi << 11 | uint32_t(c.g >> 2) << 5 | lo);
      return;
    }
    case kRGB15: case kBGR15: {
      const uint32_t hi = (f.format == kRGB15 ? c.r : c.b) >> 3, lo = (f.format == kRGB15 ? c.b : c.r) >> 3;
      put16(out, at0 + 2ull * x, 0x8000u | hi << 10 | uint32_t(c.g >> 3) << 5 | lo);
      return;
    }
    case kARGB64: {
      const uint64_t at = at0 + 8ull * x;
      put16(out, at, uint32_t(c.a) * 257);
      put16(out, at + 2, uint32_t(c.r) * 257);
      put16(out, at + 4, uint32_t(c.g) * 257);
      put16(out, at + 6, uint32_t(c.b) * 257);
      return;
    }
    case kRGBA_F16: {
      const uint64_t at = at0 + 8ull * x;
      const uint8_t v[4] = {c.r, c.g, c.b, c.a};
      for (int k = 0; k < 4; ++k) put16(out, at + 2ull * k, float_to_half(static_cast<float>(v[k]) / 255.0f));
      return;
    }
    case kRGBA_F32: {
      const uint64_t at = at0 + 16ull * x;
      const uint8_t v[4] = {c.r, c.g, c.b, c.a};
      for (int k = 0; k < 4; ++k) put32(out, at + 4ull * k, __builtin_bit_cast(uint32_t, static_cast<float>(v[k]) / 255.0f));
      return;
    }
    default: return;
  }
}

bool is_yuv(uint32_t format) {
  PackedYuv p;
  PlanarYuv q;
  return packed_yuv(format, &p) || planar_yuv(format, &q) || format == kAYUV || format == kAYUV64;
}
// One pixel's Y, U, V (8 bits) and alpha, from a YUV frame.
void read_yuv(const VideoFormat& f, ConstByteView in, uint32_t x, uint32_t y, int32_t* Y, int32_t* U, int32_t* V, uint8_t* A) {
  *A = 0xFF;
  PackedYuv p;
  PlanarYuv q;
  const uint64_t row0 = f.plane(0).row_bytes;
  if (packed_yuv(f.format, &p)) {
    const ConstByteView pair = in.subview(uint64_t{y} * row0 + 4ull * (x / 2), 4);
    *Y = pair[x % 2 ? p.y1 : p.y0];
    *U = pair[p.u];
    *V = pair[p.v];
    return;
  }
  if (f.format == kAYUV || f.format == kAYUV64) {
    const uint32_t step = f.format == kAYUV ? 1 : 2, off = f.format == kAYUV ? 0 : 1;      // (AYUV64: the high bytes of little-endian 16-bit samples)
    const ConstByteView px = in.subview(uint64_t{y} * row0 + 4ull * step * x, 4ull * step);
    *A = px[off];
    *Y = px[step + off];
    *U = px[2 * step + off];
    *V = px[3 * step + off];
    return;
  }
  planar_yuv(f.format, &q);
  auto sample = [&](uint64_t at) -> int32_t {
    const uint32_t v = q.bytes == 1 ? in[at] : le16(in, at);
    return static_cast<int32_t>(v >> q.shift);
  };
  *Y = sample(uint64_t{y} * row0 + uint64_t{x} * q.bytes);
  const uint64_t cx = x / q.sx, cy = y / q.sy;
  const VideoPlane c = f.plane(1);
  if (q.semi) {
    const uint64_t at = f.plane_offset(1) + cy * c.row_bytes + cx * 2 * q.bytes;
    const int32_t first = sample(at), second = sample(at + q.bytes);
    *U = q.v_first ? second : first;
    *V = q.v_first ? first : second;
  } else {
    const int32_t first = sample(f.plane_offset(1) + cy * c.row_bytes + cx * q.bytes), second = sample(f.plane_offset(2) + cy * c.row_bytes + cx * q.bytes);
    *U = q.v_first ? second : first;
    *V = q.v_first ? first : second;
  }
}

// The frame as RGBA (4 bytes a pixel, row after row). `in` holds every plane (checked by the caller).
void to_rgba(const VideoFormat& f, ConstByteView in, ByteView rgba) {
  const uint32_t w = f.width, h = f.height;
  const bool yuv = is_yuv(f.format);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      ByteView out = rgba.subview((uint64_t{y} * w + x) * 4, 4);
      if (yuv) {
        int32_t Y, U, V;
        uint8_t a;
        read_yuv(f, in, x, y, &Y, &U, &V, &a);
        yuv_to_rgb(Y, U, V, &out[0], &out[1], &out[2]);
        out[3] = a;
      } else {
        const Rgba c = read_rgb(f, in, x, y);
        out[0] = c.r;
        out[1] = c.g;
        out[2] = c.b;
        out[3] = c.a;
      }
    }
}

// The RGBA frame in `f`'s layout (`out` is f.frame_bytes()).
void from_rgba(const VideoFormat& f, ConstByteView rgba, ByteView out) {
  const uint32_t w = f.width, h = f.height;
  auto px = [&](uint32_t x, uint32_t y) {
    const ConstByteView q = rgba.subview((uint64_t{y} * w + x) * 4, 4);
    return Rgba{q[0], q[1], q[2], q[3]};
  };
  if (!is_yuv(f.format)) {
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) write_rgb(f, out, x, y, px(x, y));
    return;
  }
  const uint64_t row0 = f.plane(0).row_bytes;
  PackedYuv p;
  if (packed_yuv(f.format, &p)) {
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; x += 2) {
        const Rgba a = px(x, y), b = x + 1 < w ? px(x + 1, y) : a;
        ByteView pair = out.subview(uint64_t{y} * row0 + 2ull * x, 4);
        pair[p.y0] = luma(a.r, a.g, a.b);
        pair[p.y1] = luma(b.r, b.g, b.b);
        pair[p.u] = chroma_u((a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2);
        pair[p.v] = chroma_v((a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2);
      }
    return;
  }
  if (f.format == kAYUV || f.format == kAYUV64) {
    const uint32_t step = f.format == kAYUV ? 1 : 2;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const Rgba c = px(x, y);
        const uint8_t v[4] = {c.a, luma(c.r, c.g, c.b), chroma_u(c.r, c.g, c.b), chroma_v(c.r, c.g, c.b)};
        ByteView o = out.subview(uint64_t{y} * row0 + 4ull * step * x, 4ull * step);
        for (uint32_t k = 0; k < 4; ++k) {
          if (step == 1) o[k] = v[k];
          else put16(o, 2ull * k, uint32_t(v[k]) * 257);
        }
      }
    return;
  }
  PlanarYuv q;
  planar_yuv(f.format, &q);
  auto put_sample = [&](uint64_t at, uint8_t v) {
    if (q.bytes == 1) out[at] = v;
    else put16(out, at, q.shift == 8 ? uint32_t(v) << 8 : (uint32_t(v) << q.shift) | (uint32_t(v) >> (8 - q.shift)));
  };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const Rgba c = px(x, y);
      put_sample(uint64_t{y} * row0 + uint64_t{x} * q.bytes, luma(c.r, c.g, c.b));
    }
  // the chroma averaged over the pixels each sample covers
  const VideoPlane cp = f.plane(1);
  const uint32_t cw = (w + q.sx - 1) / q.sx, ch = (h + q.sy - 1) / q.sy;
  for (uint32_t cy = 0; cy < ch; ++cy)
    for (uint32_t cx = 0; cx < cw; ++cx) {
      int32_t rs = 0, gs = 0, bs = 0, n = 0;
      for (uint32_t dy = 0; dy < q.sy; ++dy)
        for (uint32_t dx = 0; dx < q.sx; ++dx) {
          const uint32_t x = cx * q.sx + dx, y = cy * q.sy + dy;
          if (x >= w || y >= h) continue;
          const Rgba c = px(x, y);
          rs += c.r;
          gs += c.g;
          bs += c.b;
          ++n;
        }
      const uint8_t U = chroma_u(rs / n, gs / n, bs / n), V = chroma_v(rs / n, gs / n, bs / n);
      if (q.semi) {
        const uint64_t at = f.plane_offset(1) + uint64_t{cy} * cp.row_bytes + uint64_t{cx} * 2 * q.bytes;
        put_sample(at, q.v_first ? V : U);
        put_sample(at + q.bytes, q.v_first ? U : V);
      } else {
        put_sample(f.plane_offset(1) + uint64_t{cy} * cp.row_bytes + uint64_t{cx} * q.bytes, q.v_first ? V : U);
        put_sample(f.plane_offset(2) + uint64_t{cy} * cp.row_bytes + uint64_t{cx} * q.bytes, q.v_first ? U : V);
      }
    }
}

}  // namespace

bool can_convert_video(uint32_t from, uint32_t to) { return known(from) && known(to); }

bool convert_video(const VideoFormat& from, ConstByteView in, const VideoFormat& to, ByteBuffer* out) {
  if (from.encoded() || to.encoded() || !from.valid() || !to.valid() || !can_convert_video(from.format, to.format)) return false;
  if (from.width != to.width || from.height != to.height) return false;
  if (in.size() < from.frame_bytes()) return false;
  out->clear();
  if (from.format == to.format) return out->append(in.first(from.frame_bytes()));
  ByteBuffer rgba(out->allocator());
  if (!rgba.put_zeros(uint64_t{from.width} * from.height * 4) || !out->put_zeros(to.frame_bytes())) return false;
  to_rgba(from, in, rgba.writable_view());
  from_rgba(to, rgba.view(), out->writable_view());
  return true;
}

bool can_decode_video(const VideoFormat& from, uint32_t to) { return from.subtype == kSubtypeMjpg && can_convert_video(kBGRA, to); }

bool decode_video(ConstByteView frame, const VideoFormat& to, hardware::JpegDecoder& decoder, List<uint32_t>* scratch, ByteBuffer* out) {
  if (to.encoded() || !to.valid() || !decoder.decode(frame) || decoder.width() != to.width || decoder.height() != to.height) return false;
  const uint64_t pixels = uint64_t{to.width} * to.height;
  if (scratch->count() < pixels) {
    if (!scratch->reserve(pixels)) return false;
    while (scratch->count() < pixels)
      if (!scratch->push_back(0)) return false;
  }
  ArrayView<uint32_t> words = scratch->view().first(pixels);
  if (!decoder.output_bgra(words.data(), to.width)) return false;
  VideoFormat bgra = to;
  bgra.format = kBGRA;
  return convert_video(bgra, words_as_bytes(ArrayView<const uint32_t>(words)), to, out);
}

}  // namespace pipewire
