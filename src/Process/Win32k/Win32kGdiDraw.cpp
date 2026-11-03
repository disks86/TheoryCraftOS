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

// The pixel engine: a drawing session (Painter), pens, brushes and patterns, lines, filled and outlined shapes, pixels, flood fill and region painting.
namespace process::win32k::gdi {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Painter

Painter::Painter(Dc& dc) : dc_(dc), clip_(*alloc()) {
  surface_ = dc_surface(dc, &bitmap_);
  if (!surface_.valid()) return;
  if (bitmap_ && bitmap_->dib) dib_sync_in(*bitmap_);
  ox_ = dc.origin_x;
  oy_ = dc.origin_y;
  if (!dc_effective_clip(dc, surface_, &clip_)) return;
  clip_bound_ = rgn_bound(clip_.view());
  ok_ = true;
}

Painter::~Painter() {
  if (rect_empty(dirty_) || !bitmap_) return;
  if (bitmap_->dib) dib_sync_out(*bitmap_);
  if (bitmap_->is_screen) sink_changed(dirty_);
}

bool Painter::visible(int32_t x, int32_t y) const {
  const int64_t sx = int64_t{x} + ox_, sy = int64_t{y} + oy_;
  if (sx < clip_bound_.left || sx >= clip_bound_.right || sy < clip_bound_.top || sy >= clip_bound_.bottom) return false;
  if (clip_.count() == 1) return true;
  for (const Rect& r : clip_.view()) if (sx >= r.left && sx < r.right && sy >= r.top && sy < r.bottom) return true;
  return false;
}

void Painter::mark(const Rect& d) {
  const Rect s{sat_add(d.left, ox_), sat_add(d.top, oy_), sat_add(d.right, ox_), sat_add(d.bottom, oy_)};
  dirty_ = rect_union(dirty_, rect_and(s, clip_bound_));
}

void Painter::plot_raw(int32_t x, int32_t y, ColorRef c) {
  if (!visible(x, y)) return;
  surface_.put(x + ox_, y + oy_, c);
  mark(Rect{x, y, x + 1, y + 1});
}

void Painter::plot(int32_t x, int32_t y, ColorRef c, int32_t rop2) {
  if (!visible(x, y)) return;
  if (rop2 != 13) c = rop2_apply(rop2, surface_.get(x + ox_, y + oy_), c);
  surface_.put(x + ox_, y + oy_, c);
  mark(Rect{x, y, x + 1, y + 1});
}

ColorRef Painter::read(int32_t x, int32_t y) {
  if (!visible(x, y)) return kClrInvalid;
  return surface_.get(x + ox_, y + oy_);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Colours and patterns

ColorRef resolve_color(const Dc& dc, ColorRef c) {
  switch (c >> 24) {
    case 0x01: {      // PALETTEINDEX
      if (Palette* p = find_palette(dc.s.palette)) {
        const uint32_t i = c & 0xFFFF;
        if (i < p->entries.count()) return p->entries[i] & 0xFFFFFF;
      }
      return 0;
    }
    case 0x10: {      // DIBINDEX: index into the colour table of the selected bitmap
      Bitmap* b = dc.memory ? find_bitmap(dc.bitmap) : nullptr;
      const uint32_t i = c & 0xFFFF;
      if (b && i < b->colors.count()) return b->colors[i] & 0xFFFFFF;
      return 0;
    }
    default: return c & 0xFFFFFF;
  }
}

namespace {

bool hatch_bit(uint32_t style, int32_t x, int32_t y) {
  const int32_t px = x & 7, py = y & 7;
  switch (style) {
    case 0: return py == 3;
    case 1: return px == 3;
    case 2: return px == py;
    case 3: return px + py == 7;
    case 4: return py == 3 || px == 3;
    case 5: return px == py || px + py == 7;
    default: return false;
  }
}

int32_t pmod(int64_t v, int32_t m) {
  int64_t r = v % m;
  return int32_t(r < 0 ? r + m : r);
}

}  // namespace

ColorRef PatternSource::at(int32_t x, int32_t y, bool* transparent) const {
  *transparent = false;
  if (!brush || brush->style == 0) return solid;
  if (brush->style == 2) {
    if (hatch_bit(brush->hatch, x - org_x, y - org_y)) return solid;
    if (!opaque) { *transparent = true; return 0; }
    return back;
  }
  if (brush->pw > 0 && brush->ph > 0) {
    const int32_t px = pmod(int64_t{x} - org_x, brush->pw), py = pmod(int64_t{y} - org_y, brush->ph);
    const uint64_t idx = uint64_t(py) * uint32_t(brush->pw) + uint32_t(px);
    if (idx < brush->pattern.count()) {
      const uint32_t v = brush->pattern[idx];
      if (brush->mono) return v ? back : text;
      return v & 0xFFFFFF;
    }
  }
  return solid;
}

bool pattern_of_brush(const Dc& dc, const Brush* b, PatternSource* out) {
  if (!b) b = find_brush(stock_handle(0));
  out->brush = b;
  out->text = dc.s.text_color & 0xFFFFFF;
  out->back = dc.s.bk_color & 0xFFFFFF;
  out->opaque = dc.s.bk_mode == 2;
  out->org_x = dc.s.brush_org.x;
  out->org_y = dc.s.brush_org.y;
  out->is_null = b && b->style == 1;
  out->solid = b ? (b->dc_brush ? dc.s.dc_brush_color & 0xFFFFFF : resolve_color(dc, b->color)) : 0xFFFFFF;
  if (b && b->style == 2) out->solid = resolve_color(dc, b->color);
  return true;
}

bool pattern_of_dc(const Dc& dc, PatternSource* out) { return pattern_of_brush(dc, find_brush(dc.s.brush), out); }

void fill_rect_pattern(Painter& p, const Rect& dev, const PatternSource& src, uint32_t rop) {
  if (src.is_null && (rop >> 16 & 0xFF) == 0xF0) return;
  const Rect r = rect_normalized(dev);
  if (rect_empty(r)) return;
  const bool copy = (rop >> 16 & 0xFF) == 0xF0, solid = !src.brush || src.brush->style == 0;
  for (const Rect& cr : p.clip().view()) {
    const Rect q = rect_and(Rect{sat_add(r.left, p.ox()), sat_add(r.top, p.oy()), sat_add(r.right, p.ox()), sat_add(r.bottom, p.oy())}, cr);
    if (rect_empty(q)) continue;
    Surface& s = p.surface();
    for (int32_t y = q.top; y < q.bottom; ++y) {
      for (int32_t x = q.left; x < q.right; ++x) {
        bool transparent = false;
        const ColorRef pat = solid ? src.solid : src.at(x - p.ox(), y - p.oy(), &transparent);
        if (copy) { if (!transparent) s.put(x, y, pat); }
        else s.put(x, y, rop3_apply(rop, pat, 0, s.get(x, y)));
      }
    }
    p.mark(Rect{q.left - p.ox(), q.top - p.oy(), q.right - p.ox(), q.bottom - p.oy()});
  }
}

void fill_rect_solid(Painter& p, const Rect& dev, ColorRef c, int32_t rop2) {
  const Rect r = rect_normalized(dev);
  if (rect_empty(r)) return;
  for (const Rect& cr : p.clip().view()) {
    const Rect q = rect_and(Rect{sat_add(r.left, p.ox()), sat_add(r.top, p.oy()), sat_add(r.right, p.ox()), sat_add(r.bottom, p.oy())}, cr);
    if (rect_empty(q)) continue;
    Surface& s = p.surface();
    for (int32_t y = q.top; y < q.bottom; ++y) for (int32_t x = q.left; x < q.right; ++x) s.put(x, y, rop2 == 13 ? c : rop2_apply(rop2, s.get(x, y), c));
    p.mark(Rect{q.left - p.ox(), q.top - p.oy(), q.right - p.ox(), q.bottom - p.oy()});
  }
}

bool fill_region_pattern(Painter& p, ArrayView<const Rect> rects, const PatternSource& src, uint32_t rop) {
  for (const Rect& r : rects) fill_rect_pattern(p, r, src, rop);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Pens

namespace {

struct PenInfo {
  bool null = false, geometric = false, inside_frame = false;
  int32_t width = 1;
  ColorRef color = 0;
  uint32_t kind = 0, cap = 0, join = 0;
  ArrayView<const uint32_t> dashes;
};

const uint32_t kDash[] = {18, 6}, kDot[] = {3, 3}, kDashDot[] = {9, 6, 3, 6}, kDashDotDot[] = {9, 3, 3, 3, 3, 3};

bool get_pen_info(const Dc& dc, PenInfo* out) {
  const Pen* pen = find_pen(dc.s.pen);
  if (!pen) { out->color = 0; return true; }
  out->kind = pen->style & 0xF;
  out->null = out->kind == 5;
  out->color = pen->dc_pen ? dc.s.dc_pen_color & 0xFFFFFF : resolve_color(dc, pen->color);
  out->geometric = pen->ext ? (pen->style & 0xF0000) != 0 : pen->width > 1;
  out->inside_frame = out->kind == 6;
  out->cap = pen->ext ? pen->style & 0xF00 : 0;
  out->join = pen->ext ? pen->style & 0xF000 : 0;
  int32_t w = 1;
  if (out->geometric) {
    const Point a = lp_to_dp(dc, Point{0, 0}), b = lp_to_dp(dc, Point{clamp_i32(pen->width), 0}), c = lp_to_dp(dc, Point{0, clamp_i32(pen->width)});
    const int32_t wx = iabs(sat_sub(b.x, a.x)), wy = iabs(sat_sub(c.y, a.y));
    w = wx > wy ? wx : wy;
    if (w < 1) w = 1;
    if (w > 4096) w = 4096;
  }
  out->width = w;
  switch (out->kind) {
    case 1: out->dashes = ArrayView<const uint32_t>(kDash); break;
    case 2: out->dashes = ArrayView<const uint32_t>(kDot); break;
    case 3: out->dashes = ArrayView<const uint32_t>(kDashDot); break;
    case 4: out->dashes = ArrayView<const uint32_t>(kDashDotDot); break;
    case 7: out->dashes = ArrayView<const uint32_t>(pen->dashes.view()); break;
    default: break;
  }
  if (out->geometric && !pen->ext) out->dashes = ArrayView<const uint32_t>();
  return true;
}

struct DashState {
  uint64_t index = 0;
  uint32_t left = 0;
  bool started = false;
};

bool dash_next(const PenInfo& pen, DashState* st) {
  if (pen.dashes.size() == 0) return true;
  if (!st->started) { st->started = true; st->index = 0; st->left = pen.dashes[0] ? pen.dashes[0] : 1; }
  const bool on = (st->index & 1) == 0;
  if (--st->left == 0) {
    st->index = (st->index + 1) % pen.dashes.size();
    st->left = pen.dashes[st->index] ? pen.dashes[st->index] : 1;
  }
  return on;
}

// Liang-Barsky: the part of the segment inside `box` (inclusive); false if none.
bool clip_segment(Point* a, Point* b, const Rect& box) {
  double t0 = 0, t1 = 1;
  const double dx = double(b->x) - a->x, dy = double(b->y) - a->y;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {double(a->x) - box.left, double(box.right - 1) - a->x, double(a->y) - box.top, double(box.bottom - 1) - a->y};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0) { if (q[i] < 0) return false; continue; }
    const double r = q[i] / p[i];
    if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
    else { if (r < t0) return false; if (r < t1) t1 = r; }
  }
  const Point na{round_i32(a->x + t0 * dx), round_i32(a->y + t0 * dy)}, nb{round_i32(a->x + t1 * dx), round_i32(a->y + t1 * dy)};
  *a = na;
  *b = nb;
  return true;
}

void cosmetic_line(Painter& p, const PenInfo& pen, DashState* dash, Point a, Point b, bool include_last, int32_t rop2) {
  const int64_t span = int64_t{a.x > b.x ? a.x : b.x} - (a.x > b.x ? b.x : a.x);
  const int64_t spany = int64_t{a.y > b.y ? a.y : b.y} - (a.y > b.y ? b.y : a.y);
  if (span > 100000 || spany > 100000) {
    const Rect box{p.clip_bound().left - p.ox(), p.clip_bound().top - p.oy(), p.clip_bound().right - p.ox(), p.clip_bound().bottom - p.oy()};
    if (rect_empty(box) || !clip_segment(&a, &b, box)) return;
  }
  if (!p.spend(uint64_t(span > spany ? span : spany) + 1)) return;
  const bool opaque_gap = p.dc().s.bk_mode == 2;
  int32_t x = a.x, y = a.y;
  const int32_t dx = iabs(b.x - a.x) , dy = -iabs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
  int64_t err = int64_t{dx} + dy;
  for (;;) {
    const bool last = x == b.x && y == b.y;
    if (!last || include_last) {
      if (dash_next(pen, dash)) p.plot(x, y, pen.color, rop2);
      else if (opaque_gap) p.plot(x, y, p.dc().s.bk_color & 0xFFFFFF, rop2);
    }
    if (last) break;
    const int64_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x += sx; }
    if (e2 <= dx) { err += dx; y += sy; }
  }
}

struct UnitCircle {
  double c[48], s[48];
  bool ready = false;
};
UnitCircle g_circle;
constexpr int kCircleSides = 48;

void ensure_circle() {
  if (g_circle.ready) return;
  for (int i = 0; i < kCircleSides; ++i) {
    const double a = 6.283185307179586 * i / kCircleSides;
    g_circle.c[i] = g_cos(a);
    g_circle.s[i] = g_sin(a);
  }
  g_circle.ready = true;
}

bool add_polygon(List<Rect>& acc, const Rect& limit, ArrayView<const Point> pts) {
  List<Rect> piece(*alloc()), merged(*alloc());
  const uint32_t n = uint32_t(pts.size());
  if (!rgn_polygon(&piece, pts, ArrayView<const uint32_t>(&n, 1), true, limit)) return false;
  if (piece.count() == 0) return true;
  if (!rgn_combine(acc.view(), piece.view(), 2, &merged)) return false;
  return rgn_assign(acc, merged.view());
}

bool add_disc(List<Rect>& acc, const Rect& limit, double cx, double cy, double r) {
  ensure_circle();
  Point pts[kCircleSides];
  for (int i = 0; i < kCircleSides; ++i) pts[i] = Point{round_i32(cx + r * g_circle.c[i]), round_i32(cy + r * g_circle.s[i])};
  return add_polygon(acc, limit, ArrayView<const Point>(pts, kCircleSides));
}

bool stroke_region(const PenInfo& pen, float miter_limit, ArrayView<const Point> in, bool closed, const Rect& limit, List<Rect>& acc) {
  allocators::IAllocator& al = *alloc();
  List<Point> pts(al);
  for (const Point& q : in) if (pts.count() == 0 || pts[pts.count() - 1].x != q.x || pts[pts.count() - 1].y != q.y) pts.push_back(q);
  if (closed && pts.count() > 1 && pts[0].x == pts[pts.count() - 1].x && pts[0].y == pts[pts.count() - 1].y) pts.remove_at(pts.count() - 1);
  const double hw = pen.width / 2.0;
  if (pts.count() > 2048) return false;      // wide strokes cost a polygon fill per segment
  const uint64_t n = pts.count();
  if (n == 0) return true;
  if (n == 1) {
    if (pen.cap == 0 || pen.cap == 0x100) add_disc(acc, limit, pts[0].x, pts[0].y, hw);
  } else {
    const uint64_t segs = closed ? n : n - 1;
    for (uint64_t i = 0; i < segs; ++i) {
      const Point a = pts[i], b = pts[(i + 1) % n];
      const double dx = double(b.x) - a.x, dy = double(b.y) - a.y, len = g_sqrt(dx * dx + dy * dy);
      if (len == 0) continue;
      const double ux = dx / len, uy = dy / len, nx = -uy * hw, ny = ux * hw;
      double ax = a.x, ay = a.y, bx = b.x, by = b.y;
      if (!closed && pen.cap == 0x100) {      // square caps stick out by half the width
        if (i == 0) { ax -= ux * hw; ay -= uy * hw; }
        if (i == segs - 1) { bx += ux * hw; by += uy * hw; }
      }
      const Point quad[4] = {{round_i32(ax + nx), round_i32(ay + ny)}, {round_i32(bx + nx), round_i32(by + ny)}, {round_i32(bx - nx), round_i32(by - ny)}, {round_i32(ax - nx), round_i32(ay - ny)}};
      add_polygon(acc, limit, ArrayView<const Point>(quad, 4));
      // the join with the next segment
      if (i + 1 < segs || closed) {
        const Point c = pts[(i + 2) % n];
        const double ex = double(c.x) - b.x, ey = double(c.y) - b.y, elen = g_sqrt(ex * ex + ey * ey);
        if (elen == 0) continue;
        if (pen.join == 0) add_disc(acc, limit, b.x, b.y, hw);
        else {
          const double vx = ex / elen, vy = ey / elen, mx = -vy * hw, my = vx * hw;
          const double cross = ux * vy - uy * vx;
          const double sgn = cross > 0 ? -1 : 1;      // the outer side of the turn
          const Point bevel[3] = {{b.x, b.y}, {round_i32(b.x + sgn * nx), round_i32(b.y + sgn * ny)}, {round_i32(b.x + sgn * mx), round_i32(b.y + sgn * my)}};
          add_polygon(acc, limit, ArrayView<const Point>(bevel, 3));
          if (pen.join == 0x2000) {
            const double cosang = ux * vx + uy * vy;      // the angle between the directions
            const double denom = 1 + cosang;
            if (denom > 1e-6) {
              const double ratio = g_sqrt(2 / denom);      // miter length / pen width
              if (ratio <= miter_limit) {
                const double bx2 = (sgn * nx + sgn * mx), by2 = (sgn * ny + sgn * my);
                const double k = 1 / denom;
                const Point tip = {round_i32(b.x + bx2 * k), round_i32(b.y + by2 * k)};
                const Point mq[4] = {{b.x, b.y}, bevel[1], tip, bevel[2]};
                add_polygon(acc, limit, ArrayView<const Point>(mq, 4));
              }
            }
          }
        }
      }
    }
    if (!closed && pen.cap == 0) {
      add_disc(acc, limit, pts[0].x, pts[0].y, hw);
      add_disc(acc, limit, pts[n - 1].x, pts[n - 1].y, hw);
    }
  }
  return true;
}

void stroke_wide(Painter& p, const PenInfo& pen, ArrayView<const Point> in, bool closed, int32_t rop2) {
  if (rect_empty(p.clip_bound())) return;
  const Rect limit{p.clip_bound().left - p.ox() - 2, p.clip_bound().top - p.oy() - 2, p.clip_bound().right - p.ox() + 2, p.clip_bound().bottom - p.oy() + 2};
  List<Rect> acc(*alloc());
  if (!stroke_region(pen, p.dc().s.miter_limit, in, closed, limit, acc)) return;
  for (const Rect& r : acc.view()) fill_rect_solid(p, r, pen.color, rop2);
}

}  // namespace

bool stroke_to_region(const Dc& dc, ArrayView<const Point> pts, bool closed, List<Rect>* out) {
  PenInfo pen;
  get_pen_info(dc, &pen);
  if (pen.null) { out->clear(); return true; }
  if (pen.width < 1) pen.width = 1;
  List<Rect> acc(*alloc());
  if (!stroke_region(pen, dc.s.miter_limit, pts, closed, Rect{-1 << 20, -1 << 20, 1 << 20, 1 << 20}, acc)) return false;
  return rgn_assign(*out, acc.view());
}

bool pen_is_null(const Dc& dc) {
  PenInfo pi;
  get_pen_info(dc, &pi);
  return pi.null;
}

void draw_line_pen(Painter& p, Point a, Point b, bool include_last) {
  PenInfo pen;
  get_pen_info(p.dc(), &pen);
  if (pen.null) return;
  if (pen.width > 1) {
    const Point seg[2] = {a, b};
    stroke_wide(p, pen, ArrayView<const Point>(seg, 2), false, p.dc().s.rop2);
    return;
  }
  DashState dash;
  cosmetic_line(p, pen, &dash, a, b, include_last, p.dc().s.rop2);
}

void stroke_polyline(Painter& p, ArrayView<const Point> pts, bool closed) {
  PenInfo pen;
  get_pen_info(p.dc(), &pen);
  if (pen.null || pts.size() == 0) return;
  if (pen.width > 1) { stroke_wide(p, pen, pts, closed, p.dc().s.rop2); return; }
  DashState dash;
  if (pts.size() == 1) { p.plot(pts[0].x, pts[0].y, pen.color, p.dc().s.rop2); return; }
  const uint64_t segs = closed ? pts.size() : pts.size() - 1;
  for (uint64_t i = 0; i < segs; ++i) cosmetic_line(p, pen, &dash, pts[i], pts[(i + 1) % pts.size()], false, p.dc().s.rop2);
}

void fill_polygons(Painter& p, ArrayView<const Point> pts, ArrayView<const uint32_t> counts, bool winding, const PatternSource& src) {
  if (src.is_null || rect_empty(p.clip_bound())) return;
  List<Rect> region(*alloc());
  const Rect limit{p.clip_bound().left - p.ox(), p.clip_bound().top - p.oy(), p.clip_bound().right - p.ox(), p.clip_bound().bottom - p.oy()};
  if (!rgn_polygon(&region, pts, counts, winding, limit)) return;
  fill_region_pattern(p, region.view(), src, 0xF00021);
}

void flatten_bezier(Point p0, Point p1, Point p2, Point p3, List<Point>* out) {
  const double len = g_sqrt(double(p1.x - p0.x) * (p1.x - p0.x) + double(p1.y - p0.y) * (p1.y - p0.y)) + g_sqrt(double(p2.x - p1.x) * (p2.x - p1.x) + double(p2.y - p1.y) * (p2.y - p1.y)) +
                     g_sqrt(double(p3.x - p2.x) * (p3.x - p2.x) + double(p3.y - p2.y) * (p3.y - p2.y));
  int32_t n = int32_t(len / 3);
  if (n < 6) n = 6;
  if (n > 200) n = 200;
  for (int32_t i = 1; i <= n; ++i) {
    const double t = double(i) / n, u = 1 - t;
    const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    out->push_back(Point{round_i32(b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x), round_i32(b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y)});
  }
}

namespace {

constexpr uint32_t kMaxPoints = 1u << 20;
constexpr double kPi = 3.14159265358979323846;

bool read_points(Call& c, uint64_t address, uint32_t count, List<Point>* out) {
  if (count > kMaxPoints) return false;
  for (uint32_t i = 0; i < count; ++i) {
    Point p;
    if (!c.read_struct(address + uint64_t{i} * 8, &p)) return false;
    if (!out->push_back(p)) return false;
  }
  return true;
}

void to_device(const Dc& dc, List<Point>& pts) {
  for (Point& p : pts.view()) p = lp_to_dp(dc, p);
}

void path_add_figure(Dc& dc, ArrayView<const Point> pts, bool closed) {
  if (pts.size() == 0) return;
  path_move(dc, pts[0]);
  for (uint64_t i = 1; i < pts.size(); ++i) path_line(dc, pts[i]);
  if (closed) path_close(dc);
}

// ---- Lines and rectangles.

uint64_t NtGdiLineTo(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const Point from = lp_to_dp(*dc, dc->cur_pos), to = Point{c.argi(1), c.argi(2)};
  const Point dto = lp_to_dp(*dc, to);
  if (path_recording(*dc)) {
    path_line(*dc, dto);
    dc->cur_pos = to;
    return 1;
  }
  dc->cur_pos = to;
  Painter p(*dc);
  if (!p.ok()) return 1;
  draw_line_pen(p, from, dto, false);
  return 1;
}

void rect_outline_points(const Rect& r, Point out[4]) {
  out[0] = Point{r.left, r.top};
  out[1] = Point{r.right - 1, r.top};
  out[2] = Point{r.right - 1, r.bottom - 1};
  out[3] = Point{r.left, r.bottom - 1};
}

uint64_t NtGdiRectangle(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const Rect r = lp_to_dp_rect(*dc, Rect{c.argi(1), c.argi(2), c.argi(3), c.argi(4)});
  if (path_recording(*dc)) {
    const Point corners[4] = {{r.left, r.top}, {r.right, r.top}, {r.right, r.bottom}, {r.left, r.bottom}};
    path_add_figure(*dc, ArrayView<const Point>(corners, 4), true);
    return 1;
  }
  Painter p(*dc);
  if (!p.ok()) return 1;
  PenInfo pen;
  get_pen_info(*dc, &pen);
  PatternSource src;
  pattern_of_dc(*dc, &src);
  if (rect_empty(r)) return 1;
  Rect interior = r;
  if (!pen.null) { interior.left += 1; interior.top += 1; interior.right -= 1; interior.bottom -= 1; if (pen.width > 1) { const int32_t h = pen.width / 2; interior.left += h; interior.top += h; interior.right -= h; interior.bottom -= h; } }
  fill_rect_pattern(p, interior, src, 0xF00021);
  if (!pen.null) {
    Point q[4];
    Rect edge = r;
    if (pen.inside_frame && pen.width > 1) { const int32_t h = pen.width / 2; edge.left += h; edge.top += h; edge.right -= pen.width - h - 1 + 0; edge.bottom -= pen.width - h - 1 + 0; }
    rect_outline_points(edge, q);
    stroke_polyline(p, ArrayView<const Point>(q, 4), true);
  }
  return 1;
}

// Corner points of a rounded rectangle as a polygon (device coordinates, continuous geometry).
bool round_rect_points(const Dc& dc, const Rect& lr, int32_t ew, int32_t eh, List<Point>* out) {
  const Rect r = rect_normalized(lr);
  double w = double(r.right) - r.left, h = double(r.bottom) - r.top;
  double a = iabs(ew) / 2.0, b = iabs(eh) / 2.0;
  if (a * 2 > w) a = w / 2;
  if (b * 2 > h) b = h / 2;
  const int steps = 16;
  // explicit: top-right (angle -90..0), bottom-right (0..90), bottom-left (90..180), top-left (180..270)
  const double ccx[4] = {r.right - a, r.right - a, r.left + a, r.left + a}, ccy[4] = {r.top + b, r.bottom - b, r.bottom - b, r.top + b};
  for (int corner = 0; corner < 4; ++corner) {
    for (int i = 0; i <= steps; ++i) {
      const double ang = (-90.0 + corner * 90.0 + 90.0 * i / steps) * kPi / 180.0;
      const DPoint d = lp_to_dp_f(dc, ccx[corner] + a * g_cos(ang), ccy[corner] + b * g_sin(ang));
      if (!out->push_back(Point{round_i32(d.x), round_i32(d.y)})) return false;
    }
  }
  return true;
}

uint64_t NtGdiRoundRect(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const Rect lr{c.argi(1), c.argi(2), c.argi(3), c.argi(4)};
  List<Point> pts(*alloc());
  if (!round_rect_points(*dc, lr, c.argi(5), c.argi(6), &pts)) return 0;
  if (path_recording(*dc)) { path_add_figure(*dc, pts.view(), true); return 1; }
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_dc(*dc, &src);
  const uint32_t n = uint32_t(pts.count());
  if (!src.is_null && dc_is_axis_aligned(*dc) && !dc_world_active(*dc)) {
    const Rect dr = lp_to_dp_rect(*dc, lr);
    const Point a = lp_to_dp(*dc, Point{0, 0}), b = lp_to_dp(*dc, Point{iabs(c.argi(5)), iabs(c.argi(6))});
    List<Rect> region(*alloc());
    if (rgn_round_rect(&region, dr, iabs(b.x - a.x), iabs(b.y - a.y))) fill_region_pattern(p, region.view(), src, 0xF00021);
  } else {
    fill_polygons(p, pts.view(), ArrayView<const uint32_t>(&n, 1), dc->s.poly_fill == 2, src);
  }
  stroke_polyline(p, pts.view(), true);
  return 1;
}

// ---- Ellipses and arcs.

bool ellipse_points(const Dc& dc, const Rect& lr, double a0, double sweep, bool full, List<Point>* out) {
  const Rect r = rect_normalized(lr);
  const double cx = (double(r.left) + r.right) / 2, cy = (double(r.top) + r.bottom) / 2, rx = (double(r.right) - r.left) / 2, ry = (double(r.bottom) - r.top) / 2;
  const DPoint d0 = lp_to_dp_f(dc, r.left, r.top), d1 = lp_to_dp_f(dc, r.right, r.bottom);
  double big = (d1.x > d0.x ? d1.x - d0.x : d0.x - d1.x);
  const double bigy = d1.y > d0.y ? d1.y - d0.y : d0.y - d1.y;
  if (bigy > big) big = bigy;
  int32_t n = int32_t(big * 1.5);
  if (n < 24) n = 24;
  if (n > 1500) n = 1500;
  if (!full) { n = int32_t(n * (sweep < 0 ? -sweep : sweep) / (2 * kPi)) + 2; }
  for (int32_t i = 0; i <= n; ++i) {
    if (full && i == n) break;
    const double ang = a0 + (full ? 2 * kPi : sweep) * i / n;
    const double lx = cx + rx * g_cos(ang), ly = cy - ry * g_sin(ang);
    const DPoint d = lp_to_dp_f(dc, lx, ly);
    if (!out->push_back(Point{round_i32(d.x), round_i32(d.y)})) return false;
  }
  return true;
}

uint64_t NtGdiEllipse(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const Rect lr{c.argi(1), c.argi(2), c.argi(3), c.argi(4)};
  List<Point> pts(*alloc());
  if (path_recording(*dc)) {
    if (!ellipse_points(*dc, lr, 0, 0, true, &pts)) return 0;
    path_add_figure(*dc, pts.view(), true);
    return 1;
  }
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_dc(*dc, &src);
  if (dc_is_axis_aligned(*dc) && !dc_world_active(*dc)) {
    const Rect dr = lp_to_dp_rect(*dc, lr);
    List<Rect> region(*alloc());
    if (!src.is_null && rgn_ellipse(&region, dr)) fill_region_pattern(p, region.view(), src, 0xF00021);
    if (rect_empty(dr)) return 1;
    // the outline runs through the pixels of the inclusive bounds
    const double cx = (double(dr.left) + dr.right - 1) / 2, cy = (double(dr.top) + dr.bottom - 1) / 2, rx = (double(dr.right) - dr.left - 1) / 2, ry = (double(dr.bottom) - dr.top - 1) / 2;
    int32_t n = int32_t((rx > ry ? rx : ry) * 6);
    if (n < 24) n = 24;
    if (n > 4000) n = 4000;
    for (int32_t i = 0; i < n; ++i) {
      const double ang = 2 * kPi * i / n;
      if (!pts.push_back(Point{round_i32(cx + rx * g_cos(ang)), round_i32(cy + ry * g_sin(ang))})) return 0;
    }
    stroke_polyline(p, pts.view(), true);
  } else {
    if (!ellipse_points(*dc, lr, 0, 0, true, &pts)) return 0;
    const uint32_t n = uint32_t(pts.count());
    fill_polygons(p, pts.view(), ArrayView<const uint32_t>(&n, 1), dc->s.poly_fill == 2, src);
    stroke_polyline(p, pts.view(), true);
  }
  return 1;
}

double angle_of(const Rect& r, int32_t x, int32_t y) {
  const double cx = (double(r.left) + r.right) / 2, cy = (double(r.top) + r.bottom) / 2, rx = (double(r.right) - r.left) / 2, ry = (double(r.bottom) - r.top) / 2;
  const double dx = (double(x) - cx) / (rx == 0 ? 1 : rx), dy = (cy - double(y)) / (ry == 0 ? 1 : ry);
  return g_atan2(dy, dx);
}

uint64_t NtGdiArcInternal(Call& c) {
  const uint32_t type = c.arg32(0);
  Dc* dc = dc_arg(c, c.arg32(1));
  if (!dc || type > 3) return 0;
  const Rect lr = rect_normalized(Rect{c.argi(2), c.argi(3), c.argi(4), c.argi(5)});
  const int32_t xs = c.argi(6), ys = c.argi(7), xe = c.argi(8), ye = c.argi(9);
  if (rect_empty(lr)) return 1;
  const double a0 = angle_of(lr, xs, ys), a1 = angle_of(lr, xe, ye);
  double sweep;
  const bool ccw = dc->s.arc_dir == 1;
  const bool same = (xs == xe && ys == ye) || (a0 == a1);
  if (ccw) { sweep = a1 - a0; while (sweep <= 0) sweep += 2 * kPi; }
  else { sweep = a1 - a0; while (sweep >= 0) sweep -= 2 * kPi; }
  if (same) sweep = ccw ? 2 * kPi : -2 * kPi;
  List<Point> pts(*alloc());
  if (!ellipse_points(*dc, lr, a0, sweep, false, &pts)) return 0;
  const Point first = pts.count() ? pts[0] : Point(), last = pts.count() ? pts[pts.count() - 1] : Point();
  const double cxl = (double(lr.left) + lr.right) / 2, cyl = (double(lr.top) + lr.bottom) / 2;
  const DPoint cd = lp_to_dp_f(*dc, cxl, cyl);
  const Point center{round_i32(cd.x), round_i32(cd.y)};
  const Point cur = lp_to_dp(*dc, dc->cur_pos);
  if (path_recording(*dc)) {
    if (type == 1) path_line(*dc, first); else path_move(*dc, first);
    for (uint64_t i = 1; i < pts.count(); ++i) path_line(*dc, pts[i]);
    if (type == 3) { path_line(*dc, center); path_close(*dc); }
    if (type == 2) path_close(*dc);
    if (type == 1) { dc->cur_pos = dp_to_lp(*dc, last); }
    return 1;
  }
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_dc(*dc, &src);
  if (type == 2 || type == 3) {
    List<Point> poly(*alloc());
    for (const Point& q : pts.view()) poly.push_back(q);
    if (type == 3) poly.push_back(center);
    const uint32_t n = uint32_t(poly.count());
    fill_polygons(p, poly.view(), ArrayView<const uint32_t>(&n, 1), dc->s.poly_fill == 2, src);
    stroke_polyline(p, poly.view(), true);
  } else {
    if (type == 1) draw_line_pen(p, cur, first, false);
    stroke_polyline(p, pts.view(), false);
    if (type == 1) dc->cur_pos = dp_to_lp(*dc, last);
  }
  return 1;
}

uint64_t NtGdiAngleArc(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const int32_t x = c.argi(1), y = c.argi(2);
  const double radius = double(c.arg32(3)), start = double(__builtin_bit_cast(float, c.arg32(4))) * kPi / 180, sweep = double(__builtin_bit_cast(float, c.arg32(5))) * kPi / 180;
  const Rect lr{round_i32(x - radius), round_i32(y - radius), round_i32(x + radius), round_i32(y + radius)};
  List<Point> pts(*alloc());
  if (!ellipse_points(*dc, lr, start, sweep, false, &pts) || pts.count() == 0) return 0;
  const Point cur = lp_to_dp(*dc, dc->cur_pos);
  if (path_recording(*dc)) {
    path_line(*dc, pts[0]);
    for (uint64_t i = 1; i < pts.count(); ++i) path_line(*dc, pts[i]);
  } else {
    Painter p(*dc);
    if (p.ok()) {
      draw_line_pen(p, cur, pts[0], false);
      stroke_polyline(p, pts.view(), false);
    }
  }
  dc->cur_pos = dp_to_lp(*dc, pts[pts.count() - 1]);
  return 1;
}

// ---- Polygons, polylines, curves.

enum : uint32_t { kPolyPolygon = 1, kPolyPolyline, kPolyBezier, kPolyBezierTo, kPolylineTo, kPolyPolygonRgn };

uint64_t NtGdiPolyPolyDraw(Call& c) {
  const uint32_t func = c.arg32(4), count = c.arg32(3);
  if (count == 0 || count > kMaxPoints || func < 1 || func > 6) return c.fail(kErrorInvalidParameter);
  List<uint32_t> counts(*alloc());
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t n = 0;
    if (!c.read_u32(c.arg(2) + uint64_t{i} * 4, &n)) return c.fail(kErrorInvalidParameter);
    total += n;
    if (total > kMaxPoints) return c.fail(kErrorInvalidParameter);
    counts.push_back(n);
  }
  List<Point> pts(*alloc());
  if (!read_points(c, c.arg(1), uint32_t(total), &pts)) return c.fail(kErrorInvalidParameter);
  Dc* dc = func == kPolyPolygonRgn ? nullptr : dc_arg(c, c.arg32(0));
  if (func != kPolyPolygonRgn && !dc) return 0;
  if (func == kPolyPolygonRgn) {
    List<Point> dev(*alloc());
    for (const Point& q : pts.view()) dev.push_back(q);      // region polygons are in device units
    List<Rect> region(*alloc());
    if (!rgn_polygon(&region, dev.view(), counts.view(), c.arg32(0) == 2, Rect{-65536, -65536, 65536, 65536})) return 0;
    Region* r = new_region(c.pid(), region.view());
    return r ? r->handle : 0;
  }
  to_device(*dc, pts);
  uint64_t base = 0;
  if (func == kPolyPolygon) {
    for (uint32_t n : counts.view()) if (n < 2) return c.fail(kErrorInvalidParameter);
    if (path_recording(*dc)) {
      for (uint32_t n : counts.view()) { path_add_figure(*dc, pts.view().subview(base, n), true); base += n; }
      return 1;
    }
    Painter p(*dc);
    if (!p.ok()) return 1;
    PatternSource src;
    pattern_of_dc(*dc, &src);
    fill_polygons(p, pts.view(), counts.view(), dc->s.poly_fill == 2, src);
    for (uint32_t n : counts.view()) { stroke_polyline(p, pts.view().subview(base, n), true); base += n; }
    return 1;
  }
  if (func == kPolyPolyline) {
    for (uint32_t n : counts.view()) if (n < 2) return c.fail(kErrorInvalidParameter);
    if (path_recording(*dc)) {
      for (uint32_t n : counts.view()) { path_add_figure(*dc, pts.view().subview(base, n), false); base += n; }
      return 1;
    }
    Painter p(*dc);
    if (!p.ok()) return 1;
    for (uint32_t n : counts.view()) { stroke_polyline(p, pts.view().subview(base, n), false); base += n; }
    return 1;
  }
  const uint32_t n = counts[0];
  if (func == kPolylineTo) {
    if (n < 1) return c.fail(kErrorInvalidParameter);
    List<Point> line(*alloc());
    line.push_back(lp_to_dp(*dc, dc->cur_pos));
    for (const Point& q : pts.view()) line.push_back(q);
    if (path_recording(*dc)) { for (const Point& q : pts.view()) path_line(*dc, q); }
    else { Painter p(*dc); if (p.ok()) stroke_polyline(p, line.view(), false); }
    dc->cur_pos = dp_to_lp(*dc, pts[n - 1]);
    return 1;
  }
  // beziers
  const uint32_t need_mod = n % 3;
  if (func == kPolyBezier ? (n < 4 || need_mod != 1) : (n < 3 || need_mod != 0)) return c.fail(kErrorInvalidParameter);
  List<Point> curve(*alloc());
  uint64_t at = 0;
  Point start = func == kPolyBezier ? pts[at++] : lp_to_dp(*dc, dc->cur_pos);
  const Point curve_start = start;
  if (!curve.push_back(start)) return 0;
  const bool recording = path_recording(*dc);
  if (recording && func == kPolyBezier) path_move(*dc, start);
  for (; at + 2 < n; at += 3) {
    if (recording) path_bezier(*dc, pts[at], pts[at + 1], pts[at + 2]);
    flatten_bezier(start, pts[at], pts[at + 1], pts[at + 2], &curve);
    start = pts[at + 2];
  }
  (void)curve_start;
  if (!recording) { Painter p(*dc); if (p.ok()) stroke_polyline(p, curve.view(), false); }
  dc->cur_pos = dp_to_lp(*dc, pts[n - 1]);
  return 1;
}

uint64_t NtGdiPolyDraw(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t count = c.arg32(3);
  if (!dc || count == 0 || count > kMaxPoints) return 0;
  List<Point> pts(*alloc());
  if (!read_points(c, c.arg(1), count, &pts)) return 0;
  List<uint8_t> types(*alloc());
  for (uint32_t i = 0; i < count; ++i) { uint8_t t = 0; if (!c.read_u8(c.arg(2) + i, &t)) return 0; types.push_back(t); }
  to_device(*dc, pts);
  const bool recording = path_recording(*dc);
  List<Point> run(*alloc());
  Point cur = lp_to_dp(*dc, dc->cur_pos);
  Point figure_start = cur;
  Painter* painter = nullptr;
  UniquePointer<Painter> held;
  if (!recording) { held = UniquePointer<Painter>::make(*alloc(), *dc); painter = held.get(); if (!painter || !painter->ok()) painter = nullptr; }
  auto flush = [&]() { if (!recording && painter && run.count() > 1) stroke_polyline(*painter, run.view(), false); run.clear(); };
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t t = types[i] & ~kPtCloseFigure;
    const bool close = (types[i] & kPtCloseFigure) != 0;
    if (t == kPtMoveTo) {
      flush();
      cur = pts[i];
      figure_start = cur;
      run.push_back(cur);
      if (recording) path_move(*dc, cur);
    } else if (t == kPtLineTo) {
      if (run.count() == 0) run.push_back(cur);
      run.push_back(pts[i]);
      if (recording) path_line(*dc, pts[i]);
      cur = pts[i];
    } else if (t == kPtBezierTo) {
      if (i + 2 >= count || (types[i + 1] & 0x7E) != kPtBezierTo || (types[i + 2] & 0x7E) != kPtBezierTo) return 0;
      if (run.count() == 0) run.push_back(cur);
      flatten_bezier(cur, pts[i], pts[i + 1], pts[i + 2], &run);
      if (recording) path_bezier(*dc, pts[i], pts[i + 1], pts[i + 2]);
      cur = pts[i + 2];
      i += 2;
      if (types[i] & kPtCloseFigure) { run.push_back(figure_start); if (recording) path_close(*dc); cur = figure_start; flush(); }
      continue;
    } else {
      return 0;
    }
    if (close) {
      run.push_back(figure_start);
      if (recording) path_close(*dc);
      cur = figure_start;
      flush();
    }
  }
  flush();
  dc->cur_pos = dp_to_lp(*dc, cur);
  return 1;
}

// ---- Pixels.

uint64_t NtGdiSetPixel(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return kClrInvalid;
  Painter p(*dc);
  if (!p.ok()) return kClrInvalid;
  const Point d = lp_to_dp(*dc, Point{c.argi(1), c.argi(2)});
  if (!p.visible(d.x, d.y)) return kClrInvalid;
  p.plot_raw(d.x, d.y, resolve_color(*dc, c.arg32(3)));
  return p.read(d.x, d.y);
}

uint64_t NtGdiGetPixel(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return kClrInvalid;
  Painter p(*dc);
  if (!p.ok()) return kClrInvalid;
  const Point d = lp_to_dp(*dc, Point{c.argi(1), c.argi(2)});
  return p.read(d.x, d.y);
}

uint64_t NtGdiExtFloodFill(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  Painter p(*dc);
  if (!p.ok()) return 0;
  const Point start = lp_to_dp(*dc, Point{c.argi(1), c.argi(2)});
  const ColorRef target = resolve_color(*dc, c.arg32(3)) & 0xFFFFFF;
  const bool surface_mode = c.arg32(4) == 1;
  const Rect box{p.clip_bound().left - p.ox(), p.clip_bound().top - p.oy(), p.clip_bound().right - p.ox(), p.clip_bound().bottom - p.oy()};
  if (!rect_has(box, start.x, start.y)) return 0;
  const uint64_t w = uint32_t(box.right - box.left), h = uint32_t(box.bottom - box.top);
  if (w * h > (64u << 20)) return 0;
  const ColorRef at_start = p.read(start.x, start.y) & 0xFFFFFF;
  if (surface_mode ? at_start != target : at_start == target) return 0;
  PatternSource src;
  pattern_of_dc(*dc, &src);
  if (src.is_null) return 1;
  ByteBuffer seen(*alloc());
  if (!seen.put_zeros((w * h + 7) / 8)) return 0;
  ByteView bits = seen.writable_view();
  auto fillable = [&](int32_t x, int32_t y) {
    if (!rect_has(box, x, y)) return false;
    const uint64_t i = uint64_t(y - box.top) * w + uint32_t(x - box.left);
    if (bits[i / 8] >> (i % 8) & 1) return false;
    const ColorRef col = p.read(x, y) & 0xFFFFFF;
    return surface_mode ? col == target : col != target;
  };
  auto mark = [&](int32_t x, int32_t y) { const uint64_t i = uint64_t(y - box.top) * w + uint32_t(x - box.left); bits[i / 8] = uint8_t(bits[i / 8] | (1u << (i % 8))); };
  List<Point> stack(*alloc());
  stack.push_back(start);
  uint64_t guard = 0;
  while (stack.count() > 0 && ++guard < (1u << 26)) {
    const Point s = stack[stack.count() - 1];
    stack.remove_at(stack.count() - 1);
    if (!fillable(s.x, s.y)) continue;
    int32_t l = s.x, r = s.x;
    while (fillable(l - 1, s.y)) --l;
    while (fillable(r + 1, s.y)) ++r;
    for (int32_t x = l; x <= r; ++x) mark(x, s.y);
    for (int32_t dy = -1; dy <= 1; dy += 2) {
      bool in_run = false;
      for (int32_t x = l; x <= r; ++x) {
        const bool f = fillable(x, s.y + dy);
        if (f && !in_run) stack.push_back(Point{x, s.y + dy});
        in_run = f;
      }
    }
    // paint after scanning (the scan reads the unpainted surface)
    for (int32_t x = l; x <= r; ++x) {
      bool transparent = false;
      const ColorRef col = src.at(x, s.y, &transparent);
      if (!transparent) p.plot_raw(x, s.y, col);
    }
  }
  return 1;
}

// ---- Regions.

uint64_t NtGdiFillRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Region* r = find_region(c.arg32(1));
  Brush* b = find_brush(c.arg32(2));
  if (!dc || !r || !b) return 0;
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_brush(*dc, b, &src);
  fill_region_pattern(p, r->rects.view(), src, 0xF00021);
  return 1;
}

uint64_t NtGdiFrameRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Region* r = find_region(c.arg32(1));
  Brush* b = find_brush(c.arg32(2));
  if (!dc || !r || !b) return 0;
  const Point z = lp_to_dp(*dc, Point{0, 0}), s = lp_to_dp(*dc, Point{c.argi(3), c.argi(4)});
  const int32_t w = iabs(sat_sub(s.x, z.x)), h = iabs(sat_sub(s.y, z.y));
  allocators::IAllocator& al = *alloc();
  List<Rect> inner(al), t1(al), t2(al), shifted(al), frame(al);
  if (!rgn_assign(inner, r->rects.view())) return 0;
  const int32_t dx[4] = {w, -w, 0, 0}, dy[4] = {0, 0, h, -h};
  for (int i = 0; i < 4; ++i) {
    if (!rgn_assign(shifted, r->rects.view())) return 0;
    rgn_offset(shifted, dx[i], dy[i]);
    if (!rgn_combine(inner.view(), shifted.view(), 1, &t1) || !rgn_assign(inner, t1.view())) return 0;
  }
  if (!rgn_combine(r->rects.view(), inner.view(), 4, &frame)) return 0;
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_brush(*dc, b, &src);
  fill_region_pattern(p, frame.view(), src, 0xF00021);
  return 1;
}

uint64_t NtGdiInvertRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Region* r = find_region(c.arg32(1));
  if (!dc || !r) return 0;
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_dc(*dc, &src);
  for (const Rect& e : r->rects.view()) fill_rect_pattern(p, e, src, 0x550009);
  return 1;
}

}  // namespace

void register_draw_services() {
  GDI_REGISTER(LineTo, NtGdiLineTo);
  GDI_REGISTER(Rectangle, NtGdiRectangle);
  GDI_REGISTER(RoundRect, NtGdiRoundRect);
  GDI_REGISTER(Ellipse, NtGdiEllipse);
  GDI_REGISTER(ArcInternal, NtGdiArcInternal);
  GDI_REGISTER(AngleArc, NtGdiAngleArc);
  GDI_REGISTER(PolyPolyDraw, NtGdiPolyPolyDraw);
  GDI_REGISTER(PolyDraw, NtGdiPolyDraw);
  GDI_REGISTER(SetPixel, NtGdiSetPixel);
  GDI_REGISTER(GetPixel, NtGdiGetPixel);
  GDI_REGISTER(ExtFloodFill, NtGdiExtFloodFill);
  GDI_REGISTER(FillRgn, NtGdiFillRgn);
  GDI_REGISTER(FrameRgn, NtGdiFrameRgn);
  GDI_REGISTER(InvertRgn, NtGdiInvertRgn);
}

}  // namespace process::win32k::gdi
