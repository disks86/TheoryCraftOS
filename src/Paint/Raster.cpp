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

#include "Paint/Raster.hpp"

namespace paint {

namespace {

constexpr double kPi = 3.14159265358979323846;

int32_t ifloor(double v) {
  if (!(v > -1e9)) return -1000000000;
  if (v > 1e9) return 1000000000;
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<double>(i) > v ? i - 1 : i;
}
int32_t iceil(double v) { return -ifloor(-v); }
float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
uint8_t to_byte(float v) { return static_cast<uint8_t>(v <= 0.0f ? 0 : v >= 255.0f ? 255 : static_cast<int32_t>(v + 0.5f)); }
float fsqrt(float v) { return v > 0.0f ? static_cast<float>(fmath::sqrt(static_cast<double>(v))) : 0.0f; }

float corner_depth(float dx, float dy, float rx, float ry) {
  const float u = dx / rx, v = dy / ry;
  return (1.0f - fsqrt(u * u + v * v)) * fsqrt(rx * ry);
}

}  // namespace

// ---- Pixmap, AlphaMask ------------------------------------------------------------------------------------------

bool Pixmap::create(int32_t width, int32_t height) {
  release_view(allocator_, pixels_);
  width_ = height_ = 0;
  if (width <= 0 || height <= 0 || uint64_t{static_cast<uint32_t>(width)} * static_cast<uint32_t>(height) > kMaxPixels) return false;
  pixels_ = allocate_view<uint32_t>(allocator_, uint64_t{static_cast<uint32_t>(width)} * static_cast<uint32_t>(height));
  if (pixels_.empty()) return false;
  pixels_.fill(0);
  width_ = width;
  height_ = height;
  return true;
}

graphics::Surface Pixmap::surface() const {
  graphics::Surface s;
  s.pixels = pixels_;
  s.width = static_cast<uint32_t>(width_);
  s.height = static_cast<uint32_t>(height_);
  s.pitch = static_cast<uint32_t>(width_);
  return s;
}

bool AlphaMask::create(int32_t width, int32_t height) {
  release_view(allocator_, data_);
  width_ = height_ = 0;
  if (width <= 0 || height <= 0 || uint64_t{static_cast<uint32_t>(width)} * static_cast<uint32_t>(height) > Pixmap::kMaxPixels) return false;
  data_ = allocate_view<uint8_t>(allocator_, uint64_t{static_cast<uint32_t>(width)} * static_cast<uint32_t>(height));
  if (data_.empty()) return false;
  data_.fill(0);
  width_ = width;
  height_ = height;
  return true;
}

graphics::MaskView AlphaMask::view() const {
  graphics::MaskView v;
  v.coverage = ConstByteView(data_);
  v.width = static_cast<uint32_t>(width_);
  v.height = static_cast<uint32_t>(height_);
  v.pitch = static_cast<uint32_t>(width_);
  return v;
}

uint32_t blend_over(uint32_t src, uint32_t dst) {
  const uint32_t sa = src >> 24;
  if (sa == 255) return src;
  if (sa == 0) return dst;
  const uint32_t da = dst >> 24;
  if (da == 0) return src;
  const uint32_t dst_weight = da * (255 - sa);   // (scaled by 255)
  const uint32_t out_a255 = sa * 255 + dst_weight;   // alpha x 255 x 255 / 255
  uint32_t out = 0;
  for (int shift = 0; shift < 24; shift += 8) {
    const uint32_t sc = (src >> shift) & 0xFF, dc = (dst >> shift) & 0xFF;
    const uint32_t c = (sc * sa * 255 + dc * dst_weight + out_a255 / 2) / out_a255;
    out |= (c > 255 ? 255 : c) << shift;
  }
  const uint32_t a = (out_a255 + 127) / 255;
  return out | ((a > 255 ? 255 : a) << 24);
}

IRect transformed_bounds(const Affine& t, const IRect& r) {
  const double xs[4] = {static_cast<double>(r.x), static_cast<double>(r.right()), static_cast<double>(r.x), static_cast<double>(r.right())};
  const double ys[4] = {static_cast<double>(r.y), static_cast<double>(r.y), static_cast<double>(r.bottom()), static_cast<double>(r.bottom())};
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  for (int i = 0; i < 4; ++i) {
    const double x = t.x_of(xs[i], ys[i]), y = t.y_of(xs[i], ys[i]);
    if (x < x0) x0 = x;
    if (x > x1) x1 = x;
    if (y < y0) y0 = y;
    if (y > y1) y1 = y;
  }
  auto sane = [](double v) { return v < -1e7 ? -1e7 : v > 1e7 ? 1e7 : (v == v ? v : 0.0); };
  IRect out;
  out.x = ifloor(sane(x0));
  out.y = ifloor(sane(y0));
  out.width = iceil(sane(x1)) - out.x;
  out.height = iceil(sane(y1)) - out.y;
  return out;
}

// ---- Rounded rectangles ------------------------------------------------------------------------------------------

RRect RRect::make(float x, float y, float width, float height, const graphics::CornerRadii& radii) {
  RRect r;
  r.x0 = x;
  r.y0 = y;
  r.x1 = x + width;
  r.y1 = y + height;
  auto pos = [](float v) { return v > 0.0f ? v : 0.0f; };
  r.tlx = pos(radii.tl_x); r.tly = pos(radii.tl_y);
  r.trx = pos(radii.tr_x); r.try_ = pos(radii.tr_y);
  r.brx = pos(radii.br_x); r.bry = pos(radii.br_y);
  r.blx = pos(radii.bl_x); r.bly = pos(radii.bl_y);
  float f = 1.0f;
  auto limit = [&](float sum, float side) {
    if (sum > side && sum > 0.0f) f = f < side / sum ? f : side / sum;
  };
  limit(r.tlx + r.trx, width);
  limit(r.blx + r.brx, width);
  limit(r.tly + r.bly, height);
  limit(r.try_ + r.bry, height);
  if (f < 1.0f) {
    if (f < 0.0f) f = 0.0f;
    r.tlx *= f; r.tly *= f; r.trx *= f; r.try_ *= f; r.brx *= f; r.bry *= f; r.blx *= f; r.bly *= f;
  }
  return r;
}

float RRect::coverage(int32_t px, int32_t py) const {
  const float fx = static_cast<float>(px), fy = static_cast<float>(py);
  const float ax = clamp01((fx + 1.0f < x1 ? fx + 1.0f : x1) - (fx > x0 ? fx : x0));
  const float ay = clamp01((fy + 1.0f < y1 ? fy + 1.0f : y1) - (fy > y0 ? fy : y0));
  float cov = ax * ay;
  if (cov <= 0.0f) return 0.0f;
  const float cx = fx + 0.5f, cy = fy + 0.5f;
  float corner = 2.0f;   // (no corner: larger than any coverage)
  if (tlx > 0 && tly > 0 && cx < x0 + tlx && cy < y0 + tly) corner = corner_depth(cx - (x0 + tlx), cy - (y0 + tly), tlx, tly);
  else if (trx > 0 && try_ > 0 && cx > x1 - trx && cy < y0 + try_) corner = corner_depth(cx - (x1 - trx), cy - (y0 + try_), trx, try_);
  else if (brx > 0 && bry > 0 && cx > x1 - brx && cy > y1 - bry) corner = corner_depth(cx - (x1 - brx), cy - (y1 - bry), brx, bry);
  else if (blx > 0 && bly > 0 && cx < x0 + blx && cy > y1 - bly) corner = corner_depth(cx - (x0 + blx), cy - (y1 - bly), blx, bly);
  if (corner < 1.5f) {
    const float c = clamp01(corner + 0.5f);
    cov = cov < c ? cov : c;
  }
  return cov;
}

float RRect::depth(float x, float y) const {
  float d = x - x0;
  if (x1 - x < d) d = x1 - x;
  if (y - y0 < d) d = y - y0;
  if (y1 - y < d) d = y1 - y;
  if (tlx > 0 && tly > 0 && x < x0 + tlx && y < y0 + tly) d = corner_depth(x - (x0 + tlx), y - (y0 + tly), tlx, tly);
  else if (trx > 0 && try_ > 0 && x > x1 - trx && y < y0 + try_) d = corner_depth(x - (x1 - trx), y - (y0 + try_), trx, try_);
  else if (brx > 0 && bry > 0 && x > x1 - brx && y > y1 - bry) d = corner_depth(x - (x1 - brx), y - (y1 - bry), brx, bry);
  else if (blx > 0 && bly > 0 && x < x0 + blx && y > y1 - bly) d = corner_depth(x - (x0 + blx), y - (y1 - bly), blx, bly);
  return d;
}

void fill_rrect(AlphaMask& mask, int32_t ox, int32_t oy, const RRect& shape) {
  if (shape.empty() || mask.width() <= 0) return;
  const int32_t i0 = ifloor(shape.x0) - ox > 0 ? ifloor(shape.x0) - ox : 0;
  const int32_t j0 = ifloor(shape.y0) - oy > 0 ? ifloor(shape.y0) - oy : 0;
  const int32_t i1 = iceil(shape.x1) - ox < mask.width() ? iceil(shape.x1) - ox : mask.width();
  const int32_t j1 = iceil(shape.y1) - oy < mask.height() ? iceil(shape.y1) - oy : mask.height();
  const ArrayView<uint8_t> data = mask.data();
  for (int32_t j = j0; j < j1; ++j) {
    for (int32_t i = i0; i < i1; ++i) {
      const uint8_t c = to_byte(shape.coverage(ox + i, oy + j) * 255.0f);
      uint8_t& dst = data[static_cast<uint64_t>(j) * static_cast<uint32_t>(mask.width()) + static_cast<uint32_t>(i)];
      if (c > dst) dst = c;
    }
  }
}

// ---- Polygons -----------------------------------------------------------------------------------------------------

void fill_polygon(AlphaMask& mask, int32_t ox, int32_t oy, ArrayView<const graphics::Vec2> pts, bool evenodd) {
  const uint64_t n = pts.size();
  if (n < 3 || n > kMaxClipPoints || mask.width() <= 0) return;
  float ymin = pts[0].y, ymax = pts[0].y;
  for (const graphics::Vec2& p : pts) {
    if (p.y < ymin) ymin = p.y;
    if (p.y > ymax) ymax = p.y;
  }
  const int32_t width = mask.width();
  ArrayView<float> acc = allocate_view<float>(mask.allocator(), static_cast<uint64_t>(width));
  if (acc.empty()) return;
  const int32_t j0 = ifloor(ymin) - oy > 0 ? ifloor(ymin) - oy : 0;
  const int32_t j1 = iceil(ymax) - oy < mask.height() ? iceil(ymax) - oy : mask.height();
  const ArrayView<uint8_t> data = mask.data();
  struct Crossing {
    float x;
    int dir;
  };
  Crossing xs[kMaxClipPoints];
  for (int32_t j = j0; j < j1; ++j) {
    acc.fill(0.0f);
    for (int k = 0; k < 4; ++k) {
      const float sy = static_cast<float>(oy + j) + (static_cast<float>(k) + 0.5f) * 0.25f;
      uint32_t count = 0;
      for (uint64_t e = 0; e < n; ++e) {
        const graphics::Vec2 p = pts[e], q = pts[e + 1 < n ? e + 1 : 0];
        if ((p.y <= sy) == (q.y <= sy) || count >= kMaxClipPoints) continue;
        xs[count].x = p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y);
        xs[count].dir = q.y > p.y ? 1 : -1;
        ++count;
      }
      for (uint32_t a = 1; a < count; ++a) {
        const Crossing t = xs[a];
        uint32_t b = a;
        for (; b > 0 && xs[b - 1].x > t.x; --b) xs[b] = xs[b - 1];
        xs[b] = t;
      }
      int winding = 0;
      float start = 0.0f;
      bool inside = false;
      for (uint32_t c = 0; c < count; ++c) {
        const bool was = inside;
        if (evenodd) {
          winding ^= 1;
          inside = winding != 0;
        } else {
          winding += xs[c].dir;
          inside = winding != 0;
        }
        if (!was && inside) {
          start = xs[c].x;
        } else if (was && !inside) {
          float xa = start - static_cast<float>(ox), xb = xs[c].x - static_cast<float>(ox);
          if (xa < 0.0f) xa = 0.0f;
          if (xb > static_cast<float>(width)) xb = static_cast<float>(width);
          if (xb <= xa) continue;
          const int32_t ia = static_cast<int32_t>(xa), ib = static_cast<int32_t>(xb);
          if (ia == ib) {
            acc[static_cast<uint64_t>(ia)] += (xb - xa) * 0.25f;
          } else {
            acc[static_cast<uint64_t>(ia)] += (static_cast<float>(ia + 1) - xa) * 0.25f;
            for (int32_t i = ia + 1; i < ib; ++i) acc[static_cast<uint64_t>(i)] += 0.25f;
            if (ib < width) acc[static_cast<uint64_t>(ib)] += (xb - static_cast<float>(ib)) * 0.25f;
          }
        }
      }
    }
    for (int32_t i = 0; i < width; ++i) {
      const uint8_t c = to_byte(acc[static_cast<uint64_t>(i)] * 255.0f);
      uint8_t& dst = data[static_cast<uint64_t>(j) * static_cast<uint32_t>(width) + static_cast<uint32_t>(i)];
      if (c > dst) dst = c;
    }
  }
  release_view(mask.allocator(), acc);
}

bool shape_contains(const ClipShape& shape, double x, double y) {
  if (!shape.polygon) return RRect::make(shape.x, shape.y, shape.width, shape.height, shape.radii).depth(static_cast<float>(x), static_cast<float>(y)) >= 0.0f;
  int winding = 0;
  bool odd = false;
  const uint32_t n = shape.point_count < kMaxClipPoints ? shape.point_count : kMaxClipPoints;
  for (uint32_t i = 0; i < n; ++i) {
    const graphics::Vec2 p = shape.points[i], q = shape.points[i + 1 < n ? i + 1 : 0];
    if ((p.y <= y) == (q.y <= y)) continue;
    const double cross = p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y);
    if (cross > x) {
      odd = !odd;
      winding += q.y > p.y ? 1 : -1;
    }
  }
  return shape.evenodd ? odd : winding != 0;
}

IRect shape_bounds(const ClipShape& shape) {
  IRect r;
  if (!shape.polygon) {
    r.x = ifloor(shape.x);
    r.y = ifloor(shape.y);
    r.width = iceil(shape.x + shape.width) - r.x;
    r.height = iceil(shape.y + shape.height) - r.y;
    return r;
  }
  if (shape.point_count == 0) return r;
  float x0 = shape.points[0].x, y0 = shape.points[0].y, x1 = x0, y1 = y0;
  for (uint32_t i = 1; i < shape.point_count && i < kMaxClipPoints; ++i) {
    const graphics::Vec2 p = shape.points[i];
    if (p.x < x0) x0 = p.x;
    if (p.x > x1) x1 = p.x;
    if (p.y < y0) y0 = p.y;
    if (p.y > y1) y1 = p.y;
  }
  r.x = ifloor(x0);
  r.y = ifloor(y0);
  r.width = iceil(x1) - r.x;
  r.height = iceil(y1) - r.y;
  return r;
}

// ---- Blur ------------------------------------------------------------------------------------------------------------

namespace {

// Three box sizes (odd) whose blurs in a row approximate a Gaussian of that standard deviation.
void box_sizes(float sigma, int32_t out[3]) {
  const double s2 = static_cast<double>(sigma) * static_cast<double>(sigma);
  const double ideal = fmath::sqrt(12.0 * s2 / 3.0 + 1.0);
  int32_t wl = ifloor(ideal);
  if (wl % 2 == 0) --wl;
  if (wl < 1) wl = 1;
  const int32_t wu = wl + 2;
  const double m_ideal = (12.0 * s2 - 3.0 * wl * wl - 12.0 * wl - 9.0) / (-4.0 * wl - 4.0);
  const int32_t m = ifloor(m_ideal + 0.5);
  for (int i = 0; i < 3; ++i) out[i] = i < m ? wl : wu;
}

// One box blur pass along the rows. The vertical direction (below) walks row by row with a running sum per column, so that
// memory is read in order instead of one column (a cache miss per pixel) at a time; the results are the same.
void box_pass_rows(ArrayView<const uint8_t> src, ArrayView<uint8_t> dst, int32_t w, int32_t h, int32_t r) {
  const uint32_t size = static_cast<uint32_t>(2 * r + 1);
  for (int32_t line = 0; line < h; ++line) {
    const uint64_t base = static_cast<uint64_t>(line) * static_cast<uint32_t>(w);
    int32_t sum = 0;
    for (int32_t k = 0; k <= r && k < w; ++k) sum += src[base + static_cast<uint64_t>(k)];
    for (int32_t i = 0; i < w; ++i) {
      dst[base + static_cast<uint64_t>(i)] = static_cast<uint8_t>((static_cast<uint32_t>(sum) + size / 2) / size);
      const int32_t add = i + r + 1, sub = i - r;
      if (add < w) sum += src[base + static_cast<uint64_t>(add)];
      if (sub >= 0) sum -= src[base + static_cast<uint64_t>(sub)];
    }
  }
}

bool box_pass_columns(ArrayView<const uint8_t> src, ArrayView<uint8_t> dst, int32_t w, int32_t h, int32_t r, ArrayView<int32_t> sums) {
  if (sums.size() < static_cast<uint32_t>(w)) return false;
  const uint32_t size = static_cast<uint32_t>(2 * r + 1);
  const uint64_t pitch = static_cast<uint32_t>(w);
  sums.fill(0);
  for (int32_t k = 0; k <= r && k < h; ++k)
    for (int32_t c = 0; c < w; ++c) sums[static_cast<uint64_t>(c)] += src[static_cast<uint64_t>(k) * pitch + static_cast<uint64_t>(c)];
  for (int32_t i = 0; i < h; ++i) {
    const uint64_t row = static_cast<uint64_t>(i) * pitch;
    for (int32_t c = 0; c < w; ++c) dst[row + static_cast<uint64_t>(c)] = static_cast<uint8_t>((static_cast<uint32_t>(sums[static_cast<uint64_t>(c)]) + size / 2) / size);
    const int32_t add = i + r + 1, sub = i - r;
    if (add < h) {
      const uint64_t a = static_cast<uint64_t>(add) * pitch;
      for (int32_t c = 0; c < w; ++c) sums[static_cast<uint64_t>(c)] += src[a + static_cast<uint64_t>(c)];
    }
    if (sub >= 0) {
      const uint64_t b = static_cast<uint64_t>(sub) * pitch;
      for (int32_t c = 0; c < w; ++c) sums[static_cast<uint64_t>(c)] -= src[b + static_cast<uint64_t>(c)];
    }
  }
  return true;
}

bool blur_plane(AlphaMask& plane, AlphaMask& scratch, const int32_t sizes[3]) {
  allocators::IAllocator& a = plane.allocator();
  ArrayView<int32_t> sums = allocate_view<int32_t>(a, static_cast<uint32_t>(plane.width()));
  if (sums.empty()) return false;
  bool ok = true;
  for (int i = 0; i < 3 && ok; ++i) {
    const int32_t r = (sizes[i] - 1) / 2;
    if (r <= 0) continue;
    box_pass_rows(ArrayView<const uint8_t>(plane.data()), scratch.data(), plane.width(), plane.height(), r);
    ok = box_pass_columns(ArrayView<const uint8_t>(scratch.data()), plane.data(), plane.width(), plane.height(), r, sums);
  }
  release_view(a, sums);
  return ok;
}

}  // namespace

int32_t blur_reach(float sigma) {
  if (!(sigma >= 0.3f)) return 0;
  if (sigma > 500.0f) sigma = 500.0f;
  int32_t sizes[3];
  box_sizes(sigma, sizes);
  return (sizes[0] - 1) / 2 + (sizes[1] - 1) / 2 + (sizes[2] - 1) / 2;
}

bool blur_mask(AlphaMask& mask, float sigma) {
  if (!(sigma >= 0.3f) || mask.width() <= 0) return true;
  if (sigma > 500.0f) sigma = 500.0f;
  int32_t sizes[3];
  box_sizes(sigma, sizes);
  AlphaMask scratch(mask.allocator());
  if (!scratch.create(mask.width(), mask.height())) return false;
  return blur_plane(mask, scratch, sizes);
}

bool blur_pixmap(Pixmap& pixmap, float sigma) {
  if (!(sigma >= 0.3f) || pixmap.width() <= 0) return true;
  if (sigma > 500.0f) sigma = 500.0f;
  int32_t sizes[3];
  box_sizes(sigma, sizes);
  allocators::IAllocator& a = pixmap.allocator();
  AlphaMask pa(a), pr(a), pg(a), pb(a), scratch(a);
  const int32_t w = pixmap.width(), h = pixmap.height();
  if (!pa.create(w, h) || !pr.create(w, h) || !pg.create(w, h) || !pb.create(w, h) || !scratch.create(w, h)) return false;
  const ArrayView<uint32_t> px = pixmap.pixels();
  for (uint64_t i = 0; i < px.size(); ++i) {
    const uint32_t p = px[i], alpha = p >> 24;
    pa.data()[i] = static_cast<uint8_t>(alpha);
    pr.data()[i] = static_cast<uint8_t>((((p >> 16) & 0xFF) * alpha + 127) / 255);
    pg.data()[i] = static_cast<uint8_t>((((p >> 8) & 0xFF) * alpha + 127) / 255);
    pb.data()[i] = static_cast<uint8_t>(((p & 0xFF) * alpha + 127) / 255);
  }
  blur_plane(pa, scratch, sizes);
  blur_plane(pr, scratch, sizes);
  blur_plane(pg, scratch, sizes);
  blur_plane(pb, scratch, sizes);
  for (uint64_t i = 0; i < px.size(); ++i) {
    const uint32_t alpha = pa.data()[i];
    if (alpha == 0) {
      px[i] = 0;
      continue;
    }
    auto un = [&](uint32_t c) {
      const uint32_t v = (c * 255 + alpha / 2) / alpha;
      return v > 255 ? 255u : v;
    };
    px[i] = (alpha << 24) | (un(pr.data()[i]) << 16) | (un(pg.data()[i]) << 8) | un(pb.data()[i]);
  }
  return true;
}

// ---- Colour filters ----------------------------------------------------------------------------------------------

void color_filter(Pixmap& pixmap, FilterStep::Kind kind, float amount) {
  // rgb' = m * rgb + add, the matrices of the filter effects specification
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float add[3] = {0, 0, 0};
  float alpha_scale = 1.0f;
  const float a = amount;
  switch (kind) {
    case FilterStep::Brightness:
      m[0] = m[4] = m[8] = a < 0 ? 0.0f : a;
      break;
    case FilterStep::Contrast: {
      const float c = a < 0 ? 0.0f : a;
      m[0] = m[4] = m[8] = c;
      add[0] = add[1] = add[2] = 0.5f - 0.5f * c;
      break;
    }
    case FilterStep::Grayscale: {
      const float g = 1.0f - clamp01(a);
      const float v[9] = {0.2126f + 0.7874f * g, 0.7152f - 0.7152f * g, 0.0722f - 0.0722f * g,
                          0.2126f - 0.2126f * g, 0.7152f + 0.2848f * g, 0.0722f - 0.0722f * g,
                          0.2126f - 0.2126f * g, 0.7152f - 0.7152f * g, 0.0722f + 0.9278f * g};
      for (int i = 0; i < 9; ++i) m[i] = v[i];
      break;
    }
    case FilterStep::Sepia: {
      const float g = 1.0f - clamp01(a);
      const float v[9] = {0.393f + 0.607f * g, 0.769f - 0.769f * g, 0.189f - 0.189f * g,
                          0.349f - 0.349f * g, 0.686f + 0.314f * g, 0.168f - 0.168f * g,
                          0.272f - 0.272f * g, 0.534f - 0.534f * g, 0.131f + 0.869f * g};
      for (int i = 0; i < 9; ++i) m[i] = v[i];
      break;
    }
    case FilterStep::Saturate: {
      const float s = a < 0 ? 0.0f : a;
      const float v[9] = {0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s,
                          0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s,
                          0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s};
      for (int i = 0; i < 9; ++i) m[i] = v[i];
      break;
    }
    case FilterStep::HueRotate: {
      const double rad = static_cast<double>(a) * kPi / 180.0;
      const float c = static_cast<float>(fmath::cos(rad)), s = static_cast<float>(fmath::sin(rad));
      const float v[9] = {0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
                          0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
                          0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f};
      for (int i = 0; i < 9; ++i) m[i] = v[i];
      break;
    }
    case FilterStep::Invert: {
      const float k = clamp01(a);
      m[0] = m[4] = m[8] = 1.0f - 2.0f * k;
      add[0] = add[1] = add[2] = k;
      break;
    }
    case FilterStep::Opacity: alpha_scale = clamp01(a); break;
    default: return;
  }
  const ArrayView<uint32_t> px = pixmap.pixels();
  for (uint64_t i = 0; i < px.size(); ++i) {
    const uint32_t p = px[i], alpha = p >> 24;
    if (alpha == 0) continue;
    const float r = static_cast<float>((p >> 16) & 0xFF) / 255.0f, g = static_cast<float>((p >> 8) & 0xFF) / 255.0f, b = static_cast<float>(p & 0xFF) / 255.0f;
    const float nr = m[0] * r + m[1] * g + m[2] * b + add[0];
    const float ng = m[3] * r + m[4] * g + m[5] * b + add[1];
    const float nb = m[6] * r + m[7] * g + m[8] * b + add[2];
    const uint32_t na = alpha_scale < 1.0f ? to_byte(static_cast<float>(alpha) * alpha_scale) : alpha;
    px[i] = (static_cast<uint32_t>(na) << 24) | (static_cast<uint32_t>(to_byte(nr * 255.0f)) << 16) | (static_cast<uint32_t>(to_byte(ng * 255.0f)) << 8) | to_byte(nb * 255.0f);
  }
}

bool drop_shadow(Pixmap& pixmap, int32_t dx, int32_t dy, float sigma, uint32_t color) {
  const int32_t w = pixmap.width(), h = pixmap.height();
  if (w <= 0) return true;
  AlphaMask mask(pixmap.allocator());
  if (!mask.create(w, h)) return false;
  const ArrayView<uint32_t> px = pixmap.pixels();
  for (int32_t y = 0; y < h; ++y) {
    const int32_t sy = y - dy;
    if (sy < 0 || sy >= h) continue;
    for (int32_t x = 0; x < w; ++x) {
      const int32_t sx = x - dx;
      if (sx < 0 || sx >= w) continue;
      mask.data()[static_cast<uint64_t>(y) * static_cast<uint32_t>(w) + static_cast<uint32_t>(x)] =
          static_cast<uint8_t>(px[static_cast<uint64_t>(sy) * static_cast<uint32_t>(w) + static_cast<uint32_t>(sx)] >> 24);
    }
  }
  if (!blur_mask(mask, sigma)) return false;
  const uint32_t ca = color >> 24, rgb = color & 0x00FFFFFFu;
  for (uint64_t i = 0; i < px.size(); ++i) {
    const uint32_t sa = (mask.data()[i] * ca + 127) / 255;
    if (sa == 0) continue;
    px[i] = blend_over(px[i], rgb | (sa << 24));
  }
  return true;
}

// ---- Resampling ---------------------------------------------------------------------------------------------------

namespace {

struct Accum {
  float a = 0, r = 0, g = 0, b = 0;   // premultiplied, 0..255
  void add(uint32_t p, float w) {
    const float alpha = static_cast<float>(p >> 24) * w;
    a += alpha;
    r += alpha * static_cast<float>((p >> 16) & 0xFF) / 255.0f;
    g += alpha * static_cast<float>((p >> 8) & 0xFF) / 255.0f;
    b += alpha * static_cast<float>(p & 0xFF) / 255.0f;
  }
  uint32_t result(float total) const {
    const float alpha = a / total;
    if (alpha < 0.5f) return 0;
    const float inv = 255.0f / a;   // un-premultiply: colour = sum(alpha * c) / sum(alpha)
    const uint32_t ba = to_byte(alpha);
    return (static_cast<uint32_t>(ba) << 24) | (static_cast<uint32_t>(to_byte(r * inv)) << 16) | (static_cast<uint32_t>(to_byte(g * inv)) << 8) | to_byte(b * inv);
  }
};

uint32_t texel(const Pixmap& src, int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= src.width() || y >= src.height()) return 0;
  return src.pixels()[static_cast<uint64_t>(y) * static_cast<uint32_t>(src.width()) + static_cast<uint32_t>(x)];
}

}  // namespace

bool resample(const Pixmap& src, double src_x, double src_y, const Affine& t, const IRect& dest, Pixmap* out) {
  Affine inv;
  if (!t.invert(&inv) || !out->create(dest.width, dest.height)) return false;
  const ArrayView<uint32_t> dst = out->pixels();
  const uint32_t dw = static_cast<uint32_t>(dest.width);
  if (t.axis_aligned()) {
    // a box filter: every output pixel averages the source texels its footprint covers (weights = overlap)
    for (int32_t j = 0; j < dest.height; ++j) {
      double v0 = inv.d * (dest.y + j) + inv.f - src_y, v1 = inv.d * (dest.y + j + 1) + inv.f - src_y;
      if (v0 > v1) { const double s = v0; v0 = v1; v1 = s; }
      const int32_t ty0 = ifloor(v0 + 1e-6), ty1 = iceil(v1 - 1e-6) - 1;
      const int32_t ystep = (ty1 - ty0 + 1) > 16 ? (ty1 - ty0 + 1) / 16 : 1;
      for (int32_t i = 0; i < dest.width; ++i) {
        double u0 = inv.a * (dest.x + i) + inv.e - src_x, u1 = inv.a * (dest.x + i + 1) + inv.e - src_x;
        if (u0 > u1) { const double s = u0; u0 = u1; u1 = s; }
        const int32_t tx0 = ifloor(u0 + 1e-6), tx1 = iceil(u1 - 1e-6) - 1;
        const int32_t xstep = (tx1 - tx0 + 1) > 16 ? (tx1 - tx0 + 1) / 16 : 1;
        if (tx1 < 0 || ty1 < 0 || tx0 >= src.width() || ty0 >= src.height()) continue;
        Accum acc;
        float total = 0.0f;
        for (int32_t ty = ty0; ty <= ty1; ty += ystep) {
          const double wy0 = v0 > ty ? v0 : ty, wy1 = v1 < ty + 1 ? v1 : ty + 1;
          if (wy1 <= wy0) continue;
          for (int32_t tx = tx0; tx <= tx1; tx += xstep) {
            const double wx0 = u0 > tx ? u0 : tx, wx1 = u1 < tx + 1 ? u1 : tx + 1;
            if (wx1 <= wx0) continue;
            const float w = static_cast<float>((wx1 - wx0) * (wy1 - wy0));
            total += w;
            acc.add(texel(src, tx, ty), w);
          }
        }
        if (total > 0.0f) dst[static_cast<uint64_t>(j) * dw + static_cast<uint32_t>(i)] = acc.result(total);
      }
    }
    return true;
  }
  // rotated or skewed: bilinear, with n x n samples per pixel when the picture shrinks
  const double scale = fmath::sqrt(t.determinant() < 0 ? -t.determinant() : t.determinant());
  int32_t n = 1;
  if (scale < 0.75) n = scale <= 0.25 ? 4 : scale <= 0.4 ? 3 : 2;
  const float weight = 1.0f / static_cast<float>(n * n);
  for (int32_t j = 0; j < dest.height; ++j) {
    for (int32_t i = 0; i < dest.width; ++i) {
      Accum acc;
      for (int32_t sj = 0; sj < n; ++sj) {
        for (int32_t si = 0; si < n; ++si) {
          const double px = dest.x + i + (si + 0.5) / n, py = dest.y + j + (sj + 0.5) / n;
          const double u = inv.x_of(px, py) - src_x - 0.5, v = inv.y_of(px, py) - src_y - 0.5;
          const int32_t x0 = ifloor(u), y0 = ifloor(v);
          const float fx = static_cast<float>(u - x0), fy = static_cast<float>(v - y0);
          acc.add(texel(src, x0, y0), (1 - fx) * (1 - fy) * weight);
          acc.add(texel(src, x0 + 1, y0), fx * (1 - fy) * weight);
          acc.add(texel(src, x0, y0 + 1), (1 - fx) * fy * weight);
          acc.add(texel(src, x0 + 1, y0 + 1), fx * fy * weight);
        }
      }
      if (acc.a > 0.0f) dst[static_cast<uint64_t>(j) * dw + static_cast<uint32_t>(i)] = acc.result(1.0f);
    }
  }
  return true;
}

}  // namespace paint
