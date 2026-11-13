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
// A planar or semi-planar YUV format: its chroma's subsampling, a sample's bytes (1, or 2 little-endian), its bits, whether a 16-bit sample holds
// them in its top bits (P010) rather than its bottom ones, whether U and V are one interleaved plane, and whether V comes first.
struct PlanarYuv {
  uint8_t sx, sy, bytes, depth;
  bool msb, semi, v_first;
};
bool planar_yuv(uint32_t format, PlanarYuv* out) {
  switch (format) {
    case kI420: *out = {2, 2, 1, 8, false, false, false}; return true;
    case kYV12: *out = {2, 2, 1, 8, false, false, true}; return true;
    case kY41B: *out = {4, 1, 1, 8, false, false, false}; return true;
    case kY42B: *out = {2, 1, 1, 8, false, false, false}; return true;
    case kY444: *out = {1, 1, 1, 8, false, false, false}; return true;
    case kNV12: *out = {2, 2, 1, 8, false, true, false}; return true;
    case kNV21: *out = {2, 2, 1, 8, false, true, true}; return true;
    case kNV16: *out = {2, 1, 1, 8, false, true, false}; return true;
    case kNV61: *out = {2, 1, 1, 8, false, true, true}; return true;
    case kNV24: *out = {1, 1, 1, 8, false, true, false}; return true;
    case kI420_10LE: *out = {2, 2, 2, 10, false, false, false}; return true;
    case kI422_10LE: *out = {2, 1, 2, 10, false, false, false}; return true;
    case kY444_10LE: *out = {1, 1, 2, 10, false, false, false}; return true;
    case kI420_12LE: *out = {2, 2, 2, 12, false, false, false}; return true;
    case kP010_10LE: *out = {2, 2, 2, 10, true, true, false}; return true;
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

// ---- the pivot: 16 bits a component (0 .. 65535), so that 10-, 12- and 16-bit formats keep their bits on the way through

uint16_t clamp16(int64_t v) { return static_cast<uint16_t>(v < 0 ? 0 : v > 65535 ? 65535 : v); }
// an n-bit value (n <= 16) to 16 bits and back, full scale to full scale (0 -> 0, all ones -> 65535), rounded
uint16_t widen(uint32_t v, uint32_t bits) { return static_cast<uint16_t>((uint64_t{v} * 65535 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }
uint32_t narrow(uint32_t v16, uint32_t bits) { return static_cast<uint32_t>((uint64_t{v16} * ((1u << bits) - 1) + 32767) / 65535); }

// BT.601 limited range, on samples scaled to 16 bits (an n-bit sample << (16 - n): black Y 4096, white 60160, chroma 32768 +- 28672), in 16.16
// fixed point: R = (Y - 4096) * 65535 / 56064 + 1.402 * (V - 32768) * 65535 / 57344, ...
void yuv_to_rgb(int64_t y, int64_t u, int64_t v, uint16_t* r, uint16_t* g, uint16_t* b) {
  const int64_t c = 76607 * (y - 4096), d = u - 32768, e = v - 32768;
  *r = clamp16((c + 105004 * e + 32768) >> 16);
  *g = clamp16((c - 25775 * d - 53488 * e + 32768) >> 16);
  *b = clamp16((c + 132720 * d + 32768) >> 16);
}
int64_t luma(int64_t r, int64_t g, int64_t b) { return ((16763 * r + 32910 * g + 6391 * b + 32768) >> 16) + 4096; }
int64_t chroma_u(int64_t r, int64_t g, int64_t b) { return ((-9676 * r - 18996 * g + 28672 * b + 32768) >> 16) + 32768; }
int64_t chroma_v(int64_t r, int64_t g, int64_t b) { return ((28672 * r - 24010 * g - 4663 * b + 32768) >> 16) + 32768; }
// full range (grey)
uint16_t grey(int64_t r, int64_t g, int64_t b) { return clamp16((19595 * r + 38470 * g + 7471 * b + 32768) >> 16); }
// a 16-bit-scaled video sample as `bits` bits (rounded, kept in range)
uint32_t video_sample(int64_t v16, uint32_t bits) {
  const uint32_t shift = 16 - bits;
  const int64_t v = shift ? (v16 + (int64_t{1} << (shift - 1))) >> shift : v16;
  const int64_t top = (int64_t{1} << bits) - 1;
  return static_cast<uint32_t>(v < 0 ? 0 : v > top ? top : v);
}

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
// IEEE half and single floats to and from 0 .. 65535 (clamped)
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
  uint32_t mantissa = static_cast<uint32_t>((f - 1.0f) * 1024.0f + 0.5f);
  if (mantissa >= 1024) {      // (rounded up to the next power of two)
    mantissa = 0;
    ++exponent;
  }
  return static_cast<uint32_t>(exponent) << 10 | mantissa;
}
uint16_t unit_to16(float f) { return clamp16(static_cast<int64_t>(f * 65535.0f + 0.5f)); }
float unit_of16(uint16_t v) { return static_cast<float>(v) / 65535.0f; }

// One pixel as 16-bit red, green, blue and alpha.
struct Rgba {
  uint16_t r, g, b, a;
};
Rgba read_rgb(const VideoFormat& f, ConstByteView in, uint32_t x, uint32_t y) {
  const uint64_t row = f.plane(0).row_bytes, at0 = uint64_t{y} * row;
  RgbLayout l;
  Rgb10Layout l10;
  if (rgb_layout(f.format, &l)) {
    const ConstByteView px = in.subview(at0 + uint64_t{x} * l.bytes, l.bytes);
    return {uint16_t(px[uint64_t(l.r)] * 257), uint16_t(px[uint64_t(l.g)] * 257), uint16_t(px[uint64_t(l.b)] * 257),
            uint16_t(l.a >= 0 ? px[uint64_t(l.a)] * 257 : 65535)};
  }
  if (rgb10_layout(f.format, &l10)) {
    const uint32_t w = le32(in, at0 + 4ull * x);
    const uint16_t a = l10.a >= 0 ? static_cast<uint16_t>(((w >> l10.a) & 3) * 21845) : 65535;
    return {widen((w >> l10.r) & 0x3FF, 10), widen((w >> l10.g) & 0x3FF, 10), widen((w >> l10.b) & 0x3FF, 10), a};
  }
  switch (f.format) {
    case kGRAY8: { const uint16_t v = uint16_t(in[at0 + x] * 257); return {v, v, v, 65535}; }
    case kGRAY16_LE: case kGRAY16_BE: {
      const uint16_t v = static_cast<uint16_t>(f.format == kGRAY16_LE ? le16(in, at0 + 2ull * x) : uint32_t(in[at0 + 2ull * x]) << 8 | in[at0 + 2ull * x + 1]);
      return {v, v, v, 65535};
    }
    case kRGB16: case kBGR16: {      // 5:6:5 in a little-endian word, red (or blue) on top
      const uint32_t w = le16(in, at0 + 2ull * x);
      const uint16_t hi = widen((w >> 11) & 31, 5), mid = widen((w >> 5) & 63, 6), lo = widen(w & 31, 5);
      return f.format == kRGB16 ? Rgba{hi, mid, lo, 65535} : Rgba{lo, mid, hi, 65535};
    }
    case kRGB15: case kBGR15: {      // x:5:5:5
      const uint32_t w = le16(in, at0 + 2ull * x);
      const uint16_t hi = widen((w >> 10) & 31, 5), mid = widen((w >> 5) & 31, 5), lo = widen(w & 31, 5);
      return f.format == kRGB15 ? Rgba{hi, mid, lo, 65535} : Rgba{lo, mid, hi, 65535};
    }
    case kARGB64: {      // A, R, G, B of 16 bits each (little-endian)
      const uint64_t at = at0 + 8ull * x;
      return {uint16_t(le16(in, at + 2)), uint16_t(le16(in, at + 4)), uint16_t(le16(in, at + 6)), uint16_t(le16(in, at))};
    }
    case kRGBA_F16: {
      const uint64_t at = at0 + 8ull * x;
      return {unit_to16(half_to_float(le16(in, at))), unit_to16(half_to_float(le16(in, at + 2))), unit_to16(half_to_float(le16(in, at + 4))),
              unit_to16(half_to_float(le16(in, at + 6)))};
    }
    case kRGBA_F32: {
      const uint64_t at = at0 + 16ull * x;
      return {unit_to16(__builtin_bit_cast(float, le32(in, at))), unit_to16(__builtin_bit_cast(float, le32(in, at + 4))),
              unit_to16(__builtin_bit_cast(float, le32(in, at + 8))), unit_to16(__builtin_bit_cast(float, le32(in, at + 12)))};
    }
    default: return {0, 0, 0, 65535};
  }
}
void write_rgb(const VideoFormat& f, ByteView out, uint32_t x, uint32_t y, Rgba c) {
  const uint64_t row = f.plane(0).row_bytes, at0 = uint64_t{y} * row;
  RgbLayout l;
  Rgb10Layout l10;
  if (rgb_layout(f.format, &l)) {
    ByteView o = out.subview(at0 + uint64_t{x} * l.bytes, l.bytes);
    if (l.bytes == 4) o.fill(0xFF);      // (the x byte)
    o[uint64_t(l.r)] = static_cast<uint8_t>(narrow(c.r, 8));
    o[uint64_t(l.g)] = static_cast<uint8_t>(narrow(c.g, 8));
    o[uint64_t(l.b)] = static_cast<uint8_t>(narrow(c.b, 8));
    if (l.a >= 0) o[uint64_t(l.a)] = static_cast<uint8_t>(narrow(c.a, 8));
    return;
  }
  if (rgb10_layout(f.format, &l10)) {
    uint32_t w = narrow(c.r, 10) << l10.r | narrow(c.g, 10) << l10.g | narrow(c.b, 10) << l10.b;
    w |= (l10.a >= 0 ? narrow(c.a, 2) : 3u) << (l10.a >= 0 ? l10.a : (l10.r == 22 || l10.b == 22 ? 0 : 30));      // (alpha, or the x bits all set)
    put32(out, at0 + 4ull * x, w);
    return;
  }
  switch (f.format) {
    case kGRAY8: out[at0 + x] = static_cast<uint8_t>(narrow(grey(c.r, c.g, c.b), 8)); return;      // (grey is full range)
    case kGRAY16_LE: case kGRAY16_BE: {
      const uint32_t v = grey(c.r, c.g, c.b);
      if (f.format == kGRAY16_LE) put16(out, at0 + 2ull * x, v);
      else { out[at0 + 2ull * x] = static_cast<uint8_t>(v >> 8); out[at0 + 2ull * x + 1] = static_cast<uint8_t>(v); }
      return;
    }
    case kRGB16: case kBGR16: {
      const uint32_t hi = narrow(f.format == kRGB16 ? c.r : c.b, 5), lo = narrow(f.format == kRGB16 ? c.b : c.r, 5);
      put16(out, at0 + 2ull * x, hi << 11 | narrow(c.g, 6) << 5 | lo);
      return;
    }
    case kRGB15: case kBGR15: {
      const uint32_t hi = narrow(f.format == kRGB15 ? c.r : c.b, 5), lo = narrow(f.format == kRGB15 ? c.b : c.r, 5);
      put16(out, at0 + 2ull * x, 0x8000u | hi << 10 | narrow(c.g, 5) << 5 | lo);
      return;
    }
    case kARGB64: {
      const uint64_t at = at0 + 8ull * x;
      put16(out, at, c.a);
      put16(out, at + 2, c.r);
      put16(out, at + 4, c.g);
      put16(out, at + 6, c.b);
      return;
    }
    case kRGBA_F16: {
      const uint64_t at = at0 + 8ull * x;
      const uint16_t v[4] = {c.r, c.g, c.b, c.a};
      for (int k = 0; k < 4; ++k) put16(out, at + 2ull * k, float_to_half(unit_of16(v[k])));
      return;
    }
    case kRGBA_F32: {
      const uint64_t at = at0 + 16ull * x;
      const uint16_t v[4] = {c.r, c.g, c.b, c.a};
      for (int k = 0; k < 4; ++k) put32(out, at + 4ull * k, __builtin_bit_cast(uint32_t, unit_of16(v[k])));
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
// One pixel's Y, U, V scaled to 16 bits (a sample << (16 - its bits)) and its alpha (0 .. 65535), from a YUV frame.
void read_yuv(const VideoFormat& f, ConstByteView in, uint32_t x, uint32_t y, int64_t* Y, int64_t* U, int64_t* V, uint16_t* A) {
  *A = 65535;
  PackedYuv p;
  PlanarYuv q;
  const uint64_t row0 = f.plane(0).row_bytes;
  if (packed_yuv(f.format, &p)) {
    const ConstByteView pair = in.subview(uint64_t{y} * row0 + 4ull * (x / 2), 4);
    *Y = int64_t{pair[x % 2 ? p.y1 : p.y0]} << 8;
    *U = int64_t{pair[p.u]} << 8;
    *V = int64_t{pair[p.v]} << 8;
    return;
  }
  if (f.format == kAYUV) {
    const ConstByteView px = in.subview(uint64_t{y} * row0 + 4ull * x, 4);
    *A = static_cast<uint16_t>(px[0] * 257);
    *Y = int64_t{px[1]} << 8;
    *U = int64_t{px[2]} << 8;
    *V = int64_t{px[3]} << 8;
    return;
  }
  if (f.format == kAYUV64) {      // (16-bit little-endian samples)
    const uint64_t at = uint64_t{y} * row0 + 8ull * x;
    *A = static_cast<uint16_t>(le16(in, at));
    *Y = le16(in, at + 2);
    *U = le16(in, at + 4);
    *V = le16(in, at + 6);
    return;
  }
  planar_yuv(f.format, &q);
  auto sample = [&](uint64_t at) -> int64_t {
    const uint32_t raw = q.bytes == 1 ? in[at] : le16(in, at);
    const uint32_t v = q.msb ? raw >> (16 - q.depth) : raw & ((1u << q.depth) - 1);
    return int64_t{v} << (16 - q.depth);
  };
  *Y = sample(uint64_t{y} * row0 + uint64_t{x} * q.bytes);
  const uint64_t cx = x / q.sx, cy = y / q.sy;
  const VideoPlane c = f.plane(1);
  if (q.semi) {
    const uint64_t at = f.plane_offset(1) + cy * c.row_bytes + cx * 2 * q.bytes;
    const int64_t first = sample(at), second = sample(at + q.bytes);
    *U = q.v_first ? second : first;
    *V = q.v_first ? first : second;
  } else {
    const int64_t first = sample(f.plane_offset(1) + cy * c.row_bytes + cx * q.bytes), second = sample(f.plane_offset(2) + cy * c.row_bytes + cx * q.bytes);
    *U = q.v_first ? second : first;
    *V = q.v_first ? first : second;
  }
}

// The frame as 16-bit RGBA (4 components a pixel, row after row). `in` holds every plane (checked by the caller).
void to_rgba(const VideoFormat& f, ConstByteView in, ArrayView<uint16_t> rgba) {
  const uint32_t w = f.width, h = f.height;
  const bool yuv = is_yuv(f.format);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      ArrayView<uint16_t> out = rgba.subview((uint64_t{y} * w + x) * 4, 4);
      if (yuv) {
        int64_t Y, U, V;
        uint16_t a;
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

// The 16-bit RGBA frame in `f`'s layout (`out` is f.frame_bytes()).
void from_rgba(const VideoFormat& f, ArrayView<const uint16_t> rgba, ByteView out) {
  const uint32_t w = f.width, h = f.height;
  auto px = [&](uint32_t x, uint32_t y) {
    const ArrayView<const uint16_t> q = rgba.subview((uint64_t{y} * w + x) * 4, 4);
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
        const int64_t r = (int64_t{a.r} + b.r) / 2, g = (int64_t{a.g} + b.g) / 2, bl = (int64_t{a.b} + b.b) / 2;
        ByteView pair = out.subview(uint64_t{y} * row0 + 2ull * x, 4);
        pair[p.y0] = static_cast<uint8_t>(video_sample(luma(a.r, a.g, a.b), 8));
        pair[p.y1] = static_cast<uint8_t>(video_sample(luma(b.r, b.g, b.b), 8));
        pair[p.u] = static_cast<uint8_t>(video_sample(chroma_u(r, g, bl), 8));
        pair[p.v] = static_cast<uint8_t>(video_sample(chroma_v(r, g, bl), 8));
      }
    return;
  }
  if (f.format == kAYUV || f.format == kAYUV64) {
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const Rgba c = px(x, y);
        if (f.format == kAYUV) {
          ByteView o = out.subview(uint64_t{y} * row0 + 4ull * x, 4);
          o[0] = static_cast<uint8_t>(narrow(c.a, 8));
          o[1] = static_cast<uint8_t>(video_sample(luma(c.r, c.g, c.b), 8));
          o[2] = static_cast<uint8_t>(video_sample(chroma_u(c.r, c.g, c.b), 8));
          o[3] = static_cast<uint8_t>(video_sample(chroma_v(c.r, c.g, c.b), 8));
        } else {
          const uint64_t at = uint64_t{y} * row0 + 8ull * x;
          put16(out, at, c.a);
          put16(out, at + 2, video_sample(luma(c.r, c.g, c.b), 16));
          put16(out, at + 4, video_sample(chroma_u(c.r, c.g, c.b), 16));
          put16(out, at + 6, video_sample(chroma_v(c.r, c.g, c.b), 16));
        }
      }
    return;
  }
  PlanarYuv q;
  planar_yuv(f.format, &q);
  auto put_sample = [&](uint64_t at, int64_t v16) {
    const uint32_t v = video_sample(v16, q.depth);
    if (q.bytes == 1) out[at] = static_cast<uint8_t>(v);
    else put16(out, at, q.msb ? v << (16 - q.depth) : v);
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
      int64_t rs = 0, gs = 0, bs = 0, n = 0;
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
      const int64_t U = chroma_u(rs / n, gs / n, bs / n), V = chroma_v(rs / n, gs / n, bs / n);
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

// Which source pixels one output pixel takes along one axis, and how much of each: on an axis that shrinks (or keeps its size) the ones it covers,
// alike (an average); on one that grows the two nearest its centre, linearly (bilinear).
struct AxisTaps {
  uint32_t first = 0, count = 0;      // (box: count pixels from first; linear: first and first + 1 when count is 2)
  uint32_t weight = 0;                // (linear: the second's share of 256)
  bool linear = false;
};
AxisTaps axis_taps(uint32_t i, uint32_t source, uint32_t dest) {
  AxisTaps t;
  const uint64_t p0 = uint64_t{i} * source * 256 / dest, p1 = uint64_t{i + 1} * source * 256 / dest;      // (in 1/256 of a source pixel)
  if (p1 - p0 >= 256) {
    t.first = static_cast<uint32_t>(p0 / 256);
    const uint32_t end = static_cast<uint32_t>((p1 + 255) / 256);
    t.count = (end > source ? source : end) - t.first;
    if (t.count == 0) t.count = 1;
    return t;
  }
  const int64_t centre = static_cast<int64_t>((p0 + p1) / 2) - 128;
  const uint64_t c = centre < 0 ? 0 : static_cast<uint64_t>(centre);
  t.linear = true;
  t.first = static_cast<uint32_t>(c / 256);
  if (t.first >= source - 1) {
    t.first = source - 1;
    t.count = 1;
    return t;
  }
  t.count = 2;
  t.weight = static_cast<uint32_t>(c % 256);
  return t;
}

// A 16-bit RGBA frame of sw x sh as one of dw x dh, axis by axis (averaged where it shrinks, bilinear where it grows), in fixed point.
void scale_rgba(ArrayView<const uint16_t> in, uint32_t sw, uint32_t sh, ArrayView<uint16_t> out, uint32_t dw, uint32_t dh) {
  for (uint32_t y = 0; y < dh; ++y) {
    const AxisTaps ty = axis_taps(y, sh, dh);
    for (uint32_t x = 0; x < dw; ++x) {
      const AxisTaps tx = axis_taps(x, sw, dw);
      uint64_t sum[4] = {0, 0, 0, 0}, total = 0;
      for (uint32_t j = 0; j < ty.count; ++j) {
        const uint64_t wy = ty.linear ? (j == 0 ? 256 - ty.weight : ty.weight) : 256;
        for (uint32_t k = 0; k < tx.count; ++k) {
          const uint64_t wx = tx.linear ? (k == 0 ? 256 - tx.weight : tx.weight) : 256;
          const ArrayView<const uint16_t> q = in.subview((uint64_t{ty.first + j} * sw + tx.first + k) * 4, 4);
          for (uint64_t c = 0; c < 4; ++c) sum[c] += q[c] * wx * wy;
          total += wx * wy;
        }
      }
      ArrayView<uint16_t> o = out.subview((uint64_t{y} * dw + x) * 4, 4);
      for (uint64_t c = 0; c < 4; ++c) o[c] = static_cast<uint16_t>(total ? (sum[c] + total / 2) / total : 0);
    }
  }
}

// A pivot frame of `pixels` pixels.
bool pivot(List<uint16_t>* p, uint64_t pixels) {
  p->clear();
  if (pixels > UINT64_MAX / 4 || !p->reserve(pixels * 4)) return false;
  for (uint64_t i = 0; i < pixels * 4; ++i)
    if (!p->push_back(0)) return false;
  return true;
}

}  // namespace

bool can_convert_video(uint32_t from, uint32_t to) { return known(from) && known(to); }

bool convert_video(const VideoFormat& from, ConstByteView in, const VideoFormat& to, ByteBuffer* out) {
  if (from.encoded() || to.encoded() || !from.valid() || !to.valid() || !can_convert_video(from.format, to.format)) return false;
  if (in.size() < from.frame_bytes()) return false;
  out->clear();
  const bool same_size = from.width == to.width && from.height == to.height;
  if (from.format == to.format && same_size) return out->append(in.first(from.frame_bytes()));
  List<uint16_t> rgba(out->allocator());
  if (!pivot(&rgba, uint64_t{from.width} * from.height) || !out->put_zeros(to.frame_bytes())) return false;
  to_rgba(from, in, rgba.view());
  if (!same_size) {      // (another size: scaled as RGBA)
    List<uint16_t> scaled(out->allocator());
    if (!pivot(&scaled, uint64_t{to.width} * to.height)) return false;
    scale_rgba(ArrayView<const uint16_t>(rgba.view()), from.width, from.height, scaled.view(), to.width, to.height);
    from_rgba(to, ArrayView<const uint16_t>(scaled.view()), out->writable_view());
    return true;
  }
  from_rgba(to, ArrayView<const uint16_t>(rgba.view()), out->writable_view());
  return true;
}

bool can_decode_video(const VideoFormat& from, uint32_t to, const IVideoDecoders* decoders) {
  return from.encoded() && can_decode_subtype(from.subtype, decoders) && can_convert_video(kBGRA, to);
}

bool decode_video(ConstByteView frame, const VideoFormat& from, const VideoFormat& to, VideoDecodeSession& session, IVideoDecoders* decoders,
                  ByteBuffer* out, bool* failed) {
  *failed = false;
  if (to.encoded() || !to.valid()) {
    *failed = true;
    return false;
  }
  VideoFormat picture;
  ConstByteView pixels;
  if (!session.decode(from.subtype, frame, decoders, &picture, &pixels, failed)) return false;
  if (!convert_video(picture, pixels, to, out)) {      // (to its format and size)
    *failed = true;
    return false;
  }
  return true;
}

}  // namespace pipewire
