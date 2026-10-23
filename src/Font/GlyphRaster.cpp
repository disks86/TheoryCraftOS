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
#include "Font/GlyphRaster.hpp"

namespace font {

namespace {

int32_t floor_to_int(float v) {
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<float>(i) > v ? i - 1 : i;
}
int32_t ceil_to_int(float v) {
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<float>(i) < v ? i + 1 : i;
}

struct Pointf {
  float x, y;
};

// Collects the signed area each edge contributes. `cells` is `stride` floats per row, `width` columns used plus
// two spare cells that absorb the edge cells at the right border.
class Accumulator {
 public:
  Accumulator(ArrayView<float> cells, uint32_t width, uint32_t height) : cells_(cells), width_(width), height_(height), stride_(width + 2) {}

  void line(Pointf p0, Pointf p1) {
    if (p0.y == p1.y) return;
    float dir = 1.0f;
    if (p0.y > p1.y) {
      const Pointf t = p0;
      p0 = p1;
      p1 = t;
      dir = -1.0f;
    }
    const float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    float x = p0.x;
    if (p0.y < 0.0f) x -= p0.y * dxdy;
    const int32_t first_row = p0.y > 0.0f ? floor_to_int(p0.y) : 0;
    int32_t last_row = ceil_to_int(p1.y);
    if (last_row > static_cast<int32_t>(height_)) last_row = static_cast<int32_t>(height_);
    for (int32_t y = first_row; y < last_row; ++y) {
      const uint64_t row = uint64_t{static_cast<uint32_t>(y)} * stride_;
      const float top = p0.y > static_cast<float>(y) ? p0.y : static_cast<float>(y);
      const float bottom = p1.y < static_cast<float>(y + 1) ? p1.y : static_cast<float>(y + 1);
      const float dy = bottom - top;
      const float xnext = x + dxdy * dy;
      const float d = dy * dir;
      float x0 = x < xnext ? x : xnext;
      float x1 = x < xnext ? xnext : x;
      const float limit = static_cast<float>(width_);
      if (x0 < 0.0f) x0 = 0.0f;
      if (x1 < 0.0f) x1 = 0.0f;
      if (x0 > limit) x0 = limit;
      if (x1 > limit) x1 = limit;
      const int32_t x0i = floor_to_int(x0);
      const float x0floor = static_cast<float>(x0i);
      const int32_t x1i = ceil_to_int(x1);
      if (x1i <= x0i + 1) {
        const float xmf = 0.5f * (x0 + x1) - x0floor;
        add(row, x0i, d - d * xmf);
        add(row, x0i + 1, d * xmf);
      } else {
        const float s = 1.0f / (x1 - x0);
        const float x0f = x0 - x0floor;
        const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
        const float x1f = x1 - static_cast<float>(x1i) + 1.0f;
        const float am = 0.5f * s * x1f * x1f;
        add(row, x0i, d * a0);
        if (x1i == x0i + 2) {
          add(row, x0i + 1, d * (1.0f - a0 - am));
        } else {
          const float a1 = s * (1.5f - x0f);
          add(row, x0i + 1, d * (a1 - a0));
          for (int32_t xi = x0i + 2; xi < x1i - 1; ++xi) add(row, xi, d * s);
          const float a2 = a1 + static_cast<float>(x1i - x0i - 3) * s;
          add(row, x1i - 1, d * (1.0f - a2 - am));
        }
        add(row, x1i, d * am);
      }
      x = xnext;
    }
  }

  // Adds a quadratic Bezier as short straight edges (error under a tenth of a pixel).
  void quad(Pointf p0, Pointf p1, Pointf p2) {
    const float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
    const float dev = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay);
    uint32_t n = 1;
    while (n < 24 && static_cast<float>(n * n) * 0.2f < dev) ++n;
    Pointf prev = p0;
    for (uint32_t i = 1; i <= n; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
      const Pointf p = {u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y};
      line(prev, p);
      prev = p;
    }
  }

 private:
  void add(uint64_t row, int32_t x, float v) {
    if (x < 0) x = 0;
    cells_[row + static_cast<uint32_t>(x)] += v;  // checked: the +2 spare cells keep x == width in range
  }
  ArrayView<float> cells_;
  uint32_t width_, height_, stride_;
};

}  // namespace

bool GlyphBitmap::allocate(uint32_t w, uint32_t h) {
  release_view(*allocator_, storage_);
  width = height = 0;
  if (w == 0 || h == 0) return true;
  if (w > kMaxGlyphDimension || h > kMaxGlyphDimension || uint64_t{w} * h > kMaxGlyphPixels) return false;
  storage_ = allocate_view<uint8_t>(*allocator_, uint64_t{w} * h);
  if (storage_.empty()) return false;
  storage_.fill(0);
  width = w;
  height = h;
  return true;
}

bool rasterize_outline(const Outline& outline, uint32_t units_per_em, Fixed size, uint32_t subpixel, allocators::IAllocator& allocator,
                       GlyphBitmap* out) {
  out->left = out->top = 0;
  if (!out->allocate(0, 0)) return false;
  const uint64_t count = outline.points.count();
  if (count < 3 || units_per_em == 0 || size <= 0) return true;  // nothing to draw

  const float scale = static_cast<float>(size) / (64.0f * static_cast<float>(units_per_em));
  const float shift = static_cast<float>(subpixel & 3) * 0.25f;
  // bounding box of all points (control points included: a curve stays inside them)
  float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
  bool first = true;
  for (const OutlinePoint& p : outline.points.view()) {
    const float x = static_cast<float>(p.x) * scale + shift, y = static_cast<float>(p.y) * scale;
    if (first || x < min_x) min_x = x;
    if (first || x > max_x) max_x = x;
    if (first || y < min_y) min_y = y;
    if (first || y > max_y) max_y = y;
    first = false;
  }
  if (max_x - min_x > static_cast<float>(kMaxGlyphDimension) || max_y - min_y > static_cast<float>(kMaxGlyphDimension)) return true;  // too large: blank
  const int32_t x0 = floor_to_int(min_x), x1 = ceil_to_int(max_x);
  const int32_t y_top = ceil_to_int(max_y), y_bottom = floor_to_int(min_y);  // y up
  const int32_t w = x1 - x0, h = y_top - y_bottom;
  if (w <= 0 || h <= 0 || w > static_cast<int32_t>(kMaxGlyphDimension) || h > static_cast<int32_t>(kMaxGlyphDimension) ||
      uint64_t{static_cast<uint32_t>(w)} * static_cast<uint32_t>(h) > kMaxGlyphPixels)
    return true;

  const uint32_t width = static_cast<uint32_t>(w), height = static_cast<uint32_t>(h);
  ArrayView<float> cells = allocate_view<float>(allocator, uint64_t{width + 2} * height);
  if (cells.empty()) return false;
  cells.fill(0.0f);
  Accumulator acc(cells, width, height);

  auto map = [&](const OutlinePoint& p) {
    return Pointf{static_cast<float>(p.x) * scale + shift - static_cast<float>(x0), static_cast<float>(y_top) - static_cast<float>(p.y) * scale};
  };
  uint64_t start = 0;
  for (uint32_t end_index : outline.contour_ends.view()) {
    const uint64_t end = end_index;
    if (end >= count || end < start) break;
    const uint64_t n = end - start + 1;
    if (n >= 3) {
      const ArrayView<const OutlinePoint> pts = outline.points.view().subview(start, n);
      Pointf origin;
      uint64_t from;  // first point to walk
      uint64_t steps; // how many
      if (pts[0].on_curve) {
        origin = map(pts[0]);
        from = 1;
        steps = n - 1;
      } else if (pts[n - 1].on_curve) {
        origin = map(pts[n - 1]);
        from = 0;
        steps = n - 1;
      } else {
        const Pointf a = map(pts[0]), b = map(pts[n - 1]);
        origin = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        from = 0;
        steps = n;
      }
      Pointf cur = origin, ctrl = {0, 0};
      bool has_ctrl = false;
      for (uint64_t i = 0; i < steps; ++i) {
        const OutlinePoint& op = pts[from + i];
        const Pointf p = map(op);
        if (op.on_curve) {
          if (has_ctrl) acc.quad(cur, ctrl, p);
          else acc.line(cur, p);
          has_ctrl = false;
          cur = p;
        } else if (has_ctrl) {
          const Pointf mid = {(ctrl.x + p.x) * 0.5f, (ctrl.y + p.y) * 0.5f};
          acc.quad(cur, ctrl, mid);
          cur = mid;
          ctrl = p;
        } else {
          ctrl = p;
          has_ctrl = true;
        }
      }
      if (has_ctrl) acc.quad(cur, ctrl, origin);
      else acc.line(cur, origin);
    }
    start = end + 1;
  }

  const bool ok = out->allocate(width, height);
  if (ok) {
    const ByteView pixels = out->writable_pixels();
    for (uint32_t y = 0; y < height; ++y) {
      float sum = 0.0f;
      const uint64_t row = uint64_t{y} * (width + 2);
      for (uint32_t x = 0; x < width; ++x) {
        sum += cells[row + x];
        float v = sum < 0 ? -sum : sum;
        if (v > 1.0f) v = 1.0f;
        pixels[uint64_t{y} * width + x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
      }
    }
    out->left = x0;
    out->top = y_top;
  }
  release_view(allocator, cells);
  return ok;
}

bool rasterize_glyph(const TrueTypeFont& font, uint32_t glyph, Fixed size, uint32_t subpixel, allocators::IAllocator& allocator, GlyphBitmap* out) {
  Outline outline(allocator);
  if (!font.load_outline(glyph, &outline)) return false;
  return rasterize_outline(outline, font.units_per_em(), size, subpixel, allocator, out);
}

}  // namespace font
