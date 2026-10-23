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

// Pixel maths shared by the 2D renderers (header only, no libm): exact byte arithmetic, rounded-rectangle
// coverage, gradient color lookup and image sampling. Keeping it in one place makes the CPU and the device
// renderer agree on what a rounded rectangle or a gradient looks like.
#pragma once
#include "Graphics/Graphics2D/IRenderer2D.hpp"

namespace graphics::raster {

inline int32_t min_i(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t max_i(int32_t a, int32_t b) { return a > b ? a : b; }
inline float clampf(float v, float lo, float hi) { return !(v > lo) ? lo : (v > hi ? hi : v); }  // NaN -> lo

// round(v / 255) for 0 <= v <= 65025, without a divide.
inline uint32_t div255(uint32_t v) {
  v += 128;
  return (v + (v >> 8)) >> 8;
}
// floor(v / 255) for 0 <= v <= 65534, without a divide.
inline uint32_t div255_floor(uint32_t v) { return (v + 1 + (v >> 8)) >> 8; }

// `argb` with its alpha multiplied by k / 255.
inline uint32_t scale_alpha(uint32_t argb, uint32_t k) {
  return (argb & 0x00FFFFFFu) | (div255((argb >> 24) * k) << 24);
}

// 1 / sqrt(x) for x > 0 (bit-trick seed, three Newton steps: about 1e-7 relative error).
inline float rsqrt(float x) {
  const float half = 0.5f * x;
  float y = __builtin_bit_cast(float, 0x5f3759dfu - (__builtin_bit_cast(uint32_t, x) >> 1));
  y *= 1.5f - half * y * y;
  y *= 1.5f - half * y * y;
  y *= 1.5f - half * y * y;
  return y;
}
inline float sqrt_f(float x) { return x > 0 ? x * rsqrt(x) : 0.0f; }

inline int32_t floor_i(float v) {
  v = clampf(v, -1.0e9f, 1.0e9f);
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<float>(i) > v ? i - 1 : i;
}
inline int32_t ceil_i(float v) {
  v = clampf(v, -1.0e9f, 1.0e9f);
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<float>(i) < v ? i + 1 : i;
}

inline int32_t saturate_i32(int64_t v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : (v < -0x7FFFFFFF ? -0x7FFFFFFF : static_cast<int32_t>(v)); }

// Intersection of two rectangles (width/height 0 when disjoint). Safe for any int32 position and uint32 size.
inline Rect2D intersect(const Rect2D& a, const Rect2D& b) {
  const int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
  const int64_t ax1 = int64_t{a.x} + a.width, bx1 = int64_t{b.x} + b.width;
  const int64_t ay1 = int64_t{a.y} + a.height, by1 = int64_t{b.y} + b.height;
  const int64_t x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
  if (x1 <= x0 || y1 <= y0) return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), 0, 0};
  return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
}

// ---- Rounded rectangles -------------------------------------------------------------------------------

// A rounded rectangle in pixel edges [x0, x1) x [y0, y1) with per-corner ellipse radii (0 TL, 1 TR, 2 BR, 3 BL).
struct RRect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float rx[4] = {}, ry[4] = {}, irx[4] = {}, iry[4] = {};  // radius and its inverse; rx == 0 is a square corner

  bool empty() const { return x1 <= x0 || y1 <= y0; }

  void set_corner(int i, float x, float y) {
    if (x > 1e-3f && y > 1e-3f) {
      rx[i] = x, ry[i] = y, irx[i] = 1.0f / x, iry[i] = 1.0f / y;
    } else {
      rx[i] = ry[i] = irx[i] = iry[i] = 0;
    }
  }

  static RRect make(const Rect2D& r, const CornerRadii& c) {
    RRect o;
    o.x0 = r.x, o.y0 = r.y;
    o.x1 = saturate_i32(int64_t{r.x} + r.width), o.y1 = saturate_i32(int64_t{r.y} + r.height);
    float v[8] = {c.tl_x, c.tl_y, c.tr_x, c.tr_y, c.br_x, c.br_y, c.bl_x, c.bl_y};
    for (float& f : v) f = f > 0 ? f : 0;  // also NaN
    const float w = static_cast<float>(r.width), h = static_cast<float>(r.height);
    float scale = 1.0f;
    const float top = v[0] + v[2], bottom = v[6] + v[4], left = v[1] + v[7], right = v[3] + v[5];
    if (top > w) scale = scale < w / top ? scale : w / top;
    if (bottom > w) scale = scale < w / bottom ? scale : w / bottom;
    if (left > h) scale = scale < h / left ? scale : h / left;
    if (right > h) scale = scale < h / right ? scale : h / right;
    for (int i = 0; i < 4; ++i) o.set_corner(i, v[2 * i] * scale, v[2 * i + 1] * scale);
    return o;
  }

  // The rectangle inset by the border widths; corner radii shrink by the adjacent widths.
  static RRect inset(const RRect& o, const EdgeWidths& w) {
    RRect r;
    r.x0 = saturate_i32(int64_t{o.x0} + w.left), r.y0 = saturate_i32(int64_t{o.y0} + w.top);
    r.x1 = saturate_i32(int64_t{o.x1} - w.right), r.y1 = saturate_i32(int64_t{o.y1} - w.bottom);
    const float l = static_cast<float>(w.left), t = static_cast<float>(w.top), rt = static_cast<float>(w.right),
                b = static_cast<float>(w.bottom);
    r.set_corner(0, o.rx[0] - l, o.ry[0] - t);
    r.set_corner(1, o.rx[1] - rt, o.ry[1] - t);
    r.set_corner(2, o.rx[2] - rt, o.ry[2] - b);
    r.set_corner(3, o.rx[3] - l, o.ry[3] - b);
    return r;
  }

  // Coverage 0..255 of pixel (px, py): the signed distance from the pixel center to the curve (first order
  // estimate) maps 0.5 outside .. 0.5 inside onto 0 .. 255.
  uint32_t coverage(int32_t px, int32_t py) const {
    if (px < x0 || px >= x1 || py < y0 || py >= y1) return 0;
    const float fx = static_cast<float>(px) + 0.5f, fy = static_cast<float>(py) + 0.5f;
    for (int i = 0; i < 4; ++i) {
      if (rx[i] == 0) continue;
      const bool right = i == 1 || i == 2, bottom = i == 2 || i == 3;
      const float cx = right ? static_cast<float>(x1) - rx[i] : static_cast<float>(x0) + rx[i];
      const float cy = bottom ? static_cast<float>(y1) - ry[i] : static_cast<float>(y0) + ry[i];
      if (right ? fx <= cx : fx >= cx) continue;
      if (bottom ? fy <= cy : fy >= cy) continue;
      const float u = (fx - cx) * irx[i], v = (fy - cy) * iry[i];
      const float q2 = u * u + v * v;
      const float gx = u * irx[i], gy = v * iry[i];
      const float g2 = gx * gx + gy * gy;
      if (!(g2 > 0)) return 255;
      const float c = 0.5f - (q2 - 1.0f) * 0.5f * rsqrt(g2);
      return static_cast<uint32_t>(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return 255;
  }

  // Pixels on row y that a corner may make partly covered: [x0, x0 + left) and [x1 - right, x1).
  void corner_zones(int32_t y, int32_t* left, int32_t* right) const {
    int32_t l = 0, r = 0;
    if (rx[0] > 0 && y < saturate_i32(int64_t{y0} + ceil_i(ry[0]))) l = max_i(l, ceil_i(rx[0]));
    if (rx[3] > 0 && y >= saturate_i32(int64_t{y1} - ceil_i(ry[3]))) l = max_i(l, ceil_i(rx[3]));
    if (rx[1] > 0 && y < saturate_i32(int64_t{y0} + ceil_i(ry[1]))) r = max_i(r, ceil_i(rx[1]));
    if (rx[2] > 0 && y >= saturate_i32(int64_t{y1} - ceil_i(ry[2]))) r = max_i(r, ceil_i(rx[2]));
    *left = l, *right = r;
  }
};

// Walks the rows of `outer` (minus `hole`, when given and non-empty) between clip rows [row0, row1) and reports
// fully covered runs through sink.solid(y, xa, xb) and partly covered pixels through sink.pixel(x, y, cov)
// (cov 1..255). Only the pixels near curves are evaluated one by one. x positions are not clipped.
template <class Sink>
void rrect_rows(const RRect& outer, const RRect* hole, int32_t row0, int32_t row1, Sink& sink) {
  if (outer.empty()) return;
  const bool has_hole = hole && !hole->empty();
  const int32_t ya = max_i(outer.y0, row0), yb = min_i(outer.y1, row1);
  const int64_t width = int64_t{outer.x1} - outer.x0;
  auto pixel = [&](int32_t x, int32_t y) {
    uint32_t cov = outer.coverage(x, y);
    if (has_hole && cov) cov = raster::div255(cov * (255 - hole->coverage(x, y)));
    if (cov) sink.pixel(x, y, cov);
  };
  for (int32_t y = ya; y < yb; ++y) {
    int32_t zl = 0, zr = 0;
    outer.corner_zones(y, &zl, &zr);
    const bool hole_row = has_hole && y >= hole->y0 && y < hole->y1;
    if (hole_row) {
      int32_t hl = 0, hr = 0;
      hole->corner_zones(y, &hl, &hr);
      zl = max_i(zl, saturate_i32((int64_t{hole->x0} - outer.x0) + hl));
      zr = max_i(zr, saturate_i32((int64_t{outer.x1} - hole->x1) + hr));
    }
    if (int64_t{zl} + zr >= width) {
      for (int32_t x = outer.x0; x < outer.x1; ++x) pixel(x, y);
      continue;
    }
    for (int32_t x = outer.x0; x < outer.x0 + zl; ++x) pixel(x, y);
    for (int32_t x = outer.x1 - zr; x < outer.x1; ++x) pixel(x, y);
    if (!hole_row) sink.solid(y, outer.x0 + zl, outer.x1 - zr);
  }
}

// ---- Gradients ------------------------------------------------------------------------------------------

struct GradientPrep {
  uint32_t count = 0;
  float offset[kMaxGradientStops] = {};
  uint32_t color[kMaxGradientStops] = {};
  float inv[kMaxGradientStops] = {};  // 1 / (offset[i + 1] - offset[i]), 0 for a hard stop

  bool build(ArrayView<const GradientStop> stops) {
    count = 0;
    float prev = 0;
    for (const GradientStop& s : stops) {
      if (count == kMaxGradientStops) break;
      float o = clampf(s.offset, 0.0f, 1.0f);
      if (o < prev) o = prev;
      offset[count] = prev = o;
      color[count] = s.color;
      ++count;
    }
    for (uint32_t i = 0; i + 1 < count; ++i) {
      const float d = offset[i + 1] - offset[i];
      inv[i] = d > 1e-9f ? 1.0f / d : 0.0f;
    }
    return count != 0;
  }

  // Segment of t: 0 before the first stop, i + 1 between stops i and i + 1, count after the last. A hard stop
  // belongs to the color after it.
  uint32_t segment_of(float t) const {
    uint32_t s = 0;
    while (s < count && offset[s] <= t) ++s;
    return s;
  }

  // Premultiplied interpolation inside segment s at position t.
  uint32_t color_in_segment(uint32_t s, float t) const {
    if (s == 0) return color[0];
    if (s >= count) return color[count - 1];
    const uint32_t c0 = color[s - 1], c1 = color[s];
    const uint32_t ua = static_cast<uint32_t>(clampf((t - offset[s - 1]) * inv[s - 1], 0.0f, 1.0f) * 256.0f + 0.5f);
    const uint32_t wa = 256 - ua;
    const uint32_t a0 = c0 >> 24, a1 = c1 >> 24;
    const uint32_t den = a0 * wa + a1 * ua;  // alpha * 256
    const uint32_t alpha = (den + 128) >> 8;
    uint32_t out = alpha << 24;
    for (int shift = 16; shift >= 0; shift -= 8) {
      const uint32_t k0 = (c0 >> shift) & 0xFF, k1 = (c1 >> shift) & 0xFF;
      uint32_t k;
      if (a0 == 255 && a1 == 255) {
        k = (k0 * wa + k1 * ua + 128) >> 8;
      } else if (den == 0) {
        k = 0;
      } else {
        k = (k0 * a0 * wa + k1 * a1 * ua + den / 2) / den;
      }
      out |= (k > 255 ? 255 : k) << shift;
    }
    return out;
  }
  uint32_t color_at(float t) const { return color_in_segment(segment_of(t), t); }
};

// ---- Image sampling ---------------------------------------------------------------------------------------

// The texels a sampler may read: [x0, x1) x [y0, y1), inside the surface.
struct SampleBounds {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

inline SampleBounds sample_bounds(const Surface& s, const RectF& r) {
  SampleBounds b;
  if (!s.valid() || !(r.width > 0) || !(r.height > 0)) return b;
  b.x0 = max_i(floor_i(r.x), 0);
  b.y0 = max_i(floor_i(r.y), 0);
  b.x1 = min_i(ceil_i(r.x + r.width), saturate_i32(s.width));
  b.y1 = min_i(ceil_i(r.y + r.height), saturate_i32(s.height));
  return b;
}

inline uint32_t texel(const Surface& s, int32_t x, int32_t y) {
  return s.decode(s.pixels[uint64_t{static_cast<uint32_t>(y)} * s.pitch + static_cast<uint32_t>(x)]);
}
inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint32_t sample_nearest(const Surface& s, const SampleBounds& b, float sx, float sy) {
  return texel(s, clamp_i(floor_i(sx), b.x0, b.x1 - 1), clamp_i(floor_i(sy), b.y0, b.y1 - 1));
}

// Bilinear sample at source position (sx, sy) (texel centers are at +0.5). Alpha-weighted.
inline uint32_t sample_bilinear(const Surface& s, const SampleBounds& b, float sx, float sy) {
  const float fx = sx - 0.5f, fy = sy - 0.5f;
  const int32_t ix = floor_i(fx), iy = floor_i(fy);
  const uint32_t wx = static_cast<uint32_t>(clampf((fx - static_cast<float>(ix)) * 256.0f + 0.5f, 0.0f, 256.0f));
  const uint32_t wy = static_cast<uint32_t>(clampf((fy - static_cast<float>(iy)) * 256.0f + 0.5f, 0.0f, 256.0f));
  const int32_t xa = clamp_i(ix, b.x0, b.x1 - 1), xb = clamp_i(ix + 1, b.x0, b.x1 - 1);
  const int32_t ya = clamp_i(iy, b.y0, b.y1 - 1), yb = clamp_i(iy + 1, b.y0, b.y1 - 1);
  const uint32_t t[4] = {texel(s, xa, ya), texel(s, xb, ya), texel(s, xa, yb), texel(s, xb, yb)};
  if (wx == 0 && wy == 0) return t[0];
  const uint32_t w[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy};
  const bool opaque = (t[0] & t[1] & t[2] & t[3]) >> 24 == 255;
  uint32_t out = 0;
  if (opaque) {
    out = 0xFF000000u;
    for (int shift = 16; shift >= 0; shift -= 8) {
      uint32_t sum = 0;
      for (int i = 0; i < 4; ++i) sum += w[i] * ((t[i] >> shift) & 0xFF);
      out |= ((sum + 32768) >> 16) << shift;
    }
    return out;
  }
  uint32_t den = 0;
  for (int i = 0; i < 4; ++i) den += w[i] * (t[i] >> 24);
  if (den == 0) return 0;
  out = ((den + 32768) >> 16) << 24;
  for (int shift = 16; shift >= 0; shift -= 8) {
    uint64_t sum = 0;
    for (int i = 0; i < 4; ++i) sum += uint64_t{w[i]} * (t[i] >> 24) * ((t[i] >> shift) & 0xFF);
    uint64_t k = (sum + den / 2) / den;
    out |= static_cast<uint32_t>(k > 255 ? 255 : k) << shift;
  }
  return out;
}

}  // namespace graphics::raster
