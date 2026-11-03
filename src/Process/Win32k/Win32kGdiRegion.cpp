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
#include "Process/Win32k/Win32kGdiInternal.hpp"

// Regions: GDI regions are kept as banded lists of rectangles (rows of equal height, in each row sorted non-overlapping spans, equal neighbouring rows merged), the same shape
// Windows' own region code produces. Every operation is a scanline sweep over the bands of its operands.
namespace process::win32k::gdi {

namespace {

struct Span { int32_t a = 0, b = 0; };

bool spans_in(ArrayView<const Rect> r, int32_t y0, int32_t y1, List<Span>& out) {
  out.clear();
  for (const Rect& e : r) {
    if (e.right <= e.left || e.bottom <= e.top || e.top >= y1 || e.bottom <= y0) continue;
    uint64_t i = out.count();
    if (!out.push_back(Span{e.left, e.right})) return false;
    while (i > 0 && out[i - 1].a > out[i].a) {
      const Span t = out[i - 1];
      out[i - 1] = out[i];
      out[i] = t;
      --i;
    }
  }
  // merge touching and overlapping spans in place
  uint64_t w = 0;
  for (uint64_t i = 0; i < out.count(); ++i) {
    if (w > 0 && out[i].a <= out[w - 1].b) {
      if (out[i].b > out[w - 1].b) out[w - 1].b = out[i].b;
    } else {
      out[w++] = out[i];
    }
  }
  while (out.count() > w) out.remove_at(out.count() - 1);
  return true;
}

void sorted_insert_unique(List<int32_t>& v, int32_t x, bool* ok) {
  uint64_t lo = 0;
  while (lo < v.count() && v[lo] < x) ++lo;
  if (lo < v.count() && v[lo] == x) return;
  if (!v.insert(lo, x)) *ok = false;
}

bool spans_equal(const List<Span>& a, const List<Span>& b) {
  if (a.count() != b.count()) return false;
  for (uint64_t i = 0; i < a.count(); ++i) if (a[i].a != b[i].a || a[i].b != b[i].b) return false;
  return true;
}

bool apply(bool a, bool b, int32_t mode) {
  switch (mode) {
    case 1: return a && b;
    case 2: return a || b;
    case 3: return a != b;
    case 4: return a && !b;
    default: return a;
  }
}

bool spans_op(const List<Span>& A, const List<Span>& B, int32_t mode, allocators::IAllocator& al, List<Span>& out) {
  out.clear();
  List<int32_t> xs(al);
  bool ok = true;
  for (const Span& s : A.view()) { sorted_insert_unique(xs, s.a, &ok); sorted_insert_unique(xs, s.b, &ok); }
  for (const Span& s : B.view()) { sorted_insert_unique(xs, s.a, &ok); sorted_insert_unique(xs, s.b, &ok); }
  if (!ok) return false;
  uint64_t pa = 0, pb = 0;
  for (uint64_t i = 0; i + 1 < xs.count(); ++i) {
    const int32_t x0 = xs[i], x1 = xs[i + 1];
    while (pa < A.count() && A[pa].b <= x0) ++pa;
    while (pb < B.count() && B[pb].b <= x0) ++pb;
    const bool in_a = pa < A.count() && A[pa].a <= x0, in_b = pb < B.count() && B[pb].a <= x0;
    if (!apply(in_a, in_b, mode)) continue;
    if (out.count() > 0 && out[out.count() - 1].b == x0) out[out.count() - 1].b = x1;
    else if (!out.push_back(Span{x0, x1})) return false;
  }
  return true;
}

}  // namespace

bool rgn_combine(ArrayView<const Rect> a, ArrayView<const Rect> b, int32_t mode, List<Rect>* out) {
  allocators::IAllocator* al = alloc();
  if (!al) return false;
  List<int32_t> ys(*al);
  bool ok = true;
  for (const Rect& e : a) if (!rect_empty(e)) { sorted_insert_unique(ys, e.top, &ok); sorted_insert_unique(ys, e.bottom, &ok); }
  for (const Rect& e : b) if (!rect_empty(e)) { sorted_insert_unique(ys, e.top, &ok); sorted_insert_unique(ys, e.bottom, &ok); }
  if (!ok) return false;
  List<Rect> result(*al);
  List<Span> sa(*al), sb(*al), res(*al), prev(*al);
  uint64_t prev_start = 0;
  int64_t prev_bottom = INT64_MIN;
  for (uint64_t i = 0; i + 1 < ys.count(); ++i) {
    const int32_t y0 = ys[i], y1 = ys[i + 1];
    if (!spans_in(a, y0, y1, sa) || !spans_in(b, y0, y1, sb) || !spans_op(sa, sb, mode, *al, res)) return false;
    if (res.count() == 0) { prev_bottom = INT64_MIN; continue; }
    if (prev_bottom == y0 && spans_equal(prev, res)) {
      for (uint64_t k = prev_start; k < result.count(); ++k) result[k].bottom = y1;
    } else {
      prev_start = result.count();
      for (const Span& s : res.view()) if (!result.push_back(Rect{s.a, y0, s.b, y1})) return false;
      prev.clear();
      for (const Span& s : res.view()) if (!prev.push_back(s)) return false;
    }
    prev_bottom = y1;
  }
  out->clear();
  for (const Rect& r : result.view()) if (!out->push_back(r)) return false;
  return true;
}

bool rgn_normalize(ArrayView<const Rect> in, List<Rect>* out) { return rgn_combine(in, ArrayView<const Rect>(), 2, out); }

Rect rgn_bound(ArrayView<const Rect> r) {
  Rect b;
  for (const Rect& e : r) b = rect_union(b, e);
  return b;
}

bool rgn_has_point(ArrayView<const Rect> r, int32_t x, int32_t y) {
  for (const Rect& e : r) if (rect_has(e, x, y)) return true;
  return false;
}

bool rgn_hits_rect(ArrayView<const Rect> r, const Rect& q) {
  const Rect n = rect_normalized(q);
  if (rect_empty(n)) return false;
  for (const Rect& e : r) if (!rect_empty(rect_and(e, n))) return true;
  return false;
}

bool rgn_contains_rect(ArrayView<const Rect> r, const Rect& q) {
  allocators::IAllocator* al = alloc();
  const Rect n = rect_normalized(q);
  if (!al || rect_empty(n)) return false;
  List<Rect> diff(*al);
  const Rect one[1] = {n};
  return rgn_combine(ArrayView<const Rect>(one, 1), r, 4, &diff) && diff.count() == 0;
}

void rgn_offset(List<Rect>& r, int32_t dx, int32_t dy) {
  for (Rect& e : r.view()) {
    e.left = sat_add(e.left, dx); e.right = sat_add(e.right, dx);
    e.top = sat_add(e.top, dy); e.bottom = sat_add(e.bottom, dy);
  }
}

bool rgn_assign(List<Rect>& dst, ArrayView<const Rect> src) { return rgn_normalize(src, &dst); }

bool rgn_from_rect(List<Rect>* out, const Rect& r) {
  out->clear();
  const Rect n = rect_normalized(r);
  return rect_empty(n) || out->push_back(n);
}

bool rgn_equal(ArrayView<const Rect> a, ArrayView<const Rect> b) {
  allocators::IAllocator* al = alloc();
  if (!al) return false;
  List<Rect> x(*al);
  return rgn_combine(a, b, 3, &x) && x.count() == 0;
}

int32_t region_complexity(ArrayView<const Rect> r) { return r.size() == 0 ? 1 : r.size() == 1 ? 2 : 3; }

bool rgn_polygon(List<Rect>* out, ArrayView<const Point> pts, ArrayView<const uint32_t> counts, bool winding, const Rect& limit) {
  allocators::IAllocator* al = alloc();
  if (!al) return false;
  struct Edge { double x0, y0, x1, y1; int32_t dir; };
  List<Edge> edges(*al);
  int32_t ymin = INT32_MAX, ymax = INT32_MIN;
  uint64_t base = 0;
  for (uint32_t cnt : counts) {
    if (cnt > pts.size() - base) return false;
    if (cnt >= 2) {
      for (uint32_t i = 0; i < cnt; ++i) {
        const Point p = pts[base + i], q = pts[base + (i + 1) % cnt];
        if (p.y == q.y) continue;
        Edge e{double(p.x), double(p.y), double(q.x), double(q.y), 1};
        if (p.y > q.y) { e = Edge{double(q.x), double(q.y), double(p.x), double(p.y), -1}; }
        if (!edges.push_back(e)) return false;
        if (int32_t(e.y0) < ymin) ymin = int32_t(e.y0);
        if (int32_t(e.y1) > ymax) ymax = int32_t(e.y1);
      }
    }
    base += cnt;
  }
  List<Rect> result(*al);
  if (edges.count() > 0) {
    if (ymin < limit.top) ymin = limit.top;
    if (ymax > limit.bottom) ymax = limit.bottom;
    if (int64_t{ymax} - ymin > (1 << 17) || (ymax > ymin && uint64_t(ymax - ymin) * edges.count() > 200000000ull)) return false;
    List<double> xs(*al);
    List<int32_t> dirs(*al);
    List<Span> prev(*al), cur(*al);
    uint64_t prev_start = 0;
    int64_t prev_bottom = INT64_MIN;
    for (int32_t y = ymin; y < ymax; ++y) {
      xs.clear();
      dirs.clear();
      const double ys = double(y) + 0.5;
      for (const Edge& e : edges.view()) {
        if (!(ys >= e.y0 && ys < e.y1)) continue;
        const double x = e.x0 + (ys - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0);
        uint64_t i = xs.count();
        if (!xs.push_back(x) || !dirs.push_back(e.dir)) return false;
        while (i > 0 && xs[i - 1] > xs[i]) {
          const double tx = xs[i - 1]; xs[i - 1] = xs[i]; xs[i] = tx;
          const int32_t td = dirs[i - 1]; dirs[i - 1] = dirs[i]; dirs[i] = td;
          --i;
        }
      }
      cur.clear();
      auto add_span = [&](double xl, double xr) -> bool {
        int64_t a = int64_t(g_floor(xl - 0.5)) + 1, b = int64_t(g_floor(xr - 0.5)) + 1;
        if (a < limit.left) a = limit.left;
        if (b > limit.right) b = limit.right;
        if (b <= a) return true;
        if (cur.count() > 0 && cur[cur.count() - 1].b >= a) {
          if (b > cur[cur.count() - 1].b) cur[cur.count() - 1].b = int32_t(b);
          return true;
        }
        return cur.push_back(Span{int32_t(a), int32_t(b)});
      };
      if (!winding) {
        for (uint64_t i = 0; i + 1 < xs.count(); i += 2) if (!add_span(xs[i], xs[i + 1])) return false;
      } else {
        int32_t w = 0;
        double start = 0;
        for (uint64_t i = 0; i < xs.count(); ++i) {
          const int32_t before = w;
          w += dirs[i];
          if (before == 0 && w != 0) start = xs[i];
          else if (before != 0 && w == 0 && !add_span(start, xs[i])) return false;
        }
      }
      if (cur.count() == 0) { prev_bottom = INT64_MIN; continue; }
      if (prev_bottom == y && spans_equal(prev, cur)) {
        for (uint64_t k = prev_start; k < result.count(); ++k) result[k].bottom = y + 1;
      } else {
        prev_start = result.count();
        for (const Span& s : cur.view()) if (!result.push_back(Rect{s.a, y, s.b, y + 1})) return false;
        prev.clear();
        for (const Span& s : cur.view()) if (!prev.push_back(s)) return false;
      }
      prev_bottom = int64_t{y} + 1;
    }
  }
  out->clear();
  for (const Rect& r : result.view()) if (!out->push_back(r)) return false;
  return true;
}

bool rgn_ellipse(List<Rect>* out, const Rect& rect) {
  allocators::IAllocator* al = alloc();
  const Rect r = rect_normalized(rect);
  out->clear();
  if (!al || rect_empty(r)) return al != nullptr;
  if (int64_t{r.bottom} - r.top > (1 << 16)) return false;
  const double a = (double(r.right) - r.left) / 2, b = (double(r.bottom) - r.top) / 2, cx = r.left + a, cy = r.top + b;
  List<Rect> result(*al);
  int32_t prev_l = 0, prev_r = 0, prev_top = 0;
  bool have = false;
  for (int32_t y = r.top; y < r.bottom; ++y) {
    const double dy = (double(y) + 0.5 - cy) / b;
    const double t = 1 - dy * dy;
    const double half = t > 0 ? a * g_sqrt(t) : 0;
    const int32_t l = int32_t(g_floor(cx - half - 0.5)) + 1, rr = int32_t(g_floor(cx + half - 0.5)) + 1;
    if (rr <= l) { if (have) { result.push_back(Rect{prev_l, prev_top, prev_r, y}); have = false; } continue; }
    if (have && l == prev_l && rr == prev_r) continue;
    if (have) result.push_back(Rect{prev_l, prev_top, prev_r, y});
    prev_l = l; prev_r = rr; prev_top = y; have = true;
  }
  if (have) result.push_back(Rect{prev_l, prev_top, prev_r, r.bottom});
  return rgn_normalize(result.view(), out);
}

bool rgn_round_rect(List<Rect>* out, const Rect& rect, int32_t ew, int32_t eh) {
  allocators::IAllocator* al = alloc();
  const Rect r = rect_normalized(rect);
  out->clear();
  if (!al || rect_empty(r)) return al != nullptr;
  if (int64_t{r.bottom} - r.top > (1 << 16)) return false;
  const int32_t w = r.right - r.left, h = r.bottom - r.top;
  ew = iabs(ew) > w ? w : iabs(ew);
  eh = iabs(eh) > h ? h : iabs(eh);
  if (ew == 0 || eh == 0) return rgn_from_rect(out, r);
  const double a = ew / 2.0, b = eh / 2.0;
  List<Rect> result(*al);
  for (int32_t y = r.top; y < r.bottom; ++y) {
    const double ys = double(y) + 0.5;
    double dy = 0;
    if (ys < r.top + b) dy = (r.top + b - ys) / b;
    else if (ys > r.bottom - b) dy = (ys - (r.bottom - b)) / b;
    const double t = 1 - dy * dy;
    const double inset = a - (t > 0 ? a * g_sqrt(t) : 0);
    const int32_t l = int32_t(g_floor(r.left + inset - 0.5)) + 1, rr = int32_t(g_floor(r.right - inset - 0.5)) + 1;
    if (rr > l) result.push_back(Rect{l, y, rr, y + 1});
  }
  return rgn_normalize(result.view(), out);
}

Region* new_region(uint64_t pid, ArrayView<const Rect> rects) {
  Region* r = make_object<Region>(kObjRegion, pid);
  if (!r) return nullptr;
  if (!rgn_normalize(rects, &r->rects)) { destroy_object(r->handle); return nullptr; }
  return r;
}

namespace {

constexpr uint32_t kRgnError = 0, kRgnNull = 1, kRgnSimple = 2, kRgnComplex = 3;

Region* region_arg(Call& c, uint32_t h) {
  Region* r = find_region(h);
  if (!r || (!r->stock && r->pid != c.pid())) { c.set_last_error(kErrorInvalidHandle); return nullptr; }
  return r;
}

uint64_t finish_new(Region* r) { return r ? r->handle : 0; }

uint64_t NtGdiCreateRectRgn(Call& c) {
  const Rect rc{c.argi(0), c.argi(1), c.argi(2), c.argi(3)};
  List<Rect> one(*alloc());
  rgn_from_rect(&one, rc);
  return finish_new(new_region(c.pid(), one.view()));
}

uint64_t NtGdiCreateEllipticRgn(Call& c) {
  List<Rect> rects(*alloc());
  if (!rgn_ellipse(&rects, Rect{c.argi(0), c.argi(1), c.argi(2), c.argi(3)})) return c.fail(kErrorInvalidParameter);
  return finish_new(new_region(c.pid(), rects.view()));
}

uint64_t NtGdiCreateRoundRectRgn(Call& c) {
  List<Rect> rects(*alloc());
  if (!rgn_round_rect(&rects, Rect{c.argi(0), c.argi(1), c.argi(2), c.argi(3)}, c.argi(4), c.argi(5))) return c.fail(kErrorInvalidParameter);
  return finish_new(new_region(c.pid(), rects.view()));
}

uint64_t NtGdiSetRectRgn(Call& c) {
  Region* r = region_arg(c, c.arg32(0));
  if (!r) return 0;
  return ret_bool(rgn_from_rect(&r->rects, Rect{c.argi(1), c.argi(2), c.argi(3), c.argi(4)}));
}

uint64_t NtGdiCombineRgn(Call& c) {
  Region* dst = region_arg(c, c.arg32(0));
  Region* s1 = region_arg(c, c.arg32(1));
  const int32_t mode = c.argi(3);
  if (!dst || !s1 || mode < 1 || mode > 5) return kRgnError;
  Region* s2 = nullptr;
  if (mode != 5) { s2 = region_arg(c, c.arg32(2)); if (!s2) return kRgnError; }
  List<Rect> tmp(*alloc());
  if (mode == 5) { if (!rgn_assign(tmp, s1->rects.view())) return kRgnError; }
  else if (!rgn_combine(s1->rects.view(), s2->rects.view(), mode, &tmp)) return kRgnError;
  if (!rgn_assign(dst->rects, tmp.view())) return kRgnError;
  return uint64_t(region_complexity(dst->rects.view()));
}

uint64_t NtGdiEqualRgn(Call& c) {
  Region* a = region_arg(c, c.arg32(0));
  Region* b = region_arg(c, c.arg32(1));
  return a && b && rgn_equal(a->rects.view(), b->rects.view()) ? 1 : 0;
}

uint64_t NtGdiOffsetRgn(Call& c) {
  Region* r = region_arg(c, c.arg32(0));
  if (!r) return kRgnError;
  rgn_offset(r->rects, c.argi(1), c.argi(2));
  return uint64_t(region_complexity(r->rects.view()));
}

uint64_t NtGdiGetRgnBox(Call& c) {
  Region* r = region_arg(c, c.arg32(0));
  if (!r) return kRgnError;
  const Rect b = rgn_bound(r->rects.view());
  if (!c.write_struct(c.arg(1), b)) return c.fail(kErrorInvalidParameter);
  return uint64_t(region_complexity(r->rects.view()));
}

uint64_t NtGdiPtInRegion(Call& c) {
  Region* r = region_arg(c, c.arg32(0));
  return r && rgn_has_point(r->rects.view(), c.argi(1), c.argi(2)) ? 1 : 0;
}

uint64_t NtGdiRectInRegion(Call& c) {
  Region* r = region_arg(c, c.arg32(0));
  Rect q;
  if (!r || !c.read_struct(c.arg(1), &q)) return 0;
  return rgn_hits_rect(r->rects.view(), q) ? 1 : 0;
}

struct RgnDataHeader { uint32_t size, type, count, rgn_size; Rect bound; };

uint64_t NtGdiGetRegionData(Call& c) {
  Region* r = region_arg(c, c.arg32(0));
  if (!r) return 0;
  const uint32_t count = c.arg32(1);
  const uint64_t need = 32 + uint64_t{16} * r->rects.count();
  if (c.arg(2) == 0) return need;
  if (count < need) return 0;
  RgnDataHeader h{32, 1, uint32_t(r->rects.count()), uint32_t(16 * r->rects.count()), rgn_bound(r->rects.view())};
  if (!c.write_struct(c.arg(2), h)) return 0;
  uint64_t at = c.arg(2) + 32;
  for (const Rect& e : r->rects.view()) {
    if (!c.write_struct(at, e)) return 0;
    at += 16;
  }
  return need;
}

uint64_t NtGdiExtCreateRegion(Call& c) {
  const uint64_t xf = c.arg(0), data = c.arg(2);
  const uint32_t size = c.arg32(1);
  RgnDataHeader h{};
  if (size < 32 || !c.read_struct(data, &h) || h.size != 32 || h.type != 1) return c.fail(kErrorInvalidParameter);
  if (h.count > (size - 32) / 16) return c.fail(kErrorInvalidParameter);
  List<Rect> rects(*alloc());
  for (uint32_t i = 0; i < h.count; ++i) {
    Rect e;
    if (!c.read_struct(data + 32 + uint64_t{i} * 16, &e)) return c.fail(kErrorInvalidParameter);
    rects.push_back(rect_normalized(e));
  }
  List<Rect> out(*alloc());
  float m[6] = {1, 0, 0, 1, 0, 0};
  if (xf != 0 && !c.read_array(xf, m, 6)) return c.fail(kErrorInvalidParameter);
  if (xf == 0 || (m[1] == 0 && m[2] == 0 && m[0] > 0 && m[3] > 0)) {
    for (Rect& e : rects.view()) {
      e.left = round_i32(e.left * m[0] + m[4]); e.right = round_i32(e.right * m[0] + m[4]);
      e.top = round_i32(e.top * m[3] + m[5]); e.bottom = round_i32(e.bottom * m[3] + m[5]);
    }
    if (!rgn_normalize(rects.view(), &out)) return 0;
  } else {
    List<Rect> acc(*alloc()), piece(*alloc()), tmp(*alloc());
    for (const Rect& e : rects.view()) {
      const double xs[4] = {double(e.left), double(e.right), double(e.right), double(e.left)}, ys[4] = {double(e.top), double(e.top), double(e.bottom), double(e.bottom)};
      Point pts[4];
      for (int k = 0; k < 4; ++k) {
        pts[k].x = round_i32(xs[k] * m[0] + ys[k] * m[2] + m[4]);
        pts[k].y = round_i32(xs[k] * m[1] + ys[k] * m[3] + m[5]);
      }
      const uint32_t cnt = 4;
      if (!rgn_polygon(&piece, ArrayView<const Point>(pts, 4), ArrayView<const uint32_t>(&cnt, 1), true, Rect{-65536, -65536, 65536, 65536})) return 0;
      if (!rgn_combine(acc.view(), piece.view(), 2, &tmp) || !rgn_assign(acc, tmp.view())) return 0;
    }
    if (!rgn_assign(out, acc.view())) return 0;
  }
  return finish_new(new_region(c.pid(), out.view()));
}

uint64_t NtGdiGetRandomRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Region* dst = region_arg(c, c.arg32(1));
  const int32_t code = c.argi(2) & 0xFFFF;
  if (!dc || !dst) return 0;
  List<Rect> out(*alloc());
  Bitmap* bm = nullptr;
  Surface s = dc_surface(*dc, &bm);
  if (code == 1) { if (!dc->s.has_clip) return 0; rgn_assign(out, dc->s.clip.view()); }
  else if (code == 2) { if (!dc->s.has_meta) return 0; rgn_assign(out, dc->s.meta.view()); }
  else if (code == 3) return 0;
  else if (code == 4) {
    if (dc->s.has_clip && dc->s.has_meta) rgn_combine(dc->s.clip.view(), dc->s.meta.view(), 1, &out);
    else if (dc->s.has_clip) rgn_assign(out, dc->s.clip.view());
    else if (dc->s.has_meta) rgn_assign(out, dc->s.meta.view());
    else return 0;
  } else if (code == 5) {
    if (!s.valid()) return 0;
    if (!dc_effective_clip(*dc, s, &out)) return 0;
    if (!dc->screen) rgn_offset(out, 0, 0);
  } else return 0;
  return ret_bool(rgn_assign(dst->rects, out.view()));
}

}  // namespace

void register_region_services() {
  GDI_REGISTER(CreateRectRgn, NtGdiCreateRectRgn);
  GDI_REGISTER(CreateEllipticRgn, NtGdiCreateEllipticRgn);
  GDI_REGISTER(CreateRoundRectRgn, NtGdiCreateRoundRectRgn);
  GDI_REGISTER(SetRectRgn, NtGdiSetRectRgn);
  GDI_REGISTER(CombineRgn, NtGdiCombineRgn);
  GDI_REGISTER(EqualRgn, NtGdiEqualRgn);
  GDI_REGISTER(OffsetRgn, NtGdiOffsetRgn);
  GDI_REGISTER(GetRgnBox, NtGdiGetRgnBox);
  GDI_REGISTER(PtInRegion, NtGdiPtInRegion);
  GDI_REGISTER(RectInRegion, NtGdiRectInRegion);
  GDI_REGISTER(GetRegionData, NtGdiGetRegionData);
  GDI_REGISTER(ExtCreateRegion, NtGdiExtCreateRegion);
  GDI_REGISTER(GetRandomRgn, NtGdiGetRandomRgn);
}

}  // namespace process::win32k::gdi
