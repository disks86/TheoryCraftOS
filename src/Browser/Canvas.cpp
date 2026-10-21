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

#include "Browser/Canvas.hpp"
#include "Browser/Css.hpp"
#include "Dom/Entities.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Math/FloatMath.hpp"

namespace browser {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;
constexpr uint64_t kMaxSpans = 6u << 20;

double dabs(double v) { return v < 0 ? -v : v; }
double dsqrt(double v) { return v > 0 ? fmath::sqrt(v) : 0; }
double dfloor(double v) {
  if (v > 4e18 || v < -4e18) return v;
  const int64_t i = static_cast<int64_t>(v);
  return static_cast<double>(i) > v ? static_cast<double>(i - 1) : static_cast<double>(i);
}
double dceil(double v) {
  const double f = dfloor(v);
  return f < v ? f + 1 : f;
}
int32_t clamp_i32(double v) { return v < -2e9 ? -2000000000 : v > 2e9 ? 2000000000 : static_cast<int32_t>(v); }
int32_t round_i32(double v) { return clamp_i32(dfloor(v + 0.5)); }
// The pixel whose centre is the first one at or after v (pixel i has its centre at i + 0.5).
int32_t first_pixel(double v) { return clamp_i32(dceil(v - 0.5)); }

double atan_small(double x) {   // |x| <= 0.2
  const double x2 = x * x;
  return x * (1 - x2 * (1.0 / 3 - x2 * (1.0 / 5 - x2 * (1.0 / 7 - x2 * (1.0 / 9 - x2 * (1.0 / 11 - x2 * (1.0 / 13)))))));
}
double datan(double x) {
  const bool negative = x < 0;
  if (negative) x = -x;
  bool inverted = false;
  if (x > 1) {
    x = 1 / x;
    inverted = true;
  }
  x = x / (1 + dsqrt(1 + x * x));
  x = x / (1 + dsqrt(1 + x * x));
  double r = 4 * atan_small(x);
  if (inverted) r = kPi / 2 - r;
  return negative ? -r : r;
}
double datan2(double y, double x) {
  if (x == 0) return y > 0 ? kPi / 2 : y < 0 ? -kPi / 2 : 0;
  const double a = datan(y / x);
  if (x > 0) return a;
  return y >= 0 ? a + kPi : a - kPi;
}
double dmod(double v, double m) { return v - m * dfloor(v / m); }

uint32_t mul_div_255(uint32_t v, uint32_t a) { return (v * a + 127) / 255; }

}  // namespace

double canvas_atan2(double y, double x) { return datan2(y, x); }


// --- Path -------------------------------------------------------------------------------------------------------------------------

void Path::clear() {
  verbs_.clear();
  points_.clear();
  has_current_ = false;
  after_close_ = false;
  failed_ = false;
}

void Path::add_point(Verb verb, double x, double y) {
  double tx, ty;
  transform_point(transform_, x, y, &tx, &ty);
  failed_ |= !verbs_.push_back(verb) || !points_.push_back(CanvasPoint{tx, ty});
}

void Path::add_user(Verb verb, double x, double y) {
  add_point(verb, x, y);
  cur_x_ = x;
  cur_y_ = y;
}

void Path::move_to(double x, double y) {
  if (x != x || y != y) return;
  add_user(Verb::Move, x, y);
  start_x_ = x;
  start_y_ = y;
  has_current_ = true;
  after_close_ = false;
}

bool Path::ensure_start(double x, double y) {
  if (has_current_) return false;
  move_to(x, y);
  return true;
}

void Path::line_to(double x, double y) {
  if (x != x || y != y) return;
  if (ensure_start(x, y)) return;
  restart_after_close();
  add_user(Verb::Line, x, y);
}

void Path::restart_after_close() {
  if (!after_close_) return;
  after_close_ = false;
  add_point(Verb::Move, start_x_, start_y_);
}

void Path::bezier_to(double c1x, double c1y, double c2x, double c2y, double x, double y) {
  if (c1x != c1x || c1y != c1y || c2x != c2x || c2y != c2y || x != x || y != y) return;
  ensure_start(c1x, c1y);
  restart_after_close();
  add_point(Verb::Cubic, c1x, c1y);
  double tx, ty;
  transform_point(transform_, c2x, c2y, &tx, &ty);
  failed_ |= !points_.push_back(CanvasPoint{tx, ty});
  transform_point(transform_, x, y, &tx, &ty);
  failed_ |= !points_.push_back(CanvasPoint{tx, ty});
  cur_x_ = x;
  cur_y_ = y;
}

void Path::quad_to(double cx, double cy, double x, double y) {
  if (cx != cx || cy != cy || x != x || y != y) return;
  ensure_start(cx, cy);
  const double x0 = cur_x_, y0 = cur_y_;
  bezier_to(x0 + 2.0 / 3 * (cx - x0), y0 + 2.0 / 3 * (cy - y0), x + 2.0 / 3 * (cx - x), y + 2.0 / 3 * (cy - y), x, y);
}

void Path::close() {
  if (!has_current_ || verbs_.count() == 0) return;
  failed_ |= !verbs_.push_back(Verb::Close);
  cur_x_ = start_x_;
  cur_y_ = start_y_;
  after_close_ = true;   // what follows starts a new subpath where this one started
}

namespace {

// Appends the cubics of an elliptical arc from `start` over `sweep` radians, the current point being at its start.
void arc_segments(Path& p, double cx, double cy, double rx, double ry, double rotation, double start, double sweep) {
  if (sweep == 0) return;
  double cr = 1, sr = 0;
  if (rotation != 0) {
    cr = fmath::cos(rotation);
    sr = fmath::sin(rotation);
  }
  const double abs_sweep = dabs(sweep);
  uint32_t n = static_cast<uint32_t>(dceil(abs_sweep / (kPi / 2) - 1e-9));
  if (n < 1) n = 1;
  const double step = sweep / n;
  const double k = 4.0 / 3.0 * fmath::tan(step / 4);
  auto point = [&](double t, double* px, double* py, double* dx, double* dy) {
    const double c = fmath::cos(t), s = fmath::sin(t);
    const double ex = rx * c, ey = ry * s;
    *px = cx + ex * cr - ey * sr;
    *py = cy + ex * sr + ey * cr;
    const double tx = -rx * s, ty = ry * c;
    *dx = tx * cr - ty * sr;
    *dy = tx * sr + ty * cr;
  };
  double t0 = start, x0, y0, dx0, dy0;
  point(t0, &x0, &y0, &dx0, &dy0);
  for (uint32_t i = 0; i < n; ++i) {
    const double t1 = start + step * (i + 1);
    double x1, y1, dx1, dy1;
    point(t1, &x1, &y1, &dx1, &dy1);
    p.bezier_to(x0 + k * dx0, y0 + k * dy0, x1 - k * dx1, y1 - k * dy1, x1, y1);
    t0 = t1;
    x0 = x1;
    y0 = y1;
    dx0 = dx1;
    dy0 = dy1;
  }
}

double normalized_sweep(double start, double end, bool ccw) {
  double sweep = end - start;
  if (!ccw) {
    if (sweep >= kTwoPi) return kTwoPi;
    return dmod(sweep, kTwoPi);
  }
  if (sweep <= -kTwoPi) return -kTwoPi;
  sweep = dmod(sweep, kTwoPi);
  return sweep > 0 ? sweep - kTwoPi : sweep;
}

}  // namespace

bool Path::ellipse(double x, double y, double rx, double ry, double rotation, double start, double end, bool ccw) {
  if (x != x || y != y || rx != rx || ry != ry || start != start || end != end || rotation != rotation) return true;
  if (rx < 0 || ry < 0) return false;
  const double sweep = normalized_sweep(start, end, ccw);
  const double cr = fmath::cos(rotation), sr = fmath::sin(rotation);
  const double ex = rx * fmath::cos(start), ey = ry * fmath::sin(start);
  const double px = x + ex * cr - ey * sr, py = y + ex * sr + ey * cr;
  if (has_current_) line_to(px, py);
  else move_to(px, py);
  arc_segments(*this, x, y, rx, ry, rotation, start, sweep);
  return true;
}

bool Path::arc(double x, double y, double radius, double start, double end, bool ccw) { return ellipse(x, y, radius, radius, 0, start, end, ccw); }

bool Path::arc_to(double x1, double y1, double x2, double y2, double radius) {
  if (x1 != x1 || y1 != y1 || x2 != x2 || y2 != y2 || radius != radius) return true;
  if (radius < 0) return false;
  if (ensure_start(x1, y1)) return true;
  const double x0 = cur_x_, y0 = cur_y_;
  const double v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
  const double l1 = dsqrt(v1x * v1x + v1y * v1y), l2 = dsqrt(v2x * v2x + v2y * v2y);
  if (l1 == 0 || l2 == 0 || radius == 0) {
    line_to(x1, y1);
    return true;
  }
  const double u1x = v1x / l1, u1y = v1y / l1, u2x = v2x / l2, u2y = v2y / l2;
  const double cross = u1x * u2y - u1y * u2x, dot = u1x * u2x + u1y * u2y;
  if (dabs(cross) < 1e-12) {   // in a line
    line_to(x1, y1);
    return true;
  }
  const double tan_half = dabs(cross) / (1 + dot);
  const double d = radius / tan_half;
  const double t1x = x1 + u1x * d, t1y = y1 + u1y * d;
  line_to(t1x, t1y);
  double bx = u1x + u2x, by = u1y + u2y;
  const double bl = dsqrt(bx * bx + by * by);
  bx /= bl;
  by /= bl;
  const double dist = radius / dsqrt((1 - dot) / 2);
  const double cx = x1 + bx * dist, cy = y1 + by * dist;
  const double t2x = x1 + u2x * d, t2y = y1 + u2y * d;
  const double ax = t1x - cx, ay = t1y - cy, bx2 = t2x - cx, by2 = t2y - cy;
  const double acr = ax * by2 - ay * bx2, adot = ax * bx2 + ay * by2;
  double sweep = datan2(dabs(acr), adot);
  if (acr < 0) sweep = -sweep;
  arc_segments(*this, cx, cy, radius, radius, 0, datan2(ay, ax), sweep);
  return true;
}

void Path::rect(double x, double y, double w, double h) {
  if (x != x || y != y || w != w || h != h) return;
  move_to(x, y);
  line_to(x + w, y);
  line_to(x + w, y + h);
  line_to(x, y + h);
  close();
}

void Path::round_rect(double x, double y, double w, double h, double radius) {
  if (x != x || y != y || w != w || h != h || radius != radius) return;
  if (w < 0) {
    x += w;
    w = -w;
  }
  if (h < 0) {
    y += h;
    h = -h;
  }
  double r = radius < 0 ? 0 : radius;
  const double limit = (w < h ? w : h) / 2;
  if (r > limit) r = limit;
  if (r <= 0) {
    rect(x, y, w, h);
    return;
  }
  move_to(x + r, y);
  line_to(x + w - r, y);
  arc_segments(*this, x + w - r, y + r, r, r, 0, -kPi / 2, kPi / 2);
  line_to(x + w, y + h - r);
  arc_segments(*this, x + w - r, y + h - r, r, r, 0, 0, kPi / 2);
  line_to(x + r, y + h);
  arc_segments(*this, x + r, y + h - r, r, r, 0, kPi / 2, kPi / 2);
  line_to(x, y + r);
  arc_segments(*this, x + r, y + r, r, r, 0, kPi, kPi / 2);
  close();
}

void Path::add_path(const Path& other, const Transform& m) {
  uint64_t p = 0;
  for (uint64_t i = 0; i < other.verbs_.count(); ++i) {
    const Verb verb = other.verbs_[i];
    const uint32_t n = verb == Verb::Cubic ? 3 : verb == Verb::Close ? 0 : 1;
    failed_ |= !verbs_.push_back(verb);
    for (uint32_t k = 0; k < n; ++k, ++p) {
      double tx, ty;
      transform_point(m, other.points_[p].x, other.points_[p].y, &tx, &ty);
      failed_ |= !points_.push_back(CanvasPoint{tx, ty});
      cur_x_ = other.points_[p].x;
      cur_y_ = other.points_[p].y;
    }
  }
  if (other.has_current_) {
    has_current_ = true;
    start_x_ = other.start_x_;
    start_y_ = other.start_y_;
  }
}

bool Path::flatten(const Transform& m, List<CanvasPoint>* points, List<Sub>* subs) const {
  points->clear();
  subs->clear();
  uint64_t p = 0;
  bool open = false;
  Sub sub;
  CanvasPoint last;
  auto finish = [&]() -> bool {
    if (!open) return true;
    sub.count = static_cast<uint32_t>(points->count()) - sub.first;
    open = false;
    return subs->push_back(sub);
  };
  for (uint64_t i = 0; i < verbs_.count(); ++i) {
    switch (verbs_[i]) {
      case Verb::Move: {
        if (!finish()) return false;
        double x, y;
        transform_point(m, points_[p].x, points_[p].y, &x, &y);
        ++p;
        sub = Sub();
        sub.first = static_cast<uint32_t>(points->count());
        open = true;
        last = CanvasPoint{x, y};
        if (!points->push_back(last)) return false;
        break;
      }
      case Verb::Line: {
        double x, y;
        transform_point(m, points_[p].x, points_[p].y, &x, &y);
        ++p;
        if (!open) {   // (a line without a start: cannot happen from the builders)
          sub = Sub();
          sub.first = static_cast<uint32_t>(points->count());
          open = true;
        }
        last = CanvasPoint{x, y};
        if (!points->push_back(last)) return false;
        break;
      }
      case Verb::Cubic: {
        double x1, y1, x2, y2, x3, y3;
        transform_point(m, points_[p].x, points_[p].y, &x1, &y1);
        transform_point(m, points_[p + 1].x, points_[p + 1].y, &x2, &y2);
        transform_point(m, points_[p + 2].x, points_[p + 2].y, &x3, &y3);
        p += 3;
        if (!open) {
          sub = Sub();
          sub.first = static_cast<uint32_t>(points->count());
          open = true;
          last = CanvasPoint{x1, y1};
          if (!points->push_back(last)) return false;
        }
        const double len = dsqrt((x1 - last.x) * (x1 - last.x) + (y1 - last.y) * (y1 - last.y)) + dsqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)) +
                           dsqrt((x3 - x2) * (x3 - x2) + (y3 - y2) * (y3 - y2));
        int32_t n = static_cast<int32_t>(len / 3) + 3;
        if (n > 160) n = 160;
        for (int32_t k = 1; k <= n; ++k) {
          const double t = static_cast<double>(k) / n, u = 1 - t;
          const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
          const CanvasPoint q{b0 * last.x + b1 * x1 + b2 * x2 + b3 * x3, b0 * last.y + b1 * y1 + b2 * y2 + b3 * y3};
          if (k == n) {
            if (!points->push_back(CanvasPoint{x3, y3})) return false;
          } else if (!points->push_back(q)) {
            return false;
          }
        }
        // (the curve's polynomial above used `last` as the start of this segment; keep the end for the next one)
        last = CanvasPoint{x3, y3};
        break;
      }
      case Verb::Close:
        if (open) sub.closed = true;
        if (!finish()) return false;
        break;
    }
  }
  return finish();
}

// --- Composite operations and gradients ---------------------------------------------------------------------------------------------

namespace {
struct CompositeName {
  const char* name;
  Composite op;
};
const CompositeName kComposites[] = {
    {"source-over", Composite::SourceOver}, {"source-in", Composite::SourceIn}, {"source-out", Composite::SourceOut}, {"source-atop", Composite::SourceAtop},
    {"destination-over", Composite::DestinationOver}, {"destination-in", Composite::DestinationIn}, {"destination-out", Composite::DestinationOut},
    {"destination-atop", Composite::DestinationAtop}, {"lighter", Composite::Lighter}, {"copy", Composite::Copy}, {"xor", Composite::Xor},
    {"multiply", Composite::Multiply}, {"screen", Composite::Screen}, {"overlay", Composite::Overlay}, {"darken", Composite::Darken},
    {"lighten", Composite::Lighten}, {"difference", Composite::Difference}, {"exclusion", Composite::Exclusion},
};
}  // namespace

bool parse_composite(StringView name, Composite* out) {
  for (const CompositeName& c : kComposites)
    if (name.equals(c.name)) {
      *out = c.op;
      return true;
    }
  return false;
}

const char* composite_name(Composite op) {
  for (const CompositeName& c : kComposites)
    if (c.op == op) return c.name;
  return "source-over";
}

void CanvasPaint::add_stop(double offset, uint32_t color_value) {
  if (stop_count >= kMaxStops) return;
  GradientStop s;
  s.offset = static_cast<float>(offset < 0 ? 0 : offset > 1 ? 1 : offset);
  s.color = color_value;
  uint32_t at = stop_count;
  while (at > 0 && stops[at - 1].offset > s.offset) {   // (stops with the same offset keep the order they came in)
    stops[at] = stops[at - 1];
    --at;
  }
  stops[at] = s;
  ++stop_count;
}

namespace {

uint32_t gradient_color(const CanvasPaint& p, double t) {
  if (p.stop_count == 0) return 0;
  if (t <= p.stops[0].offset) return p.stops[0].color;
  if (t >= p.stops[p.stop_count - 1].offset) return p.stops[p.stop_count - 1].color;
  for (uint32_t i = 1; i < p.stop_count; ++i) {
    if (t > p.stops[i].offset) continue;
    const GradientStop& a = p.stops[i - 1];
    const GradientStop& b = p.stops[i];
    const double span = static_cast<double>(b.offset) - a.offset;
    const double f = span <= 0 ? 1 : (t - a.offset) / span;
    uint32_t out = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
      const double ca = (a.color >> shift) & 0xFF, cb = (b.color >> shift) & 0xFF;
      const double v = ca + (cb - ca) * f;
      out |= static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v + 0.5) << shift;
    }
    return out;
  }
  return p.stops[p.stop_count - 1].color;
}

}  // namespace

// --- Scan conversion ----------------------------------------------------------------------------------------------------------------

namespace {

struct Edge {
  double x0, y0, x1, y1;
  int32_t dir;
};
struct Crossing {
  double x;
  int32_t dir;
};

void sift_down(ArrayView<Edge> e, uint64_t start, uint64_t end) {
  uint64_t root = start;
  while (root * 2 + 1 < end) {
    uint64_t child = root * 2 + 1;
    if (child + 1 < end && e[child].y0 < e[child + 1].y0) ++child;
    if (e[root].y0 >= e[child].y0) return;
    const Edge t = e[root];
    e[root] = e[child];
    e[child] = t;
    root = child;
  }
}

void sort_edges(ArrayView<Edge> e) {
  const uint64_t n = e.size();
  if (n < 2) return;
  for (uint64_t start = n / 2; start > 0; --start) sift_down(e, start - 1, n);
  for (uint64_t end = n - 1; end > 0; --end) {
    const Edge t = e[0];
    e[0] = e[end];
    e[end] = t;
    sift_down(e, 0, end);
  }
}

// The pixels (centres inside) of the polygons, as horizontal spans in the clip.
bool scan_fill(allocators::IAllocator& a, const List<CanvasPoint>& pts, const List<Path::Sub>& subs, bool even_odd, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1,
               List<CanvasSpan>* out) {
  if (cx1 <= cx0 || cy1 <= cy0) return true;
  List<Edge> edges(a);
  double min_y = 1e300, max_y = -1e300;
  for (uint64_t s = 0; s < subs.count(); ++s) {
    const Path::Sub& sub = subs[s];
    if (sub.count < 3) continue;
    for (uint32_t i = 0; i < sub.count; ++i) {
      const CanvasPoint& p = pts[sub.first + i];
      const CanvasPoint& q = pts[sub.first + (i + 1) % sub.count];
      if (p.y == q.y || p.x != p.x || p.y != p.y || q.x != q.x || q.y != q.y) continue;
      Edge e;
      if (p.y < q.y) e = Edge{p.x, p.y, q.x, q.y, 1};
      else e = Edge{q.x, q.y, p.x, p.y, -1};
      if (e.y1 <= cy0 || e.y0 >= cy1) continue;
      if (e.y0 < min_y) min_y = e.y0;
      if (e.y1 > max_y) max_y = e.y1;
      if (!edges.push_back(e)) return false;
    }
  }
  if (edges.count() == 0) return true;
  sort_edges(edges.view());
  int32_t y_start = first_pixel(min_y), y_end = first_pixel(max_y);
  if (y_start < cy0) y_start = cy0;
  if (y_end > cy1) y_end = cy1;
  List<uint32_t> active(a);
  List<Crossing> xs(a);
  uint64_t next = 0;
  for (int32_t y = y_start; y < y_end; ++y) {
    const double ys = y + 0.5;
    while (next < edges.count() && edges[next].y0 <= ys) {
      if (!active.push_back(static_cast<uint32_t>(next))) return false;
      ++next;
    }
    xs.clear();
    uint64_t keep = 0;
    for (uint64_t k = 0; k < active.count(); ++k) {
      const Edge& e = edges[active[k]];
      if (e.y1 <= ys) continue;
      active[keep++] = active[k];
      if (!xs.push_back(Crossing{e.x0 + (ys - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir})) return false;
    }
    while (active.count() > keep) active.remove_at(active.count() - 1);
    for (uint64_t i = 1; i < xs.count(); ++i) {   // (insertion sort: a row rarely has more than a few crossings)
      const Crossing c = xs[i];
      uint64_t j = i;
      while (j > 0 && xs[j - 1].x > c.x) {
        xs[j] = xs[j - 1];
        --j;
      }
      xs[j] = c;
    }
    int32_t wind = 0;
    double left = 0;
    for (uint64_t i = 0; i < xs.count(); ++i) {
      const bool was = even_odd ? (wind & 1) != 0 : wind != 0;
      wind = even_odd ? wind ^ 1 : wind + xs[i].dir;
      const bool now = even_odd ? (wind & 1) != 0 : wind != 0;
      if (!was && now) {
        left = xs[i].x;
      } else if (was && !now) {
        int32_t x0 = first_pixel(left), x1 = first_pixel(xs[i].x);
        if (x0 < cx0) x0 = cx0;
        if (x1 > cx1) x1 = cx1;
        if (x1 > x0) {
          if (out->count() >= kMaxSpans || !out->push_back(CanvasSpan{y, x0, x1})) return false;
        }
      }
    }
  }
  return true;
}

// --- Stroking -----------------------------------------------------------------------------------------------------------------------

struct StrokeParams {
  double half = 0.5, miter_limit = 10;
  uint8_t cap = 0, join = 0;
};

double polygon_area(ArrayView<const CanvasPoint> p) {
  const uint64_t n = p.size();
  double a = 0;
  for (uint64_t i = 0; i < n; ++i) {
    const CanvasPoint& u = p[i];
    const CanvasPoint& v = p[(i + 1) % n];
    a += u.x * v.y - v.x * u.y;
  }
  return a / 2;
}

// Adds a polygon with a positive orientation (so overlapping ones add up under the non-zero rule).
bool add_polygon(List<CanvasPoint>* pts, List<Path::Sub>* subs, ArrayView<const CanvasPoint> p) {
  const uint32_t n = static_cast<uint32_t>(p.size());
  if (n < 3) return true;
  Path::Sub sub;
  sub.first = static_cast<uint32_t>(pts->count());
  sub.count = n;
  sub.closed = true;
  if (polygon_area(p) >= 0) {
    for (uint32_t i = 0; i < n; ++i)
      if (!pts->push_back(p[i])) return false;
  } else {
    for (uint32_t i = n; i > 0; --i)
      if (!pts->push_back(p[i - 1])) return false;
  }
  return subs->push_back(sub);
}

bool add_circle(List<CanvasPoint>* pts, List<Path::Sub>* subs, double cx, double cy, double r) {
  int32_t n = static_cast<int32_t>(r * 1.6) + 8;
  if (n > 48) n = 48;
  CanvasPoint poly_storage[48];
  const ArrayView<CanvasPoint> poly(poly_storage);
  for (int32_t i = 0; i < n; ++i) {
    const double t = kTwoPi * i / n;
    poly[i] = CanvasPoint{cx + r * fmath::cos(t), cy + r * fmath::sin(t)};
  }
  return add_polygon(pts, subs, poly.first(static_cast<uint64_t>(n)));
}

bool stroke_one(allocators::IAllocator& a, ArrayView<const CanvasPoint> src, bool closed, const StrokeParams& sp, List<CanvasPoint>* pts, List<Path::Sub>* subs) {
  List<CanvasPoint> v(a);
  for (uint64_t i = 0; i < src.size(); ++i) {
    if (src[i].x != src[i].x || src[i].y != src[i].y) continue;
    if (v.count() > 0) {
      const CanvasPoint& l = v[v.count() - 1];
      if (dabs(l.x - src[i].x) < 1e-9 && dabs(l.y - src[i].y) < 1e-9) continue;
    }
    if (!v.push_back(src[i])) return false;
  }
  if (closed && v.count() > 1 && dabs(v[v.count() - 1].x - v[0].x) < 1e-9 && dabs(v[v.count() - 1].y - v[0].y) < 1e-9) v.remove_at(v.count() - 1);
  const uint32_t n = static_cast<uint32_t>(v.count());
  if (n == 0) return true;
  const double h = sp.half;
  if (n == 1) {   // a point: only the caps show
    if (sp.cap == 1) return add_circle(pts, subs, v[0].x, v[0].y, h);
    if (sp.cap == 2) {
      const CanvasPoint sq[4] = {{v[0].x - h, v[0].y - h}, {v[0].x + h, v[0].y - h}, {v[0].x + h, v[0].y + h}, {v[0].x - h, v[0].y + h}};
      return add_polygon(pts, subs, sq);
    }
    return true;
  }
  if (closed && n < 3) closed = false;
  const uint32_t segments = closed ? n : n - 1;
  for (uint32_t i = 0; i < segments; ++i) {
    const CanvasPoint p0 = v[i], p1 = v[(i + 1) % n];
    const double dx = p1.x - p0.x, dy = p1.y - p0.y, len = dsqrt(dx * dx + dy * dy);
    const double nx = -dy / len * h, ny = dx / len * h;
    const CanvasPoint quad[4] = {{p0.x + nx, p0.y + ny}, {p1.x + nx, p1.y + ny}, {p1.x - nx, p1.y - ny}, {p0.x - nx, p0.y - ny}};
    if (!add_polygon(pts, subs, quad)) return false;
  }
  // joins
  const uint32_t first_join = closed ? 0 : 1, last_join = closed ? n : n - 1;
  for (uint32_t i = first_join; i < last_join; ++i) {
    const CanvasPoint prev = v[(i + n - 1) % n], at = v[i], nxt = v[(i + 1) % n];
    double d0x = at.x - prev.x, d0y = at.y - prev.y, d1x = nxt.x - at.x, d1y = nxt.y - at.y;
    const double l0 = dsqrt(d0x * d0x + d0y * d0y), l1 = dsqrt(d1x * d1x + d1y * d1y);
    d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
    const double cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
    if (dabs(cross) < 1e-9 && dot > 0) continue;
    if (sp.join == 1) {
      if (!add_circle(pts, subs, at.x, at.y, h)) return false;
      continue;
    }
    const double sgn = cross > 0 ? -1 : 1;
    const CanvasPoint o0{at.x + sgn * -d0y * h, at.y + sgn * d0x * h}, o1{at.x + sgn * -d1y * h, at.y + sgn * d1x * h};
    bool miter = false;
    if (sp.join == 0 && dot > -0.9999) {
      const double ratio = 1 / dsqrt((1 + dot) / 2);   // 1 / cos(turn / 2) = miter length / line width
      miter = ratio <= sp.miter_limit;
    }
    if (miter) {
      const double k = sgn / (1 + dot) * h;
      const CanvasPoint tip{at.x + (-d0y + -d1y) * k, at.y + (d0x + d1x) * k};
      const CanvasPoint poly[4] = {at, o0, tip, o1};
      if (!add_polygon(pts, subs, poly)) return false;
    } else {
      const CanvasPoint poly[3] = {at, o0, o1};
      if (!add_polygon(pts, subs, poly)) return false;
    }
  }
  // caps
  if (!closed && sp.cap != 0) {
    for (int end = 0; end < 2; ++end) {
      const CanvasPoint at = end ? v[n - 1] : v[0], other = end ? v[n - 2] : v[1];
      if (sp.cap == 1) {
        if (!add_circle(pts, subs, at.x, at.y, h)) return false;
        continue;
      }
      double dx = at.x - other.x, dy = at.y - other.y;
      const double len = dsqrt(dx * dx + dy * dy);
      dx /= len;
      dy /= len;
      const double nx = -dy * h, ny = dx * h;
      const CanvasPoint poly[4] = {{at.x + nx, at.y + ny}, {at.x + nx + dx * h, at.y + ny + dy * h}, {at.x - nx + dx * h, at.y - ny + dy * h}, {at.x - nx, at.y - ny}};
      if (!add_polygon(pts, subs, poly)) return false;
    }
  }
  return true;
}

// Splits a polyline into dashes (device units) and strokes each.
bool stroke_dashed(allocators::IAllocator& a, ArrayView<const CanvasPoint> v, bool closed, ArrayView<const double> pattern, double offset, const StrokeParams& sp,
                   List<CanvasPoint>* pts, List<Path::Sub>* subs) {
  const uint32_t n = static_cast<uint32_t>(v.size()), pattern_count = static_cast<uint32_t>(pattern.size());
  double total = 0;
  for (uint32_t i = 0; i < pattern_count; ++i) total += pattern[i];
  if (n < 2 || total <= 1e-9) return stroke_one(a, v, closed, sp, pts, subs);
  uint32_t idx = 0;
  double remaining = pattern[0];
  bool on = true;
  double skip = dmod(offset, total);
  while (skip > 0) {   // start part-way through the pattern
    if (skip >= remaining) {
      skip -= remaining;
      idx = (idx + 1) % pattern_count;
      remaining = pattern[idx];
      on = idx % 2 == 0;
    } else {
      remaining -= skip;
      skip = 0;
    }
  }
  List<CanvasPoint> cur(a);
  const uint32_t segments = closed ? n : n - 1;
  uint32_t guard = 0;
  for (uint32_t s = 0; s < segments; ++s) {
    const CanvasPoint p0 = v[s], p1 = v[(s + 1) % n];
    const double dx = p1.x - p0.x, dy = p1.y - p0.y, len = dsqrt(dx * dx + dy * dy);
    if (len < 1e-9) continue;
    double pos = 0;
    while (pos < len) {
      if (++guard > 400000) return true;
      const double take = remaining < len - pos ? remaining : len - pos;
      pos += take;
      remaining -= take;
      const CanvasPoint at{p0.x + dx * pos / len, p0.y + dy * pos / len};
      if (on) {
        if (cur.count() == 0) {
          const double back = pos - take;
          if (!cur.push_back(CanvasPoint{p0.x + dx * back / len, p0.y + dy * back / len})) return false;
        }
        if (!cur.push_back(at)) return false;
      }
      if (remaining <= 1e-9) {
        if (on) {
          if (cur.count() == 0 && !cur.push_back(at)) return false;
          if (!stroke_one(a, cur.view(), false, sp, pts, subs)) return false;
          cur.clear();
        }
        idx = (idx + 1) % pattern_count;
        remaining = pattern[idx];
        on = idx % 2 == 0;
      }
    }
  }
  if (on && cur.count() > 0) return stroke_one(a, cur.view(), false, sp, pts, subs);
  return true;
}

// The winding number of a point in the polygons.
int32_t winding_at(const List<CanvasPoint>& pts, const List<Path::Sub>& subs, double x, double y) {
  int32_t wind = 0;
  for (uint64_t s = 0; s < subs.count(); ++s) {
    const Path::Sub& sub = subs[s];
    if (sub.count < 3) continue;
    for (uint32_t i = 0; i < sub.count; ++i) {
      const CanvasPoint& p = pts[sub.first + i];
      const CanvasPoint& q = pts[sub.first + (i + 1) % sub.count];
      if ((p.y <= y) == (q.y <= y)) continue;
      const double cx = p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y);
      if (cx > x) wind += q.y > p.y ? 1 : -1;
    }
  }
  return wind;
}

// --- Pixels -------------------------------------------------------------------------------------------------------------------------

uint32_t composite_pixel(uint32_t dst, uint32_t src, Composite op) {
  if (op == Composite::SourceOver) return graphics::blend_over(src, dst);
  const float sa = static_cast<float>(src >> 24) / 255.0f, da = static_cast<float>(dst >> 24) / 255.0f;
  float sc[3], dc[3];
  for (int i = 0; i < 3; ++i) {
    sc[i] = static_cast<float>((src >> (16 - 8 * i)) & 0xFF) / 255.0f;
    dc[i] = static_cast<float>((dst >> (16 - 8 * i)) & 0xFF) / 255.0f;
  }
  float fa = 1, fb = 0;
  bool blend_mode = false;
  switch (op) {
    case Composite::SourceIn: fa = da; fb = 0; break;
    case Composite::SourceOut: fa = 1 - da; fb = 0; break;
    case Composite::SourceAtop: fa = da; fb = 1 - sa; break;
    case Composite::DestinationOver: fa = 1 - da; fb = 1; break;
    case Composite::DestinationIn: fa = 0; fb = sa; break;
    case Composite::DestinationOut: fa = 0; fb = 1 - sa; break;
    case Composite::DestinationAtop: fa = 1 - da; fb = sa; break;
    case Composite::Lighter: fa = 1; fb = 1; break;
    case Composite::Copy: fa = 1; fb = 0; break;
    case Composite::Xor: fa = 1 - da; fb = 1 - sa; break;
    default: blend_mode = true; break;
  }
  float out[3];
  float oa;
  if (blend_mode) {
    for (int i = 0; i < 3; ++i) {
      const float cb = dc[i], cs = sc[i];
      float b;
      switch (op) {
        case Composite::Multiply: b = cb * cs; break;
        case Composite::Screen: b = cb + cs - cb * cs; break;
        case Composite::Overlay: b = cb <= 0.5f ? cs * 2 * cb : 1 - (1 - cs) * (1 - (2 * cb - 1)); break;
        case Composite::Darken: b = cb < cs ? cb : cs; break;
        case Composite::Lighten: b = cb > cs ? cb : cs; break;
        case Composite::Difference: b = cb > cs ? cb - cs : cs - cb; break;
        default: b = cb + cs - 2 * cb * cs; break;
      }
      const float source_color = (1 - da) * cs + da * b;
      out[i] = sa * source_color + (1 - sa) * da * cb;
    }
    oa = sa + da * (1 - sa);
  } else {
    for (int i = 0; i < 3; ++i) out[i] = sa * sc[i] * fa + da * dc[i] * fb;
    oa = sa * fa + da * fb;
  }
  if (oa > 1) oa = 1;
  if (oa <= 0.0001f) return 0;
  uint32_t result = static_cast<uint32_t>(oa * 255 + 0.5f) << 24;
  for (int i = 0; i < 3; ++i) {
    float c = out[i] / oa;
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    result |= static_cast<uint32_t>(c * 255 + 0.5f) << (16 - 8 * i);
  }
  return result;
}

uint32_t with_alpha(uint32_t color, double alpha) {
  const double a = static_cast<double>(color >> 24) * alpha;
  return (static_cast<uint32_t>(a < 0 ? 0 : a > 255 ? 255 : a + 0.5) << 24) | (color & 0xFFFFFF);
}

}  // namespace

// --- CanvasBitmap ---------------------------------------------------------------------------------------------------------------------

CanvasBitmap::CanvasBitmap(allocators::IAllocator& allocator, uint32_t width, uint32_t height)
    : allocator_(allocator), width_(width), height_(height), ops_(allocator), op_images_(allocator), patterns_(allocator), masks_(allocator), saved_(allocator), path_(allocator) {
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension || uint64_t{width} * height > kMaxPixels) return;
  base_ = allocate_view<uint32_t>(allocator_, uint64_t{width} * height);
  if (base_.empty()) return;
  base_.fill(0);
  state_.clip_x1 = static_cast<int32_t>(width);
  state_.clip_y1 = static_cast<int32_t>(height);
  valid_ = true;
}

CanvasBitmap::~CanvasBitmap() {
  release_view(allocator_, base_);
  release_view(allocator_, scaled_);
  release_blocks(op_images_);
  release_blocks(patterns_);
  for (uint64_t i = 0; i < masks_.count(); ++i) release_view(allocator_, masks_[i].bytes);
}

bool CanvasBitmap::make_block(uint32_t w, uint32_t h, Block* block) {
  if (w == 0 || h == 0 || uint64_t{w} * h > kMaxPixels) return false;
  block->pixels = allocate_view<uint32_t>(allocator_, uint64_t{w} * h);
  if (block->pixels.empty()) return false;
  block->width = w;
  block->height = h;
  return true;
}

void CanvasBitmap::release_block(Block* block) {
  release_view(allocator_, block->pixels);
  block->width = block->height = 0;
}

void CanvasBitmap::release_blocks(List<Block>& blocks) {
  for (uint64_t i = 0; i < blocks.count(); ++i) release_block(&blocks[i]);
  blocks.clear();
}

void CanvasBitmap::save() {
  if (saved_.count() < kMaxStates) saved_.push_back(state_);
}

void CanvasBitmap::restore() {
  if (saved_.count() == 0) return;
  state_ = saved_[saved_.count() - 1];
  saved_.remove_at(saved_.count() - 1);
}

void CanvasBitmap::set_transform(const Transform& m) {
  for (uint64_t i = 0; i < 9; ++i)
    if (m.data[i] != m.data[i]) return;
  state_.ctm = m;
}

void CanvasBitmap::begin_path() { path_.clear(); }

void CanvasBitmap::clear_all() {
  ops_.clear();
  release_blocks(op_images_);
  if (!base_empty_) base_.fill(0);
  base_empty_ = true;
  touch();
}

void CanvasBitmap::reset() {
  clear_all();
  saved_.clear();
  path_.clear();
  for (uint64_t i = 0; i < masks_.count(); ++i) release_view(allocator_, masks_[i].bytes);
  masks_.clear();
  mask_bytes_ = 0;
  release_blocks(patterns_);
  state_ = CanvasState();
  state_.clip_x1 = static_cast<int32_t>(width_);
  state_.clip_y1 = static_cast<int32_t>(height_);
}

// --- Recording and playing back -----------------------------------------------------------------------------------------------------

bool CanvasBitmap::record_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
  if (w == 0 || h == 0 || (color >> 24) == 0) return true;
  if (ops_.count() >= kMaxPendingOps) flatten();
  Op op;
  op.kind = 0;
  op.x = x;
  op.y = y;
  op.w = w;
  op.h = h;
  op.color = color;
  op.cx0 = 0;
  op.cy0 = 0;
  op.cx1 = static_cast<int32_t>(width_);
  op.cy1 = static_cast<int32_t>(height_);
  touch();
  return ops_.push_back(op);
}

bool CanvasBitmap::record_image(int32_t x, int32_t y, const Block& block) {
  if (ops_.count() >= kMaxPendingOps) flatten();
  Op op;
  op.kind = 1;
  op.x = x;
  op.y = y;
  op.w = block.width;
  op.h = block.height;
  op.image = static_cast<uint32_t>(op_images_.count());
  op.cx0 = 0;
  op.cy0 = 0;
  op.cx1 = static_cast<int32_t>(width_);
  op.cy1 = static_cast<int32_t>(height_);
  Block copy;
  copy.pixels = block.pixels;
  copy.width = block.width;
  copy.height = block.height;
  if (!op_images_.push_back(copy)) return false;
  touch();
  return ops_.push_back(op);
}

void CanvasBitmap::replay(graphics::IRenderer2D& r, int32_t ox, int32_t oy, const Rect2D& view) {
  r.set_blend(graphics::BlendMode::Alpha);
  Rect2D bounds{ox, oy, width_, height_};
  for (uint64_t i = 0; i < ops_.count(); ++i) {
    const Op& op = ops_[i];
    // the visible part of the operation
    const int64_t x0 = int64_t{ox} + op.x, y0 = int64_t{oy} + op.y;
    int64_t cx0 = x0 > view.x ? x0 : view.x, cy0 = y0 > view.y ? y0 : view.y;
    int64_t cx1 = x0 + op.w < int64_t{view.x} + view.width ? x0 + op.w : int64_t{view.x} + view.width;
    int64_t cy1 = y0 + op.h < int64_t{view.y} + view.height ? y0 + op.h : int64_t{view.y} + view.height;
    if (cx0 < bounds.x) cx0 = bounds.x;
    if (cy0 < bounds.y) cy0 = bounds.y;
    if (cx1 > int64_t{bounds.x} + bounds.width) cx1 = int64_t{bounds.x} + bounds.width;
    if (cy1 > int64_t{bounds.y} + bounds.height) cy1 = int64_t{bounds.y} + bounds.height;
    if (cx1 <= cx0 || cy1 <= cy0) continue;
    if (op.kind == 0) {
      r.fill_rect({static_cast<int32_t>(cx0), static_cast<int32_t>(cy0), static_cast<uint32_t>(cx1 - cx0), static_cast<uint32_t>(cy1 - cy0)}, op.color);
    } else {
      const Block& block = op_images_[op.image];
      graphics::Surface s;
      s.pixels = block.pixels;
      s.width = block.width;
      s.height = block.height;
      s.pitch = block.width;
      r.draw_image(s, {static_cast<int32_t>(cx0 - x0), static_cast<int32_t>(cy0 - y0), static_cast<uint32_t>(cx1 - cx0), static_cast<uint32_t>(cy1 - cy0)},
                   static_cast<int32_t>(cx0), static_cast<int32_t>(cy0));
    }
  }
  r.set_clip(view);
}

void CanvasBitmap::flatten() {
  if (ops_.count() == 0) return;
  graphics::Surface s;
  s.pixels = base_;
  s.width = width_;
  s.height = height_;
  s.pitch = width_;
  graphics::SoftwareRenderer2D r(s);
  replay(r, 0, 0, Rect2D{0, 0, width_, height_});
  ops_.clear();
  release_blocks(op_images_);
  base_empty_ = false;
  ++flattened_;
}

ImageSource CanvasBitmap::snapshot() {
  flatten();
  ImageSource s;
  s.pixels = base_;
  s.width = width_;
  s.height = height_;
  s.pitch = width_;
  return s;
}

void CanvasBitmap::paint(graphics::IRenderer2D& dest, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip) {
  if (!valid_ || w == 0 || h == 0) return;
  const int64_t vx0 = x > clip.x ? x : clip.x, vy0 = y > clip.y ? y : clip.y;
  const int64_t vx1 = int64_t{x} + w < int64_t{clip.x} + clip.width ? int64_t{x} + w : int64_t{clip.x} + clip.width;
  const int64_t vy1 = int64_t{y} + h < int64_t{clip.y} + clip.height ? int64_t{y} + h : int64_t{clip.y} + clip.height;
  if (vx1 <= vx0 || vy1 <= vy0) return;
  dest.set_blend(graphics::BlendMode::Alpha);
  if (w == width_ && h == height_) {
    if (!base_empty_) {
      graphics::Surface s;
      s.pixels = base_;
      s.width = width_;
      s.height = height_;
      s.pitch = width_;
      dest.draw_image(s, {static_cast<int32_t>(vx0 - x), static_cast<int32_t>(vy0 - y), static_cast<uint32_t>(vx1 - vx0), static_cast<uint32_t>(vy1 - vy0)}, static_cast<int32_t>(vx0),
                      static_cast<int32_t>(vy0));
    }
    replay(dest, x, y, clip);
    return;
  }
  // shown at another size: from the pixels, scaled (kept until they change)
  flatten();
  if (scaled_w_ != w || scaled_h_ != h || scaled_version_ != version_ || scaled_.empty()) {
    if (uint64_t{w} * h > kMaxPixels) return;
    release_view(allocator_, scaled_);
    scaled_ = allocate_view<uint32_t>(allocator_, uint64_t{w} * h);
    if (scaled_.empty()) return;
    for (uint32_t j = 0; j < h; ++j) {
      const uint32_t sy = static_cast<uint32_t>(uint64_t{j} * height_ / h);
      for (uint32_t i = 0; i < w; ++i) scaled_[uint64_t{j} * w + i] = base_[uint64_t{sy} * width_ + static_cast<uint32_t>(uint64_t{i} * width_ / w)];
    }
    scaled_w_ = w;
    scaled_h_ = h;
    scaled_version_ = version_;
  }
  graphics::Surface s;
  s.pixels = scaled_;
  s.width = w;
  s.height = h;
  s.pitch = w;
  dest.draw_image(s, {static_cast<int32_t>(vx0 - x), static_cast<int32_t>(vy0 - y), static_cast<uint32_t>(vx1 - vx0), static_cast<uint32_t>(vy1 - vy0)}, static_cast<int32_t>(vx0),
                  static_cast<int32_t>(vy0));
  dest.set_clip(clip);
}

// --- Painting spans -----------------------------------------------------------------------------------------------------------------

uint32_t CanvasBitmap::solid_for(const CanvasPaint& p, double alpha) const { return with_alpha(p.color, alpha); }

bool CanvasBitmap::gpu_eligible(const CanvasPaint& p, double alpha) const {
  return p.kind == CanvasPaint::Kind::Solid && state_.composite == Composite::SourceOver && state_.clip_mask == 0 && alpha > 0;
}

uint8_t CanvasBitmap::mask_at(uint32_t id, int32_t x, int32_t y) const {
  if (id == 0 || id > masks_.count()) return 255;
  if (x < 0 || y < 0 || x >= static_cast<int32_t>(width_) || y >= static_cast<int32_t>(height_)) return 0;
  return masks_[id - 1].bytes[uint64_t{static_cast<uint32_t>(y)} * width_ + static_cast<uint32_t>(x)];
}

uint32_t CanvasBitmap::paint_color(const CanvasPaint& p, const Transform& inverse, double px, double py) const {
  if (p.kind == CanvasPaint::Kind::Solid) return p.color;
  double ux, uy;
  transform_point(inverse, px, py, &ux, &uy);
  switch (p.kind) {
    case CanvasPaint::Kind::Linear: {
      const double dx = p.x1 - p.x0, dy = p.y1 - p.y0, len2 = dx * dx + dy * dy;
      if (len2 == 0) return 0;
      return gradient_color(p, ((ux - p.x0) * dx + (uy - p.y0) * dy) / len2);
    }
    case CanvasPaint::Kind::Radial: {
      const double dx = p.x1 - p.x0, dy = p.y1 - p.y0, dr = p.r1 - p.r0;
      const double a = dx * dx + dy * dy - dr * dr;
      const double ex = ux - p.x0, ey = uy - p.y0;
      const double b = ex * dx + ey * dy + p.r0 * dr;
      const double c = ex * ex + ey * ey - p.r0 * p.r0;
      double t0, t1;
      if (dabs(a) < 1e-12) {
        if (dabs(b) < 1e-12) return 0;
        t0 = t1 = c / (2 * b);
      } else {
        const double disc = b * b - a * c;
        if (disc < 0) return 0;
        const double sq = dsqrt(disc);
        t0 = (b + sq) / a;
        t1 = (b - sq) / a;
        if (t0 < t1) {
          const double t = t0;
          t0 = t1;
          t1 = t;
        }
      }
      if (p.r0 + t0 * dr >= 0) return gradient_color(p, t0);
      if (p.r0 + t1 * dr >= 0) return gradient_color(p, t1);
      return 0;
    }
    case CanvasPaint::Kind::Conic: {
      const double angle = datan2(uy - p.y0, ux - p.x0) - p.r0;
      return gradient_color(p, dmod(angle / kTwoPi, 1.0));
    }
    case CanvasPaint::Kind::Pattern: {
      if (p.pattern == 0 || p.pattern > patterns_.count()) return 0;
      const Block& b = patterns_[p.pattern - 1];
      int64_t ix = static_cast<int64_t>(dfloor(ux)), iy = static_cast<int64_t>(dfloor(uy));
      if (p.repeat_x) {
        ix %= static_cast<int64_t>(b.width);
        if (ix < 0) ix += b.width;
      } else if (ix < 0 || ix >= static_cast<int64_t>(b.width)) {
        return 0;
      }
      if (p.repeat_y) {
        iy %= static_cast<int64_t>(b.height);
        if (iy < 0) iy += b.height;
      } else if (iy < 0 || iy >= static_cast<int64_t>(b.height)) {
        return 0;
      }
      return b.pixels[static_cast<uint64_t>(iy) * b.width + static_cast<uint64_t>(ix)];
    }
    default: return p.color;
  }
}

// Solid colour, plain blending: only recorded. Spans of consecutive rows with the same extent become one rectangle.
bool CanvasBitmap::emit_solid(const List<Span>& spans, int32_t dx, int32_t dy, uint32_t color) {
  if ((color >> 24) == 0) return true;
  struct Open {
    int32_t x0, x1, y0, y1;
  };
  List<Open> prev(allocator_), cur(allocator_);
  const int32_t cx0 = state_.clip_x0, cy0 = state_.clip_y0, cx1 = state_.clip_x1, cy1 = state_.clip_y1;
  auto flush_list = [&](List<Open>& list) {
    for (uint64_t i = 0; i < list.count(); ++i) {
      const Open& o = list[i];
      if (!record_rect(o.x0, o.y0, static_cast<uint32_t>(o.x1 - o.x0), static_cast<uint32_t>(o.y1 - o.y0), color)) return false;
    }
    list.clear();
    return true;
  };
  int32_t row = -0x7FFFFFFF;
  for (uint64_t i = 0; i < spans.count(); ++i) {
    const Span& sp = spans[i];
    const int32_t y = sp.y + dy;
    int32_t x0 = sp.x0 + dx, x1 = sp.x1 + dx;
    if (y < cy0 || y >= cy1) continue;
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    if (x1 <= x0) continue;
    if (y != row) {
      if (!flush_list(prev)) return false;
      for (uint64_t k = 0; k < cur.count(); ++k)
        if (!prev.push_back(cur[k])) return false;
      cur.clear();
      row = y;
    }
    bool merged = false;
    if (prev.count() <= 64) {
      for (uint64_t k = 0; k < prev.count(); ++k) {
        if (prev[k].x0 == x0 && prev[k].x1 == x1 && prev[k].y1 == y) {
          Open o = prev[k];
          o.y1 = y + 1;
          prev.remove_at(k);
          if (!cur.push_back(o)) return false;
          merged = true;
          break;
        }
      }
    }
    if (!merged && !cur.push_back(Open{x0, x1, y, y + 1})) return false;
  }
  return flush_list(prev) && flush_list(cur);
}

bool CanvasBitmap::cpu_spans(const List<Span>& spans, const CanvasPaint& paint, const Transform& inverse, int32_t dx, int32_t dy, double alpha, Composite op) {
  flatten();
  const bool whole = op == Composite::SourceIn || op == Composite::SourceOut || op == Composite::DestinationIn || op == Composite::DestinationAtop || op == Composite::Copy;
  const int32_t cx0 = state_.clip_x0, cy0 = state_.clip_y0, cx1 = state_.clip_x1, cy1 = state_.clip_y1;
  const uint32_t id = state_.clip_mask;
  auto put = [&](int32_t x, int32_t y, uint32_t src) {
    if (id && mask_at(id, x, y) == 0) return;
    uint32_t& d = base_[uint64_t{static_cast<uint32_t>(y)} * width_ + static_cast<uint32_t>(x)];
    d = composite_pixel(d, src, op);
  };
  uint64_t si = 0;
  if (whole) {
    for (int32_t y = cy0; y < cy1; ++y) {
      while (si < spans.count() && spans[si].y + dy < y) ++si;
      uint64_t sj = si;
      for (int32_t x = cx0; x < cx1; ++x) {
        while (sj < spans.count() && spans[sj].y + dy == y && spans[sj].x1 + dx <= x) ++sj;
        const bool covered = sj < spans.count() && spans[sj].y + dy == y && spans[sj].x0 + dx <= x && x < spans[sj].x1 + dx;
        uint32_t src = 0;
        if (covered) src = with_alpha(paint_color(paint, inverse, x - dx + 0.5, y - dy + 0.5), alpha);
        put(x, y, src);
      }
    }
    touch();
    base_empty_ = false;
    return true;
  }
  for (uint64_t i = 0; i < spans.count(); ++i) {
    const Span& sp = spans[i];
    const int32_t y = sp.y + dy;
    if (y < cy0 || y >= cy1) continue;
    int32_t x0 = sp.x0 + dx, x1 = sp.x1 + dx;
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    for (int32_t x = x0; x < x1; ++x) {
      const uint32_t color = paint.kind == CanvasPaint::Kind::Solid ? paint.color : paint_color(paint, inverse, x - dx + 0.5, y - dy + 0.5);
      const uint32_t src = with_alpha(color, alpha);
      if ((src >> 24) == 0 && op == Composite::SourceOver) continue;
      put(x, y, src);
    }
  }
  touch();
  base_empty_ = false;
  return true;
}

bool CanvasBitmap::emit_spans(const List<Span>& spans, const CanvasPaint& paint, const Transform& inverse, bool use_shadow) {
  const double alpha = state_.global_alpha;
  if (alpha <= 0 && state_.composite == Composite::SourceOver) return true;
  const bool shadow = use_shadow && (state_.shadow_color >> 24) != 0 && (state_.shadow_x != 0 || state_.shadow_y != 0 || state_.shadow_blur != 0);
  if (shadow) {
    CanvasPaint sp;
    sp.kind = CanvasPaint::Kind::Solid;
    sp.color = state_.shadow_color;
    const int32_t sx = round_i32(state_.shadow_x), sy = round_i32(state_.shadow_y);
    if (gpu_eligible(sp, alpha)) {
      if (!emit_solid(spans, sx, sy, solid_for(sp, alpha))) return false;
    } else if (!cpu_spans(spans, sp, inverse, sx, sy, alpha, state_.composite)) {
      return false;
    }
  }
  if (gpu_eligible(paint, alpha)) return emit_solid(spans, 0, 0, solid_for(paint, alpha));
  return cpu_spans(spans, paint, inverse, 0, 0, alpha, state_.composite);
}

bool CanvasBitmap::fill_polygons(const List<CanvasPoint>& points, const List<Path::Sub>& subs, bool even_odd, const CanvasPaint& paint, bool use_shadow) {
  List<Span> spans(allocator_);
  if (!scan_fill(allocator_, points, subs, even_odd, state_.clip_x0, state_.clip_y0, state_.clip_x1, state_.clip_y1, &spans)) return false;
  if (spans.count() == 0 && !(state_.composite == Composite::Copy || state_.composite == Composite::SourceIn || state_.composite == Composite::SourceOut ||
                              state_.composite == Composite::DestinationIn || state_.composite == Composite::DestinationAtop))
    return true;
  Transform inverse = Transform::Identity();
  if (!transform_invert(state_.ctm, &inverse)) {
    if (paint.kind != CanvasPaint::Kind::Solid) return true;
  }
  return emit_spans(spans, paint, inverse, use_shadow);
}

bool CanvasBitmap::fill_path(const Path* path, bool even_odd) {
  if (!valid_) return true;
  const Path& p = path ? *path : path_;
  List<CanvasPoint> points(allocator_);
  List<Path::Sub> subs(allocator_);
  if (!p.flatten(path ? state_.ctm : Transform::Identity(), &points, &subs)) return false;
  return fill_polygons(points, subs, even_odd, state_.fill, true);
}

bool CanvasBitmap::stroke_polygons(const List<CanvasPoint>& points, const List<Path::Sub>& subs, List<CanvasPoint>* out_points, List<Path::Sub>* out_subs) {
  const double scale = dsqrt(dabs(state_.ctm.determinant()));
  if (scale == 0 || state_.line_width <= 0) return true;
  StrokeParams sp;
  sp.half = state_.line_width * scale / 2;
  sp.miter_limit = state_.miter_limit;
  sp.cap = state_.line_cap;
  sp.join = state_.line_join;
  double pattern_storage[2 * CanvasState::kMaxDashes];
  uint32_t pattern_count = 0;
  if (state_.dash_count > 0) {
    const uint32_t reps = state_.dash_count % 2 ? 2 : 1;
    for (uint32_t r = 0; r < reps; ++r)
      for (uint32_t i = 0; i < state_.dash_count; ++i) pattern_storage[pattern_count++] = state_.dash[i] * scale;
  }
  for (uint64_t s = 0; s < subs.count(); ++s) {
    const Path::Sub& sub = subs[s];
    if (sub.count == 0) continue;
    const ArrayView<const CanvasPoint> v = points.view().subview(sub.first, sub.count);
    const ArrayView<const double> pattern(pattern_storage, pattern_count);
    const bool ok = pattern_count > 0 ? stroke_dashed(allocator_, v, sub.closed, pattern, state_.dash_offset * scale, sp, out_points, out_subs)
                                      : stroke_one(allocator_, v, sub.closed, sp, out_points, out_subs);
    if (!ok) return false;
  }
  return true;
}

bool CanvasBitmap::stroke_path(const Path* path) {
  if (!valid_) return true;
  const Path& p = path ? *path : path_;
  List<CanvasPoint> points(allocator_), outline(allocator_);
  List<Path::Sub> subs(allocator_), outline_subs(allocator_);
  if (!p.flatten(path ? state_.ctm : Transform::Identity(), &points, &subs)) return false;
  if (!stroke_polygons(points, subs, &outline, &outline_subs)) return false;
  return fill_polygons(outline, outline_subs, false, state_.stroke, true);
}

bool CanvasBitmap::in_path(const Path* path, double x, double y, bool even_odd) {
  const Path& p = path ? *path : path_;
  List<CanvasPoint> points(allocator_);
  List<Path::Sub> subs(allocator_);
  if (!p.flatten(path ? state_.ctm : Transform::Identity(), &points, &subs)) return false;
  const int32_t w = winding_at(points, subs, x, y);
  return even_odd ? (w & 1) != 0 : w != 0;
}

bool CanvasBitmap::in_stroke(const Path* path, double x, double y) {
  const Path& p = path ? *path : path_;
  List<CanvasPoint> points(allocator_), outline(allocator_);
  List<Path::Sub> subs(allocator_), outline_subs(allocator_);
  if (!p.flatten(path ? state_.ctm : Transform::Identity(), &points, &subs)) return false;
  if (!stroke_polygons(points, subs, &outline, &outline_subs)) return false;
  return winding_at(outline, outline_subs, x, y) != 0;
}

// --- Clip ------------------------------------------------------------------------------------------------------------------------------

bool CanvasBitmap::new_mask_from(const List<Span>& spans, uint32_t* id) {
  const uint64_t bytes = uint64_t{width_} * height_;
  if (masks_.count() >= 48 || mask_bytes_ + bytes > (96u << 20)) return false;
  Mask m;
  m.bytes = allocate_view<uint8_t>(allocator_, bytes);
  if (m.bytes.empty()) return false;
  m.bytes.fill(0);
  for (uint64_t i = 0; i < spans.count(); ++i) {
    const Span& s = spans[i];
    for (int32_t x = s.x0; x < s.x1; ++x) {
      uint8_t v = 255;
      if (state_.clip_mask) v = mask_at(state_.clip_mask, x, s.y);
      m.bytes[uint64_t{static_cast<uint32_t>(s.y)} * width_ + static_cast<uint32_t>(x)] = v;
    }
  }
  if (!masks_.push_back(m)) {
    release_view(allocator_, m.bytes);
    return false;
  }
  mask_bytes_ += bytes;
  *id = static_cast<uint32_t>(masks_.count());
  return true;
}

bool CanvasBitmap::clip_path(const Path* path, bool even_odd) {
  if (!valid_) return true;
  const Path& p = path ? *path : path_;
  List<CanvasPoint> points(allocator_);
  List<Path::Sub> subs(allocator_);
  if (!p.flatten(path ? state_.ctm : Transform::Identity(), &points, &subs)) return false;
  List<Span> spans(allocator_);
  if (!scan_fill(allocator_, points, subs, even_odd, state_.clip_x0, state_.clip_y0, state_.clip_x1, state_.clip_y1, &spans)) return false;
  if (spans.count() == 0) {
    state_.clip_x1 = state_.clip_x0;
    state_.clip_y1 = state_.clip_y0;
    return true;
  }
  int32_t bx0 = spans[0].x0, bx1 = spans[0].x1;
  const int32_t by0 = spans[0].y, by1 = spans[spans.count() - 1].y + 1;
  bool rectangular = true;
  for (uint64_t i = 0; i < spans.count(); ++i) {
    if (spans[i].x0 < bx0) bx0 = spans[i].x0;
    if (spans[i].x1 > bx1) bx1 = spans[i].x1;
  }
  for (uint64_t i = 0; i < spans.count() && rectangular; ++i) {
    rectangular = spans[i].x0 == spans[0].x0 && spans[i].x1 == spans[0].x1 && spans[i].y == by0 + static_cast<int32_t>(i);
  }
  if (rectangular && state_.clip_mask == 0) {
    state_.clip_x0 = bx0;
    state_.clip_x1 = bx1;
    state_.clip_y0 = by0;
    state_.clip_y1 = by1;
    return true;
  }
  uint32_t id = 0;
  if (!new_mask_from(spans, &id)) {   // (no room for another mask: keep the bounds only)
    state_.clip_x0 = bx0;
    state_.clip_x1 = bx1;
    state_.clip_y0 = by0;
    state_.clip_y1 = by1;
    return true;
  }
  state_.clip_mask = id;
  state_.clip_x0 = bx0;
  state_.clip_x1 = bx1;
  state_.clip_y0 = by0;
  state_.clip_y1 = by1;
  return true;
}

// --- Rectangles -----------------------------------------------------------------------------------------------------------------------

bool CanvasBitmap::fill_rect(double x, double y, double w, double h) {
  if (!valid_ || x != x || y != y || w != w || h != h) return true;
  if (w == 0 || h == 0) return true;
  Path p(allocator_);
  p.rect(x, y, w, h);
  return fill_path(&p, false);
}

bool CanvasBitmap::stroke_rect(double x, double y, double w, double h) {
  if (!valid_ || x != x || y != y || w != w || h != h) return true;
  Path p(allocator_);
  if (w == 0 && h == 0) return true;
  p.move_to(x, y);
  if (w == 0 || h == 0) {
    p.line_to(x + w, y + h);
  } else {
    p.line_to(x + w, y);
    p.line_to(x + w, y + h);
    p.line_to(x, y + h);
    p.close();
  }
  return stroke_path(&p);
}

bool CanvasBitmap::clear_rect(double x, double y, double w, double h) {
  if (!valid_ || x != x || y != y || w != w || h != h || w == 0 || h == 0) return true;
  {
    double ax, ay, bx, by;
    transform_point(state_.ctm, x, y, &ax, &ay);
    transform_point(state_.ctm, x + w, y + h, &bx, &by);
    if (transform_axis_aligned(state_.ctm)) {
      const double x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax, y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
      int32_t ix0 = first_pixel(x0), ix1 = first_pixel(x1), iy0 = first_pixel(y0), iy1 = first_pixel(y1);
      if (ix0 <= 0 && iy0 <= 0 && ix1 >= static_cast<int32_t>(width_) && iy1 >= static_cast<int32_t>(height_) && state_.clip_x0 <= 0 && state_.clip_y0 <= 0 &&
          state_.clip_x1 >= static_cast<int32_t>(width_) && state_.clip_y1 >= static_cast<int32_t>(height_) && state_.clip_mask == 0) {
        clear_all();   // everything goes: nothing recorded stays
        return true;
      }
      flatten();
      if (ix0 < state_.clip_x0) ix0 = state_.clip_x0;
      if (iy0 < state_.clip_y0) iy0 = state_.clip_y0;
      if (ix1 > state_.clip_x1) ix1 = state_.clip_x1;
      if (iy1 > state_.clip_y1) iy1 = state_.clip_y1;
      for (int32_t j = iy0; j < iy1; ++j)
        for (int32_t i = ix0; i < ix1; ++i)
          if (state_.clip_mask == 0 || mask_at(state_.clip_mask, i, j)) base_[uint64_t{static_cast<uint32_t>(j)} * width_ + static_cast<uint32_t>(i)] = 0;
      touch();
      return true;
    }
  }
  // turned or skewed: erase the shape
  Path p(allocator_);
  p.rect(x, y, w, h);
  List<CanvasPoint> points(allocator_);
  List<Path::Sub> subs(allocator_);
  if (!p.flatten(state_.ctm, &points, &subs)) return false;
  List<Span> spans(allocator_);
  if (!scan_fill(allocator_, points, subs, false, state_.clip_x0, state_.clip_y0, state_.clip_x1, state_.clip_y1, &spans)) return false;
  CanvasPaint clear;
  clear.color = 0xFF000000u;
  Transform identity = Transform::Identity();
  return cpu_spans(spans, clear, identity, 0, 0, 1.0, Composite::DestinationOut);
}

// --- Pictures ------------------------------------------------------------------------------------------------------------------------

namespace {

// One pixel of `src` at (fx, fy) (pixel centres at +0.5), nearest or bilinear, never reading outside [x0,x1) x [y0,y1) of the picture.
uint32_t sample(const ImageSource& src, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double fx, double fy, bool smooth) {
  if (!smooth) {
    int32_t ix = clamp_i32(dfloor(fx)), iy = clamp_i32(dfloor(fy));
    ix = ix < x0 ? x0 : ix >= x1 ? x1 - 1 : ix;
    iy = iy < y0 ? y0 : iy >= y1 ? y1 - 1 : iy;
    return src.pixels[uint64_t{static_cast<uint32_t>(iy)} * src.pitch + static_cast<uint32_t>(ix)];
  }
  const double gx = fx - 0.5, gy = fy - 0.5;
  const double fxl = dfloor(gx), fyl = dfloor(gy);
  const double tx = gx - fxl, ty = gy - fyl;
  int32_t ix0 = clamp_i32(fxl), iy0 = clamp_i32(fyl), ix1 = ix0 + 1, iy1 = iy0 + 1;
  auto cx = [&](int32_t v) { return v < x0 ? x0 : v >= x1 ? x1 - 1 : v; };
  auto cy = [&](int32_t v) { return v < y0 ? y0 : v >= y1 ? y1 - 1 : v; };
  ix0 = cx(ix0); ix1 = cx(ix1); iy0 = cy(iy0); iy1 = cy(iy1);
  const uint32_t p00 = src.pixels[uint64_t{static_cast<uint32_t>(iy0)} * src.pitch + static_cast<uint32_t>(ix0)];
  const uint32_t p10 = src.pixels[uint64_t{static_cast<uint32_t>(iy0)} * src.pitch + static_cast<uint32_t>(ix1)];
  const uint32_t p01 = src.pixels[uint64_t{static_cast<uint32_t>(iy1)} * src.pitch + static_cast<uint32_t>(ix0)];
  const uint32_t p11 = src.pixels[uint64_t{static_cast<uint32_t>(iy1)} * src.pitch + static_cast<uint32_t>(ix1)];
  if (p00 == p10 && p00 == p01 && p00 == p11) return p00;
  const double w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
  // weighted by alpha, so transparent pixels do not tint their neighbours
  const double a00 = (p00 >> 24) * w00, a10 = (p10 >> 24) * w10, a01 = (p01 >> 24) * w01, a11 = (p11 >> 24) * w11;
  const double a = a00 + a10 + a01 + a11;
  if (a <= 0.001) return 0;
  uint32_t out = static_cast<uint32_t>(a + 0.5) << 24;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const double c = (((p00 >> shift) & 0xFF) * a00 + ((p10 >> shift) & 0xFF) * a10 + ((p01 >> shift) & 0xFF) * a01 + ((p11 >> shift) & 0xFF) * a11) / a;
    out |= static_cast<uint32_t>(c < 0 ? 0 : c > 255 ? 255 : c + 0.5) << shift;
  }
  return out;
}

// The text as the screen font can show it (other characters come out as what they look like in ASCII, or '?').
void to_ascii(StringView text, String* out) {
  for (uint64_t i = 0; i < text.size();) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c < 0x80) {
      out->push_back(c >= 32 && c < 127 ? static_cast<char>(c) : ' ');
      ++i;
      continue;
    }
    const char* look = dom::ascii_look_alike(dom::next_code_point(text, &i));
    out->append(look ? StringView(look) : StringView("?"));
  }
}

}  // namespace

// Samples the source rectangle drawn into the destination rectangle (both in the source's and the user's space) into a block of device pixels - only the part
// that is inside the clip. `*ox`, `*oy` say where the block's top left is.
bool CanvasBitmap::sample_picture(const ImageSource& src, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh, Block* block, int32_t* ox,
                                  int32_t* oy) {
  if (!src.valid() || sw == 0 || sh == 0 || dw == 0 || dh == 0) return false;
  if (sw < 0) {
    sx += sw;
    sw = -sw;
    dx += dw;
    dw = -dw;
  }
  if (sh < 0) {
    sy += sh;
    sh = -sh;
    dy += dh;
    dh = -dh;
  }
  // the part of the source that exists
  double cx0 = sx < 0 ? 0 : sx, cy0 = sy < 0 ? 0 : sy;
  double cx1 = sx + sw > src.width ? src.width : sx + sw, cy1 = sy + sh > src.height ? src.height : sy + sh;
  if (cx1 <= cx0 || cy1 <= cy0) return false;
  const double t0x = (cx0 - sx) / sw, t1x = (cx1 - sx) / sw, t0y = (cy0 - sy) / sh, t1y = (cy1 - sy) / sh;
  const double ndx = dx + t0x * dw, ndw = (t1x - t0x) * dw, ndy = dy + t0y * dh, ndh = (t1y - t0y) * dh;
  const int32_t px0 = static_cast<int32_t>(dfloor(cx0)), py0 = static_cast<int32_t>(dfloor(cy0));
  int32_t px1 = static_cast<int32_t>(dceil(cx1)), py1 = static_cast<int32_t>(dceil(cy1));
  if (px1 > static_cast<int32_t>(src.width)) px1 = static_cast<int32_t>(src.width);
  if (py1 > static_cast<int32_t>(src.height)) py1 = static_cast<int32_t>(src.height);
  // the device bounding box of the destination rectangle
  double xs[4], ys[4];
  transform_point(state_.ctm, ndx, ndy, &xs[0], &ys[0]);
  transform_point(state_.ctm, ndx + ndw, ndy, &xs[1], &ys[1]);
  transform_point(state_.ctm, ndx + ndw, ndy + ndh, &xs[2], &ys[2]);
  transform_point(state_.ctm, ndx, ndy + ndh, &xs[3], &ys[3]);
  double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
  for (int i = 1; i < 4; ++i) {
    if (xs[i] < minx) minx = xs[i];
    if (xs[i] > maxx) maxx = xs[i];
    if (ys[i] < miny) miny = ys[i];
    if (ys[i] > maxy) maxy = ys[i];
  }
  int32_t bx0 = first_pixel(minx), bx1 = first_pixel(maxx), by0 = first_pixel(miny), by1 = first_pixel(maxy);
  if (bx0 < state_.clip_x0) bx0 = state_.clip_x0;
  if (by0 < state_.clip_y0) by0 = state_.clip_y0;
  if (bx1 > state_.clip_x1) bx1 = state_.clip_x1;
  if (by1 > state_.clip_y1) by1 = state_.clip_y1;
  if (bx1 <= bx0 || by1 <= by0) return false;
  const bool aligned = transform_axis_aligned(state_.ctm);
  Transform inverse = Transform::Identity();
  if (!aligned && !transform_invert(state_.ctm, &inverse)) return false;
  const uint32_t bw = static_cast<uint32_t>(bx1 - bx0), bh = static_cast<uint32_t>(by1 - by0);
  if (!make_block(bw, bh, block)) return false;
  const bool smooth = state_.smoothing;
  const double scale_x = dabs(tr_a(state_.ctm) * ndw / (cx1 - cx0)), scale_y = dabs(tr_d(state_.ctm) * ndh / (cy1 - cy0));
  const bool resampling = aligned ? (dabs(scale_x - 1) > 1e-6 || dabs(scale_y - 1) > 1e-6) : true;
  const bool use_smooth = smooth && resampling;
  const double inv_dw = 1 / ndw, inv_dh = 1 / ndh;
  for (uint32_t j = 0; j < bh; ++j) {
    uint32_t* row = block->pixels.data() + uint64_t{j} * bw;
    for (uint32_t i = 0; i < bw; ++i) {
      const double X = bx0 + i + 0.5, Y = by0 + j + 0.5;
      double ux, uy;
      if (aligned) {
        ux = (X - tr_e(state_.ctm)) / tr_a(state_.ctm);
        uy = (Y - tr_f(state_.ctm)) / tr_d(state_.ctm);
      } else {
        transform_point(inverse, X, Y, &ux, &uy);
      }
      const double tx = (ux - ndx) * inv_dw, ty = (uy - ndy) * inv_dh;
      if (tx < 0 || tx >= 1 || ty < 0 || ty >= 1) {
        row[i] = 0;
        continue;
      }
      row[i] = sample(src, px0, py0, px1, py1, cx0 + tx * (cx1 - cx0), cy0 + ty * (cy1 - cy0), use_smooth);
    }
  }
  *ox = bx0;
  *oy = by0;
  return true;
}

// Puts a block of device pixels on the canvas: recorded when the display can do it, else mixed into the pixels.
bool CanvasBitmap::place_block(Block* block, int32_t ox, int32_t oy, double alpha, bool use_shadow) {
  if (alpha <= 0 && state_.composite == Composite::SourceOver) {
    release_block(block);
    return true;
  }
  if (alpha < 1) {
    for (uint64_t i = 0; i < block->pixels.size(); ++i) block->pixels[i] = with_alpha(block->pixels[i], alpha);
  }
  const bool direct = state_.composite == Composite::SourceOver && state_.clip_mask == 0;
  if (use_shadow && (state_.shadow_color >> 24) != 0 && (state_.shadow_x != 0 || state_.shadow_y != 0 || state_.shadow_blur != 0)) {
    Block shadow;
    if (make_block(block->width, block->height, &shadow)) {
      for (uint64_t i = 0; i < shadow.pixels.size(); ++i) {
        const uint32_t a = block->pixels[i] >> 24;
        shadow.pixels[i] = with_alpha(state_.shadow_color, a / 255.0);
      }
      int32_t sx = ox + round_i32(state_.shadow_x), sy = oy + round_i32(state_.shadow_y);
      if (direct) {
        // (the shadow is kept as a recorded picture, clipped to the clip rectangle by replay)
        Block clipped;
        if (!clip_block(&shadow, &sx, &sy, &clipped)) release_block(&shadow);
        else {
          release_block(&shadow);
          if (!record_image(sx, sy, clipped)) {
            release_block(&clipped);
            return false;
          }
        }
      } else {
        flatten();
        mix_block(shadow, sx, sy);
        release_block(&shadow);
      }
    }
  }
  if (direct) {
    Block clipped;
    if (!clip_block(block, &ox, &oy, &clipped)) {
      release_block(block);
      return true;
    }
    release_block(block);
    if (!record_image(ox, oy, clipped)) {
      release_block(&clipped);
      return false;
    }
    return true;
  }
  flatten();
  mix_block(*block, ox, oy);
  release_block(block);
  return true;
}

// The part of a block inside the clip rectangle, as a block of its own (the original is left alone).
bool CanvasBitmap::clip_block(const Block* block, int32_t* ox, int32_t* oy, Block* out) {
  const int32_t x0 = *ox > state_.clip_x0 ? *ox : state_.clip_x0, y0 = *oy > state_.clip_y0 ? *oy : state_.clip_y0;
  int32_t x1 = *ox + static_cast<int32_t>(block->width), y1 = *oy + static_cast<int32_t>(block->height);
  if (x1 > state_.clip_x1) x1 = state_.clip_x1;
  if (y1 > state_.clip_y1) y1 = state_.clip_y1;
  if (x1 <= x0 || y1 <= y0) return false;
  if (!make_block(static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), out)) return false;
  for (int32_t j = y0; j < y1; ++j)
    for (int32_t i = x0; i < x1; ++i)
      out->pixels[uint64_t{static_cast<uint32_t>(j - y0)} * out->width + static_cast<uint32_t>(i - x0)] =
          block->pixels[uint64_t{static_cast<uint32_t>(j - *oy)} * block->width + static_cast<uint32_t>(i - *ox)];
  *ox = x0;
  *oy = y0;
  return true;
}

void CanvasBitmap::mix_block(const Block& block, int32_t ox, int32_t oy) {
  const Composite op = state_.composite;
  for (uint32_t j = 0; j < block.height; ++j) {
    const int32_t y = oy + static_cast<int32_t>(j);
    if (y < state_.clip_y0 || y >= state_.clip_y1) continue;
    for (uint32_t i = 0; i < block.width; ++i) {
      const int32_t x = ox + static_cast<int32_t>(i);
      if (x < state_.clip_x0 || x >= state_.clip_x1) continue;
      if (state_.clip_mask && mask_at(state_.clip_mask, x, y) == 0) continue;
      const uint32_t src = block.pixels[uint64_t{j} * block.width + i];
      if ((src >> 24) == 0 && op == Composite::SourceOver) continue;
      uint32_t& d = base_[uint64_t{static_cast<uint32_t>(y)} * width_ + static_cast<uint32_t>(x)];
      d = composite_pixel(d, src, op);
    }
  }
  base_empty_ = false;
  touch();
}

bool CanvasBitmap::draw_image(const ImageSource& source, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh) {
  if (!valid_ || sx != sx || sy != sy || sw != sw || sh != sh || dx != dx || dy != dy || dw != dw || dh != dh) return true;
  Block block;
  int32_t ox = 0, oy = 0;
  if (!sample_picture(source, sx, sy, sw, sh, dx, dy, dw, dh, &block, &ox, &oy)) return true;
  return place_block(&block, ox, oy, state_.global_alpha, true);
}

// --- Text --------------------------------------------------------------------------------------------------------------------------

double CanvasBitmap::measure_text(StringView text) const {
  String shown(allocator_);
  to_ascii(text, &shown);
  return static_cast<double>(shown.size()) * 8.0 * state_.font_size / 16.0;
}

bool CanvasBitmap::text_mask(StringView text, bool bold, Block* mask) {
  String shown(allocator_);
  to_ascii(text, &shown);
  if (shown.empty() || shown.size() > 4096) return false;
  const uint32_t w = static_cast<uint32_t>(shown.size()) * 8, h = 16;
  if (!make_block(w, h, mask)) return false;
  mask->pixels.fill(0);
  for (uint64_t c = 0; c < shown.size(); ++c) {
    const ConstByteView glyph = hardware::Font8x16::glyph(shown[c]);
    for (uint32_t j = 0; j < h; ++j)
      for (uint32_t i = 0; i < 8; ++i)
        if (glyph[j] & (0x80 >> i)) {
          mask->pixels[uint64_t{j} * w + c * 8 + i] = 0xFFFFFFFFu;
          if (bold && c * 8 + i + 1 < w) mask->pixels[uint64_t{j} * w + c * 8 + i + 1] = 0xFFFFFFFFu;
        }
  }
  return true;
}

bool CanvasBitmap::draw_text(StringView text, double x, double y, double max_width, bool stroke) {
  if (!valid_ || x != x || y != y || max_width != max_width || max_width <= 0) return true;
  Block mask;
  if (!text_mask(text, state_.font_bold, &mask)) return true;
  ImageSource src;
  src.pixels = mask.pixels;
  src.width = mask.width;
  src.height = mask.height;
  src.pitch = mask.width;
  const double fs = state_.font_size > 0 ? state_.font_size : 10;
  double width = mask.width * fs / 16.0;
  const double height = fs;
  double shrink = 1;
  if (width > max_width) {
    shrink = max_width / width;
    width = max_width;
  }
  (void)shrink;
  double left = x;
  switch (state_.text_align) {
    case 1: case 3: left = x - width; break;      // end (left to right text), right
    case 4: left = x - width / 2; break;          // center
    default: break;
  }
  double top = y - 12.0 * fs / 16.0;   // alphabetic: the baseline is 12 rows down in the 16 row font
  switch (state_.text_baseline) {
    case 1: top = y; break;
    case 2: top = y - 1.5 * fs / 16.0; break;
    case 3: top = y - height / 2; break;
    case 4: top = y - 14.0 * fs / 16.0; break;
    case 5: top = y - height; break;
    default: break;
  }
  Block block;
  int32_t ox = 0, oy = 0;
  const bool smooth_before = state_.smoothing;
  state_.smoothing = fs != 16.0;
  const bool ok = sample_picture(src, 0, 0, mask.width, mask.height, left, top, width, height, &block, &ox, &oy);
  state_.smoothing = smooth_before;
  release_block(&mask);
  if (!ok) return true;
  // the mask's coverage becomes the style's colour
  const CanvasPaint& paint = stroke ? state_.stroke : state_.fill;
  Transform inverse = Transform::Identity();
  const bool have_inverse = transform_invert(state_.ctm, &inverse);
  for (uint32_t j = 0; j < block.height; ++j)
    for (uint32_t i = 0; i < block.width; ++i) {
      uint32_t& px = block.pixels[uint64_t{j} * block.width + i];
      const uint32_t coverage = px >> 24;
      if (coverage == 0) continue;
      uint32_t color = paint.color;
      if (paint.kind != CanvasPaint::Kind::Solid) color = have_inverse ? paint_color(paint, inverse, ox + i + 0.5, oy + j + 0.5) : 0;
      px = (mul_div_255(color >> 24, coverage) << 24) | (color & 0xFFFFFF);
    }
  return place_block(&block, ox, oy, state_.global_alpha, true);
}

bool CanvasBitmap::fill_text(StringView text, double x, double y, double max_width) { return draw_text(text, x, y, max_width, false); }
bool CanvasBitmap::stroke_text(StringView text, double x, double y, double max_width) { return draw_text(text, x, y, max_width, true); }

// --- Pixels ------------------------------------------------------------------------------------------------------------------------

bool CanvasBitmap::get_image_data(int32_t x, int32_t y, uint32_t w, uint32_t h, ArrayView<uint32_t> out) {
  if (!valid_ || out.size() < uint64_t{w} * h) return false;
  flatten();
  for (uint32_t j = 0; j < h; ++j)
    for (uint32_t i = 0; i < w; ++i) {
      const int64_t px = int64_t{x} + i, py = int64_t{y} + j;
      out[uint64_t{j} * w + i] = px < 0 || py < 0 || px >= width_ || py >= height_ ? 0 : base_[static_cast<uint64_t>(py) * width_ + static_cast<uint64_t>(px)];
    }
  return true;
}

bool CanvasBitmap::put_image_data(const ImageSource& source, int32_t dx, int32_t dy, int32_t dirty_x, int32_t dirty_y, int32_t dirty_w, int32_t dirty_h) {
  if (!valid_ || !source.valid()) return true;
  if (dirty_w < 0) {
    dirty_x += dirty_w;
    dirty_w = -dirty_w;
  }
  if (dirty_h < 0) {
    dirty_y += dirty_h;
    dirty_h = -dirty_h;
  }
  flatten();
  for (int32_t j = dirty_y < 0 ? 0 : dirty_y; j < dirty_y + dirty_h && j < static_cast<int32_t>(source.height); ++j)
    for (int32_t i = dirty_x < 0 ? 0 : dirty_x; i < dirty_x + dirty_w && i < static_cast<int32_t>(source.width); ++i) {
      const int64_t px = int64_t{dx} + i, py = int64_t{dy} + j;
      if (px < 0 || py < 0 || px >= width_ || py >= height_) continue;
      base_[static_cast<uint64_t>(py) * width_ + static_cast<uint64_t>(px)] = source.pixels[uint64_t{static_cast<uint32_t>(j)} * source.pitch + static_cast<uint32_t>(i)];
    }
  base_empty_ = false;
  touch();
  return true;
}

uint32_t CanvasBitmap::add_pattern(const ImageSource& source) {
  if (!source.valid() || patterns_.count() >= 64) return 0;
  Block b;
  if (!make_block(source.width, source.height, &b)) return 0;
  for (uint32_t j = 0; j < source.height; ++j)
    for (uint32_t i = 0; i < source.width; ++i) b.pixels[uint64_t{j} * source.width + i] = source.pixels[uint64_t{j} * source.pitch + i];
  if (!patterns_.push_back(b)) {
    release_block(&b);
    return 0;
  }
  return static_cast<uint32_t>(patterns_.count());
}

}  // namespace browser
