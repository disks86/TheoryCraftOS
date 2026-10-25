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

// Clipping, setup and scan conversion (see Raster.hpp). Triangle edges are evaluated in 24.8 fixed point so that triangles sharing an edge never leave a gap
// or draw a pixel twice (the top-left rule decides the pixels exactly on an edge).
#include "WebGl/Raster.hpp"
#include "WebGl/Shader.hpp"

namespace gpu {

struct TriSetup {
  int64_t X[3], Y[3];
  double area;
  float z[3], iw[3];
  const float* var[3];
  const float* flatvals;
  const RasterState* rs;
  // returns the interpolated varyings at pixel centre (px + 0.5, py + 0.5)
  void eval(int32_t px, int32_t py, float* out) const {
    const int64_t PX = int64_t{px} * 256 + 128, PY = int64_t{py} * 256 + 128;
    const double e0 = static_cast<double>((X[2] - X[1]) * (PY - Y[1]) - (Y[2] - Y[1]) * (PX - X[1]));
    const double e1 = static_cast<double>((X[0] - X[2]) * (PY - Y[2]) - (Y[0] - Y[2]) * (PX - X[2]));
    const double e2 = static_cast<double>((X[1] - X[0]) * (PY - Y[0]) - (Y[1] - Y[0]) * (PX - X[0]));
    const double b0 = e0 / area, b1 = e1 / area, b2 = e2 / area;
    const double d = b0 * iw[0] + b1 * iw[1] + b2 * iw[2];
    const double inv = d != 0 ? 1.0 / d : 0.0;
    const float p0 = static_cast<float>(b0 * iw[0] * inv), p1 = static_cast<float>(b1 * iw[1] * inv), p2 = static_cast<float>(b2 * iw[2] * inv);
    for (uint32_t i = 0; i < rs->nvar; ++i) out[i] = rs->flat[i] ? flatvals[i] : p0 * var[0][i] + p1 * var[1][i] + p2 * var[2][i];
  }
};

void Fragment::varyings_at(int32_t px, int32_t py, float* out, uint32_t n) const {
  if (setup && !is_point) {
    float tmp[kMaxVarying];
    setup->eval(px, py, tmp);
    for (uint32_t i = 0; i < n && i < kMaxVarying; ++i) out[i] = tmp[i];
    return;
  }
  for (uint32_t i = 0; i < n && i < kMaxVarying; ++i) out[i] = varyings ? varyings[i] : 0;
}

namespace {

constexpr float kGuard = 8.0f;
constexpr uint32_t kMaxPoly = 14;

struct Poly {
  RVertex v[kMaxPoly];
  uint32_t n = 0;
};

float plane_dist(const RasterState& rs, const RVertex& v, int plane) {
  const float x = v.pos[0], y = v.pos[1], z = v.pos[2], w = v.pos[3];
  switch (plane) {
    case 0: return w - 1e-6f;                                         // in front of the eye
    case 1: return rs.clip_z_zero_to_one ? z : z + w;                // near
    case 2: return w - z;                                             // far
    case 3: return kGuard * w + x;
    case 4: return kGuard * w - x;
    case 5: return kGuard * w + y;
    default: return kGuard * w - y;
  }
}

void lerp_vertex(const RVertex& a, const RVertex& b, float t, uint32_t nvar, RVertex* out) {
  for (int i = 0; i < 4; ++i) out->pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
  for (uint32_t i = 0; i < nvar; ++i) out->var[i] = a.var[i] + (b.var[i] - a.var[i]) * t;
  out->point_size = a.point_size;
}

// Sutherland-Hodgman against every plane.
bool clip_poly(const RasterState& rs, Poly* poly) {
  Poly tmp;
  for (int plane = 0; plane < 7; ++plane) {
    if (!rs.depth_clip && (plane == 1 || plane == 2)) continue;
    tmp.n = 0;
    for (uint32_t i = 0; i < poly->n; ++i) {
      const RVertex& a = poly->v[i];
      const RVertex& b = poly->v[(i + 1) % poly->n];
      const float da = plane_dist(rs, a, plane), db = plane_dist(rs, b, plane);
      if (da >= 0 && tmp.n < kMaxPoly) tmp.v[tmp.n++] = a;
      if ((da >= 0) != (db >= 0) && tmp.n < kMaxPoly) {
        lerp_vertex(a, b, da / (da - db), rs.nvar, &tmp.v[tmp.n]);
        ++tmp.n;
      }
    }
    *poly = tmp;
    if (poly->n < 3) return false;
  }
  return true;
}

struct Win {
  float x, y, z, iw;
};

Win to_window(const RasterState& rs, const RVertex& v) {
  const float iw = 1.0f / v.pos[3];
  Win w;
  w.x = rs.vp_x + (v.pos[0] * iw * 0.5f + 0.5f) * rs.vp_w;
  w.y = rs.vp_y + (v.pos[1] * iw * 0.5f + 0.5f) * rs.vp_h;
  const float zn = v.pos[2] * iw;
  w.z = rs.depth_near + (rs.clip_z_zero_to_one ? zn : (zn * 0.5f + 0.5f)) * (rs.depth_far - rs.depth_near);
  w.iw = iw;
  return w;
}

int64_t fix(float v) {
  const double s = static_cast<double>(v) * 256.0;
  return static_cast<int64_t>(s < 0 ? s - 0.5 : s + 0.5);
}

int32_t floor_div256(int64_t v) { return static_cast<int32_t>(v >= 0 ? v / 256 : -((-v + 255) / 256)); }
int32_t ceil_div256(int64_t v) { return static_cast<int32_t>(v >= 0 ? (v + 255) / 256 : -((-v) / 256)); }

bool top_left(int64_t ax, int64_t ay, int64_t bx, int64_t by) {
  const int64_t dx = bx - ax, dy = by - ay;
  return (dy == 0 && dx < 0) || dy < 0;
}

void raster_triangle(const RasterState& rs, const RVertex& a, const RVertex& b, const RVertex& c, const float* flatvals, bool original_swapped, IFragmentSink& sink) {
  Win w[3] = {to_window(rs, a), to_window(rs, b), to_window(rs, c)};
  const RVertex* vs[3] = {&a, &b, &c};
  TriSetup s;
  s.rs = &rs;
  s.flatvals = flatvals;
  for (int i = 0; i < 3; ++i) {
    s.X[i] = fix(w[i].x);
    s.Y[i] = fix(w[i].y);
    s.z[i] = w[i].z;
    s.iw[i] = w[i].iw;
    s.var[i] = vs[i]->var;
  }
  int64_t area2 = (s.X[1] - s.X[0]) * (s.Y[2] - s.Y[0]) - (s.X[2] - s.X[0]) * (s.Y[1] - s.Y[0]);
  if (area2 == 0) return;
  bool ccw = area2 > 0;
  if (original_swapped) ccw = !ccw;
  const bool front = ccw == rs.front_ccw;
  if ((rs.cull == 3) || (rs.cull == 1 && front) || (rs.cull == 2 && !front)) return;
  if (area2 < 0) {   // work with a positive area: swap vertices 1 and 2
    int64_t t = s.X[1]; s.X[1] = s.X[2]; s.X[2] = t;
    t = s.Y[1]; s.Y[1] = s.Y[2]; s.Y[2] = t;
    const float tz = s.z[1]; s.z[1] = s.z[2]; s.z[2] = tz;
    const float ti = s.iw[1]; s.iw[1] = s.iw[2]; s.iw[2] = ti;
    const float* tv = s.var[1]; s.var[1] = s.var[2]; s.var[2] = tv;
    area2 = -area2;
  }
  s.area = static_cast<double>(area2);
  // bounding box in pixels
  int64_t minx = s.X[0], maxx = s.X[0], miny = s.Y[0], maxy = s.Y[0];
  for (int i = 1; i < 3; ++i) {
    if (s.X[i] < minx) minx = s.X[i];
    if (s.X[i] > maxx) maxx = s.X[i];
    if (s.Y[i] < miny) miny = s.Y[i];
    if (s.Y[i] > maxy) maxy = s.Y[i];
  }
  int32_t x0 = floor_div256(minx), x1 = ceil_div256(maxx), y0 = floor_div256(miny), y1 = ceil_div256(maxy);
  int32_t lx = rs.vp_x > rs.sc_x ? rs.vp_x : rs.sc_x, ly = rs.vp_y > rs.sc_y ? rs.vp_y : rs.sc_y;
  int32_t hx = rs.vp_x + rs.vp_w, hy = rs.vp_y + rs.vp_h;
  if (rs.sc_x + rs.sc_w < hx) hx = rs.sc_x + rs.sc_w;
  if (rs.sc_y + rs.sc_h < hy) hy = rs.sc_y + rs.sc_h;
  if (lx < 0) lx = 0;
  if (ly < 0) ly = 0;
  if (hx > rs.target_w) hx = rs.target_w;
  if (hy > rs.target_h) hy = rs.target_h;
  if (x0 < lx) x0 = lx;
  if (y0 < ly) y0 = ly;
  if (x1 > hx) x1 = hx;
  if (y1 > hy) y1 = hy;
  if (x0 >= x1 || y0 >= y1) return;
  // edge setup: e_i(P) = (Bx - Ax) * (Py - Ay) - (By - Ay) * (Px - Ax) for the edge opposite vertex i
  const int64_t ax[3] = {s.X[1], s.X[2], s.X[0]}, ay[3] = {s.Y[1], s.Y[2], s.Y[0]};
  const int64_t bx[3] = {s.X[2], s.X[0], s.X[1]}, by[3] = {s.Y[2], s.Y[0], s.Y[1]};
  const bool tl[3] = {top_left(ax[0], ay[0], bx[0], by[0]), top_left(ax[1], ay[1], bx[1], by[1]), top_left(ax[2], ay[2], bx[2], by[2])};
  float offset = 0;
  if (rs.offset) {
    const double inv = 256.0 / s.area;
    double dzdx = 0, dzdy = 0;
    for (int i = 0; i < 3; ++i) {
      dzdx += s.z[i] * -static_cast<double>(by[i] - ay[i]) * inv;
      dzdy += s.z[i] * static_cast<double>(bx[i] - ax[i]) * inv;
    }
    if (dzdx < 0) dzdx = -dzdx;
    if (dzdy < 0) dzdy = -dzdy;
    offset = static_cast<float>(rs.offset_factor * (dzdx > dzdy ? dzdx : dzdy) + rs.offset_units * (1.0 / 16777216.0));
  }
  float var[kMaxVarying];
  Fragment f;
  f.front = front;
  f.varyings = var;
  f.setup = &s;
  for (int32_t py = y0; py < y1; ++py) {
    const int64_t PY = int64_t{py} * 256 + 128;
    for (int32_t px = x0; px < x1; ++px) {
      const int64_t PX = int64_t{px} * 256 + 128;
      bool inside = true;
      int64_t e[3];
      for (int i = 0; i < 3 && inside; ++i) {
        e[i] = (bx[i] - ax[i]) * (PY - ay[i]) - (by[i] - ay[i]) * (PX - ax[i]);
        if (e[i] < 0 || (e[i] == 0 && !tl[i])) inside = false;
      }
      if (!inside) continue;
      const double b0 = static_cast<double>(e[0]) / s.area, b1 = static_cast<double>(e[1]) / s.area, b2 = static_cast<double>(e[2]) / s.area;
      float z = static_cast<float>(b0 * s.z[0] + b1 * s.z[1] + b2 * s.z[2]) + offset;
      if (z < 0) z = 0;
      if (z > 1) z = 1;
      const double d = b0 * s.iw[0] + b1 * s.iw[1] + b2 * s.iw[2];
      const double inv = d != 0 ? 1.0 / d : 0.0;
      const float p0 = static_cast<float>(b0 * s.iw[0] * inv), p1 = static_cast<float>(b1 * s.iw[1] * inv), p2 = static_cast<float>(b2 * s.iw[2] * inv);
      for (uint32_t i = 0; i < rs.nvar; ++i) var[i] = rs.flat[i] ? flatvals[i] : p0 * s.var[0][i] + p1 * s.var[1][i] + p2 * s.var[2][i];
      f.x = px;
      f.y = py;
      f.z = z;
      f.coord[0] = px + 0.5f;
      f.coord[1] = py + 0.5f;
      f.coord[2] = z;
      f.coord[3] = static_cast<float>(d);
      sink.fragment(f);
    }
  }
}

}  // namespace

void draw_triangle(const RasterState& rs, const RVertex& a, const RVertex& b, const RVertex& c, IFragmentSink& sink) {
  float flatvals[kMaxVarying];
  const RVertex& prov = rs.flat_first ? a : c;
  for (uint32_t i = 0; i < rs.nvar; ++i) flatvals[i] = prov.var[i];
  // trivial reject / accept
  bool all_in = true;
  for (int plane = 0; plane < 7 && all_in; ++plane) {
    if (!rs.depth_clip && (plane == 1 || plane == 2)) continue;
    const float da = plane_dist(rs, a, plane), db = plane_dist(rs, b, plane), dc = plane_dist(rs, c, plane);
    if (da < 0 && db < 0 && dc < 0) return;
    if (da < 0 || db < 0 || dc < 0) all_in = false;
  }
  if (all_in) {
    raster_triangle(rs, a, b, c, flatvals, false, sink);
    return;
  }
  Poly poly;
  poly.v[0] = a;
  poly.v[1] = b;
  poly.v[2] = c;
  poly.n = 3;
  if (!clip_poly(rs, &poly)) return;
  for (uint32_t i = 1; i + 1 < poly.n; ++i) raster_triangle(rs, poly.v[0], poly.v[i], poly.v[i + 1], flatvals, false, sink);
}

void draw_line(const RasterState& rs, const RVertex& a, const RVertex& b, IFragmentSink& sink) {
  RVertex p = a, q = b;
  float flatvals[kMaxVarying];
  for (uint32_t i = 0; i < rs.nvar; ++i) flatvals[i] = rs.flat_first ? a.var[i] : b.var[i];
  // clip the segment against the planes
  for (int plane = 0; plane < 7; ++plane) {
    if (!rs.depth_clip && (plane == 1 || plane == 2)) continue;
    const float dp = plane_dist(rs, p, plane), dq = plane_dist(rs, q, plane);
    if (dp < 0 && dq < 0) return;
    if (dp < 0) {
      RVertex n;
      lerp_vertex(p, q, dp / (dp - dq), rs.nvar, &n);
      p = n;
    } else if (dq < 0) {
      RVertex n;
      lerp_vertex(q, p, dq / (dq - dp), rs.nvar, &n);
      q = n;
    }
  }
  const Win wp = to_window(rs, p), wq = to_window(rs, q);
  float dx = wq.x - wp.x, dy = wq.y - wp.y;
  const bool xmajor = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
  const float len = xmajor ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  int32_t lx = rs.vp_x > rs.sc_x ? rs.vp_x : rs.sc_x, ly = rs.vp_y > rs.sc_y ? rs.vp_y : rs.sc_y;
  int32_t hx = rs.vp_x + rs.vp_w, hy = rs.vp_y + rs.vp_h;
  if (rs.sc_x + rs.sc_w < hx) hx = rs.sc_x + rs.sc_w;
  if (rs.sc_y + rs.sc_h < hy) hy = rs.sc_y + rs.sc_h;
  if (lx < 0) lx = 0;
  if (ly < 0) ly = 0;
  if (hx > rs.target_w) hx = rs.target_w;
  if (hy > rs.target_h) hy = rs.target_h;
  float var[kMaxVarying];
  Fragment f;
  f.varyings = var;
  f.is_point = false;
  const int32_t thick = rs.line_width > 1 ? static_cast<int32_t>(rs.line_width + 0.5f) : 1;
  const int32_t start = static_cast<int32_t>(xmajor ? (wp.x < wq.x ? wp.x : wq.x) : (wp.y < wq.y ? wp.y : wq.y)) - 0;
  const int32_t end = static_cast<int32_t>(xmajor ? (wp.x < wq.x ? wq.x : wp.x) : (wp.y < wq.y ? wq.y : wp.y));
  int32_t s0 = start < 0 && end >= 0 ? (start < -1000000 ? -1000000 : start) : start;
  if (end - s0 > 100000) s0 = end - 100000;
  for (int32_t m = s0; m <= end; ++m) {
    // parameter along the line of the pixel centre on the major axis
    float t;
    if (len < 1e-6f) t = 0;
    else {
      const float centre = static_cast<float>(m) + 0.5f;
      t = xmajor ? (centre - wp.x) / dx : (centre - wp.y) / dy;
    }
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const float minor = xmajor ? wp.y + dy * t : wp.x + dx * t;
    const int32_t mn = static_cast<int32_t>(minor < 0 ? minor - 1 : minor);
    const float iw = wp.iw + (wq.iw - wp.iw) * t;
    const float lam = iw != 0 ? (wq.iw * t) / iw : 0;
    for (uint32_t i = 0; i < rs.nvar; ++i) var[i] = rs.flat[i] ? flatvals[i] : p.var[i] + (q.var[i] - p.var[i]) * lam;
    for (int32_t k = 0; k < thick; ++k) {
      const int32_t mm = mn + k - (thick - 1) / 2;
      const int32_t px = xmajor ? m : mm, py = xmajor ? mm : m;
      if (px < lx || px >= hx || py < ly || py >= hy) continue;
      float z = wp.z + (wq.z - wp.z) * t;
      if (z < 0) z = 0;
      if (z > 1) z = 1;
      f.x = px;
      f.y = py;
      f.z = z;
      f.coord[0] = px + 0.5f;
      f.coord[1] = py + 0.5f;
      f.coord[2] = z;
      f.coord[3] = iw;
      sink.fragment(f);
    }
  }
}

void draw_point(const RasterState& rs, const RVertex& a, IFragmentSink& sink) {
  for (int plane = 0; plane < 3; ++plane) {
    if (!rs.depth_clip && (plane == 1 || plane == 2)) continue;
    if (plane_dist(rs, a, plane) < 0) return;
  }
  const Win w = to_window(rs, a);
  float size = a.point_size;
  if (size < 1) size = 1;
  if (size > rs.max_point_size) size = rs.max_point_size;
  const float fx0 = w.x - size * 0.5f, fy0 = w.y - size * 0.5f;
  auto first = [](float v) { const float c = v - 0.5f; const int32_t i = static_cast<int32_t>(c); return (static_cast<float>(i) < c) ? i + 1 : i; };
  const int32_t px0 = first(fx0), px1 = first(fx0 + size), py0 = first(fy0), py1 = first(fy0 + size);
  int32_t lx = rs.vp_x > rs.sc_x ? rs.vp_x : rs.sc_x, ly = rs.vp_y > rs.sc_y ? rs.vp_y : rs.sc_y;
  int32_t hx = rs.vp_x + rs.vp_w, hy = rs.vp_y + rs.vp_h;
  if (rs.sc_x + rs.sc_w < hx) hx = rs.sc_x + rs.sc_w;
  if (rs.sc_y + rs.sc_h < hy) hy = rs.sc_y + rs.sc_h;
  if (lx < 0) lx = 0;
  if (ly < 0) ly = 0;
  if (hx > rs.target_w) hx = rs.target_w;
  if (hy > rs.target_h) hy = rs.target_h;
  Fragment f;
  f.varyings = a.var;
  f.is_point = true;
  f.z = w.z < 0 ? 0 : (w.z > 1 ? 1 : w.z);
  for (int32_t py = py0; py < py1; ++py)
    for (int32_t px = px0; px < px1; ++px) {
      if (px < lx || px >= hx || py < ly || py >= hy) continue;
      f.x = px;
      f.y = py;
      f.coord[0] = px + 0.5f;
      f.coord[1] = py + 0.5f;
      f.coord[2] = f.z;
      f.coord[3] = w.iw;
      f.point_coord[0] = (px + 0.5f - fx0) / size;
      f.point_coord[1] = 1.0f - (py + 0.5f - fy0) / size;
      sink.fragment(f);
    }
}

}  // namespace gpu
