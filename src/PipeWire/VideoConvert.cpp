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
  kI420 = 2, kYV12 = 3, kYUY2 = 4, kUYVY = 5, kRGBx = 7, kBGRx = 8, kxRGB = 9, kxBGR = 10, kRGBA = 11, kBGRA = 12, kARGB = 13, kABGR = 14,
  kRGB = 15, kBGR = 16, kYVYU = 19, kNV12 = 23, kNV21 = 24, kGRAY8 = 25
};

// Where a packed RGB format keeps red, green, blue and alpha in a pixel (alpha -1: none, opaque), and the pixel's bytes.
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
// Where a packed 4:2:2 format keeps its first and second luma and its U and V in each four bytes.
struct PackedYuv {
  uint8_t y0, u, y1, v;
};
bool packed_yuv(uint32_t format, PackedYuv* out) {
  switch (format) {
    case kYUY2: *out = {0, 1, 2, 3}; return true;
    case kUYVY: *out = {1, 0, 3, 2}; return true;
    case kYVYU: *out = {0, 3, 2, 1}; return true;
    default: return false;
  }
}
bool known(uint32_t format) {
  RgbLayout l;
  PackedYuv p;
  return rgb_layout(format, &l) || packed_yuv(format, &p) || format == kI420 || format == kYV12 || format == kNV12 || format == kNV21 || format == kGRAY8;
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

// The frame as RGBA (4 bytes a pixel, row after row). `in` holds every plane (checked by the caller).
void to_rgba(const VideoFormat& f, ConstByteView in, ByteView rgba) {
  const uint32_t w = f.width, h = f.height;
  RgbLayout l;
  PackedYuv p;
  if (rgb_layout(f.format, &l)) {
    for (uint32_t y = 0; y < h; ++y) {
      const ConstByteView row = in.subview(uint64_t{y} * w * l.bytes, uint64_t{w} * l.bytes);
      ByteView out = rgba.subview(uint64_t{y} * w * 4, uint64_t{w} * 4);
      for (uint32_t x = 0; x < w; ++x) {
        const ConstByteView px = row.subview(uint64_t{x} * l.bytes, l.bytes);
        out[4ull * x] = px[static_cast<uint64_t>(l.r)];
        out[4ull * x + 1] = px[static_cast<uint64_t>(l.g)];
        out[4ull * x + 2] = px[static_cast<uint64_t>(l.b)];
        out[4ull * x + 3] = l.a >= 0 ? px[static_cast<uint64_t>(l.a)] : 0xFF;
      }
    }
    return;
  }
  if (f.format == kGRAY8) {
    for (uint64_t i = 0; i < uint64_t{w} * h; ++i) {
      const uint8_t v = in[i];
      rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = v;
      rgba[4 * i + 3] = 0xFF;
    }
    return;
  }
  const VideoPlane y_plane = f.plane(0);
  for (uint32_t y = 0; y < h; ++y) {
    ByteView out = rgba.subview(uint64_t{y} * w * 4, uint64_t{w} * 4);
    for (uint32_t x = 0; x < w; ++x) {
      int32_t Y = 0, U = 128, V = 128;
      if (packed_yuv(f.format, &p)) {
        const ConstByteView pair = in.subview(uint64_t{y} * y_plane.row_bytes + 4ull * (x / 2), 4);
        Y = pair[x % 2 ? p.y1 : p.y0];
        U = pair[p.u];
        V = pair[p.v];
      } else {
        Y = in[uint64_t{y} * w + x];
        const VideoPlane c = f.plane(1);
        const uint64_t cy = y / 2, cx = x / 2;
        if (f.format == kNV12 || f.format == kNV21) {
          const ConstByteView uv = in.subview(f.plane_offset(1) + cy * c.row_bytes + 2 * cx, 2);
          U = uv[f.format == kNV12 ? 0 : 1];
          V = uv[f.format == kNV12 ? 1 : 0];
        } else {      // I420: U then V; YV12: V then U
          const uint64_t first = f.plane_offset(1) + cy * c.row_bytes + cx, second = f.plane_offset(2) + cy * c.row_bytes + cx;
          U = in[f.format == kI420 ? first : second];
          V = in[f.format == kI420 ? second : first];
        }
      }
      uint8_t r, g, b;
      yuv_to_rgb(Y, U, V, &r, &g, &b);
      out[4ull * x] = r;
      out[4ull * x + 1] = g;
      out[4ull * x + 2] = b;
      out[4ull * x + 3] = 0xFF;
    }
  }
}

// The RGBA frame in `f`'s layout (`out` is f.frame_bytes()).
void from_rgba(const VideoFormat& f, ConstByteView rgba, ByteView out) {
  const uint32_t w = f.width, h = f.height;
  auto px = [&](uint32_t x, uint32_t y, int32_t* r, int32_t* g, int32_t* b, int32_t* a) {
    const ConstByteView q = rgba.subview((uint64_t{y} * w + x) * 4, 4);
    *r = q[0];
    *g = q[1];
    *b = q[2];
    if (a) *a = q[3];
  };
  RgbLayout l;
  PackedYuv p;
  if (rgb_layout(f.format, &l)) {
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        int32_t r, g, b, a;
        px(x, y, &r, &g, &b, &a);
        ByteView o = out.subview((uint64_t{y} * w + x) * l.bytes, l.bytes);
        if (l.bytes == 4) o.fill(0xFF);      // (the x byte)
        o[static_cast<uint64_t>(l.r)] = static_cast<uint8_t>(r);
        o[static_cast<uint64_t>(l.g)] = static_cast<uint8_t>(g);
        o[static_cast<uint64_t>(l.b)] = static_cast<uint8_t>(b);
        if (l.a >= 0) o[static_cast<uint64_t>(l.a)] = static_cast<uint8_t>(a);
      }
    return;
  }
  if (f.format == kGRAY8) {
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        int32_t r, g, b;
        px(x, y, &r, &g, &b, nullptr);
        out[uint64_t{y} * w + x] = clamp8((77 * r + 150 * g + 29 * b + 128) >> 8);      // (GRAY8 is full range)
      }
    return;
  }
  if (packed_yuv(f.format, &p)) {
    const uint32_t row = f.plane(0).row_bytes;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; x += 2) {
        int32_t r0, g0, b0, r1, g1, b1;
        px(x, y, &r0, &g0, &b0, nullptr);
        if (x + 1 < w) px(x + 1, y, &r1, &g1, &b1, nullptr);
        else r1 = r0, g1 = g0, b1 = b0;
        ByteView pair = out.subview(uint64_t{y} * row + 2ull * x, 4);
        pair[p.y0] = luma(r0, g0, b0);
        pair[p.y1] = luma(r1, g1, b1);
        pair[p.u] = chroma_u((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        pair[p.v] = chroma_v((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
      }
    return;
  }
  // 4:2:0: the luma plane, then chroma averaged over each 2 x 2 block
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      int32_t r, g, b;
      px(x, y, &r, &g, &b, nullptr);
      out[uint64_t{y} * w + x] = luma(r, g, b);
    }
  const VideoPlane c = f.plane(1);
  for (uint32_t cy = 0; cy < (h + 1) / 2; ++cy)
    for (uint32_t cx = 0; cx < (w + 1) / 2; ++cx) {
      int32_t rs = 0, gs = 0, bs = 0, n = 0;
      for (uint32_t dy = 0; dy < 2; ++dy)
        for (uint32_t dx = 0; dx < 2; ++dx) {
          const uint32_t x = 2 * cx + dx, y = 2 * cy + dy;
          if (x >= w || y >= h) continue;
          int32_t r, g, b;
          px(x, y, &r, &g, &b, nullptr);
          rs += r;
          gs += g;
          bs += b;
          ++n;
        }
      const uint8_t U = chroma_u(rs / n, gs / n, bs / n), V = chroma_v(rs / n, gs / n, bs / n);
      if (f.format == kNV12 || f.format == kNV21) {
        ByteView uv = out.subview(f.plane_offset(1) + uint64_t{cy} * c.row_bytes + 2ull * cx, 2);
        uv[0] = f.format == kNV12 ? U : V;
        uv[1] = f.format == kNV12 ? V : U;
      } else {
        out[f.plane_offset(1) + uint64_t{cy} * c.row_bytes + cx] = f.format == kI420 ? U : V;
        out[f.plane_offset(2) + uint64_t{cy} * c.row_bytes + cx] = f.format == kI420 ? V : U;
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

}  // namespace pipewire
