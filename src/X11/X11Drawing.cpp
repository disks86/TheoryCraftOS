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

// The X server's drawing: where a drawable's pixels are (a canvas: a picture and a clip list), the GC's raster operation, fill style and clip, and the core
// drawing requests (points, lines, segments, rectangles, arcs, polygons, images, text, copies). Everything ends in fill_span(), which clips against the
// canvas' rectangles and the GC's clip mask, so no drawing writes outside its picture whatever coordinates a request carries.

#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Math/FloatMath.hpp"
#include "X11/X11Internal.hpp"

namespace x11 {

namespace {

constexpr int32_t kTextAscent = 12;
constexpr uint32_t kMaxClipRects = 1024;

uint32_t rop(uint8_t function, uint32_t s, uint32_t d) {
  switch (function & 15) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return ~s & d;
    case 5: return d;
    case 6: return s ^ d;
    case 7: return s | d;
    case 8: return ~(s | d);
    case 9: return ~s ^ d;
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return ~0u;
  }
}

// ceil and round for coordinates (clamped to a range every int32_t holds)
int32_t ceil_of(double v) {
  if (v > 1e9) v = 1e9;
  if (v < -1e9) v = -1e9;
  int32_t i = static_cast<int32_t>(v);
  if (i < v) ++i;
  return i;
}
int32_t round_of(double v) { return ceil_of(v - 0.5); }

// The value of picture `p` at (x, y), tiled from (0, 0).
uint32_t tiled(const Picture& p, int32_t x, int32_t y) {
  if (p.width == 0 || p.height == 0) return 0;
  const int64_t tx = ((int64_t{x} % p.width) + p.width) % p.width, ty = ((int64_t{y} % p.height) + p.height) % p.height;
  return p.pixels[static_cast<uint64_t>(ty) * p.width + static_cast<uint64_t>(tx)];
}

// atan2 in radians (-pi, pi], good to about 0.005 radians: enough to decide which pixels an arc's angles include.
double angle_of(double y, double x) {
  constexpr double kPi = 3.14159265358979323846;
  if (x == 0 && y == 0) return 0;
  const double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  const double a = ax > ay ? ay / ax : ax / ay;
  const double s = a * a;
  double r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
  if (ay > ax) r = kPi / 2 - r;
  if (x < 0) r = kPi - r;
  return y < 0 ? -r : r;
}

}  // namespace

// ------------------------------------------------------------------ canvases

bool Server::canvas_of(Window* w, Pixmap* p, Gc* gc, bool include_inferiors, Canvas* out) {
  out->clip.set({});
  out->mask = nullptr;
  out->drew = false;
  out->dirty = {};
  if (p) {
    Picture& pic = *p->picture;
    out->picture = &pic;
    out->ox = out->oy = 0;
    out->depth = pic.depth;
    out->top = nullptr;
    out->clip.set({0, 0, static_cast<int32_t>(pic.width), static_cast<int32_t>(pic.height)});
  } else {
    if (!w || w->input_only) return false;
    Window* top = top_of(w);
    if (!top || !top->picture || !viewable(w)) return false;
    out->picture = top->picture.get();
    out->top = top;
    out->depth = w->depth;
    int32_t ox = 0, oy = 0;
    for (const Window* a = w; a != top; a = a->parent) {
      ox += a->x + static_cast<int32_t>(a->border);
      oy += a->y + static_cast<int32_t>(a->border);
    }
    out->ox = ox;
    out->oy = oy;
    out->clip.set({0, 0, static_cast<int32_t>(w->width), static_cast<int32_t>(w->height)});
    out->clip.intersect({-ox, -oy, static_cast<int32_t>(out->picture->width), static_cast<int32_t>(out->picture->height)});
    // the ancestors' insides, and the siblings stacked above each window on the way up
    int32_t ax = ox, ay = oy;      // the inside of `a` in picture coordinates
    for (Window* a = w; a != top; a = a->parent) {
      Window* parent = a->parent;
      const int32_t px = ax - a->x - static_cast<int32_t>(a->border), py = ay - a->y - static_cast<int32_t>(a->border);
      out->clip.intersect({px - ox, py - oy, static_cast<int32_t>(parent->width), static_cast<int32_t>(parent->height)});
      bool above = false;
      for (Window* s : parent->children.view()) {
        if (s == a) { above = true; continue; }
        if (!above || !s->mapped) continue;
        out->clip.subtract({px + s->x - ox, py + s->y - oy, static_cast<int32_t>(s->width + 2 * s->border), static_cast<int32_t>(s->height + 2 * s->border)});
      }
      ax = px;
      ay = py;
    }
    if (!include_inferiors)
      for (Window* ch : w->children.view())
        if (ch->mapped) out->clip.subtract({ch->x, ch->y, static_cast<int32_t>(ch->width + 2 * ch->border), static_cast<int32_t>(ch->height + 2 * ch->border)});
  }
  if (gc) {
    if (gc->clip_rects) {
      // the union of the GC's rectangles (made not to overlap: an XOR must not draw a pixel twice), within the clip
      Region piece(allocator_);
      List<Rect> pieces(allocator_);
      uint32_t n = 0;
      for (const Rect& cr : gc->clip_rects->view()) {
        if (++n > kMaxClipRects) break;
        const Rect moved{cr.x + gc->clip_x, cr.y + gc->clip_y, cr.w, cr.h};
        for (const Rect& base : out->clip.rects()) {
          const Rect is = intersect(base, moved);
          if (is.empty()) continue;
          piece.set(is);
          for (const Rect& done : pieces.view()) piece.subtract(done);
          for (const Rect& keep : piece.rects()) pieces.push_back(keep);
        }
      }
      out->clip.assign(pieces.view());
    } else if (gc->clip_mask) {
      out->mask = gc->clip_mask.get();
      out->mask_x = gc->clip_x;
      out->mask_y = gc->clip_y;
    }
  }
  return true;
}

void Server::finish(Canvas& cv) {
  if (!cv.top || !cv.drew) return;
  note_repaint(*cv.top, intersect({cv.dirty.x + cv.ox, cv.dirty.y + cv.oy, cv.dirty.w, cv.dirty.h},
                                  {0, 0, static_cast<int32_t>(cv.picture->width), static_cast<int32_t>(cv.picture->height)}));
}

// ------------------------------------------------------------------ pixels

Ink Server::ink_of(const Gc& gc) const {
  Ink k;
  k.function = gc.function;
  k.fill_style = gc.fill_style;
  k.foreground = gc.foreground;
  k.background = gc.background;
  k.plane_mask = gc.plane_mask;
  return k;
}

void Server::fill_span(Canvas& cv, const Gc* gc, const Ink& ink, int32_t y, int32_t x0, int32_t x1, ArrayView<const uint32_t> source) {
  if (x1 <= x0) return;
  const bool sourced = !source.empty();      // (one source value per pixel of the span: an image or a copy, drawn as a plain fill)
  if (sourced && source.size() < static_cast<uint64_t>(int64_t{x1} - x0)) return;
  Picture& pic = *cv.picture;
  const uint32_t dm = depth_mask(cv.depth);
  const uint32_t pm = ink.plane_mask & dm;
  const Picture* tile = gc && gc->tile ? gc->tile.get() : nullptr;
  const Picture* stipple = gc && gc->stipple ? gc->stipple.get() : nullptr;
  const int32_t ts_x = gc ? gc->ts_x : 0, ts_y = gc ? gc->ts_y : 0;
  for (const Rect& r : cv.clip.rects()) {
    if (y < r.y || y >= r.y + r.h) continue;
    const int32_t a = max32(x0, r.x), b = min32(x1, r.x + r.w);
    if (a >= b) continue;
    const uint64_t row = static_cast<uint64_t>(y + cv.oy) * pic.width;
    const bool plain = ink.function == 3 && pm == dm && !cv.mask && (ink.fill_style == 0 || sourced);
    if (plain && sourced) {
      for (int32_t x = a; x < b; ++x) pic.pixels[row + static_cast<uint64_t>(x + cv.ox)] = stored_pixel(cv.depth, source[static_cast<uint64_t>(x - x0)]);
    } else if (plain) {
      const uint32_t v = stored_pixel(cv.depth, ink.foreground);
      for (int32_t x = a; x < b; ++x) pic.pixels[row + static_cast<uint64_t>(x + cv.ox)] = v;
    } else {
      for (int32_t x = a; x < b; ++x) {
        if (cv.mask) {
          const int32_t mx = x - cv.mask_x, my = y - cv.mask_y;
          if (mx < 0 || my < 0 || static_cast<uint32_t>(mx) >= cv.mask->width || static_cast<uint32_t>(my) >= cv.mask->height) continue;
          if (!cv.mask->pixels[static_cast<uint64_t>(my) * cv.mask->width + static_cast<uint64_t>(mx)]) continue;
        }
        uint32_t src;
        if (sourced) src = source[static_cast<uint64_t>(x - x0)];
        else switch (ink.fill_style) {
          case 1: src = tile ? pixel_value(tile->depth, tiled(*tile, x - ts_x, y - ts_y)) : ink.foreground; break;
          case 2:
            if (stipple && !tiled(*stipple, x - ts_x, y - ts_y)) continue;
            src = ink.foreground;
            break;
          case 3: src = !stipple || tiled(*stipple, x - ts_x, y - ts_y) ? ink.foreground : ink.background; break;
          default: src = ink.foreground; break;
        }
        uint32_t& px = pic.pixels[row + static_cast<uint64_t>(x + cv.ox)];
        const uint32_t d = pixel_value(cv.depth, px);
        const uint32_t v = (d & ~pm) | (rop(ink.function, src, d) & pm);
        px = stored_pixel(cv.depth, v & dm);
      }
    }
    cv.dirty = cv.drew ? unite(cv.dirty, {a, y, b - a, 1}) : Rect{a, y, b - a, 1};
    cv.drew = true;
  }
}

void Server::fill_rect(Canvas& cv, const Gc* gc, const Ink& ink, const Rect& r) {
  const Rect b = intersect(r, cv.clip.bounds());
  for (int32_t y = b.y; y < b.y + b.h; ++y) fill_span(cv, gc, ink, y, b.x, b.x + b.w);
}

// ------------------------------------------------------------------ primitives

void Server::draw_line(Canvas& cv, const Gc& gc, int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool last) {
  const Ink ink = ink_of(gc);
  if (gc.line_width <= 1) {
    const int64_t dx = x1 > x0 ? int64_t{x1} - x0 : int64_t{x0} - x1, dy = y1 > y0 ? int64_t{y1} - y0 : int64_t{y0} - y1;
    const int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int64_t err = dx - dy;
    int32_t x = x0, y = y0;
    const Rect bounds = cv.clip.bounds();
    for (;;) {
      const bool end = x == x1 && y == y1;
      if (end && !last) break;
      if (contains(bounds, x, y)) fill_span(cv, &gc, ink, y, x, x + 1);
      if (end) break;
      const int64_t e2 = 2 * err;
      if (e2 > -dy) { err -= dy; x += sx; }
      if (e2 < dx) { err += dx; y += sy; }
    }
    return;
  }
  // a wide line: the rectangle around the segment (butt caps; projecting caps grow it by half the width)
  const double fx = double(x1) - x0, fy = double(y1) - y0;
  double length = fmath::sqrt(fx * fx + fy * fy);
  const double half = gc.line_width / 2.0;
  if (length < 1e-9) {
    const int32_t h = static_cast<int32_t>(gc.line_width);
    if (gc.cap_style == 3 || gc.cap_style == 2) fill_rect(cv, &gc, ink, {x0 - h / 2, y0 - h / 2, h, h});
    return;
  }
  const double ux = fx / length, uy = fy / length;
  const double nx = -uy * half, ny = ux * half;
  const double ext = gc.cap_style == 3 ? half : 0;      // CapProjecting
  const double ax = x0 - ux * ext, ay = y0 - uy * ext, bx = x1 + ux * ext, by = y1 + uy * ext;
  int32_t xs[4], ys[4];
  const double px[4] = {ax + nx, bx + nx, bx - nx, ax - nx}, py[4] = {ay + ny, by + ny, by - ny, ay - ny};
  for (uint32_t i = 0; i < 4; ++i) {
    xs[i] = round_of(px[i]);
    ys[i] = round_of(py[i]);
  }
  fill_polygon(cv, gc, ArrayView<const int32_t>(xs, 4), ArrayView<const int32_t>(ys, 4), true);
}

void Server::fill_polygon(Canvas& cv, const Gc& gc, ArrayView<const int32_t> xs, ArrayView<const int32_t> ys, bool winding) {
  const uint64_t n = xs.size();
  if (n < 3 || ys.size() != n) return;
  int32_t miny = ys[0], maxy = ys[0];
  for (int32_t y : ys) {
    miny = min32(miny, y);
    maxy = max32(maxy, y);
  }
  const Rect bounds = cv.clip.bounds();
  miny = max32(miny, bounds.y);
  maxy = min32(maxy, bounds.y + bounds.h);
  const Ink ink = ink_of(gc);
  List<double> cross(allocator_);
  List<int32_t> dirs(allocator_);
  for (int32_t y = miny; y < maxy; ++y) {
    const double cy = y + 0.5;
    cross.clear();
    dirs.clear();
    for (uint64_t i = 0; i < n; ++i) {
      const double x0 = xs[i], y0 = ys[i], x1 = xs[(i + 1) % n], y1 = ys[(i + 1) % n];
      if (y0 == y1) continue;
      const bool up = y1 < y0;
      const double lo = up ? y1 : y0, hi = up ? y0 : y1;
      if (cy < lo || cy >= hi) continue;
      const double t = (cy - y0) / (y1 - y0);
      const double x = x0 + t * (x1 - x0);
      // insertion keeps them sorted
      uint64_t at = cross.count();
      while (at > 0 && cross[at - 1] > x) --at;
      cross.insert(at, x);
      dirs.insert(at, up ? -1 : 1);
    }
    // pixels whose centres are inside: x + 0.5 in [start, end)
    int32_t count = 0;
    double start = 0;
    for (uint64_t i = 0; i < cross.count(); ++i) {
      const bool was_inside = winding ? count != 0 : (count & 1) != 0;
      count += winding ? dirs[i] : 1;
      const bool inside = winding ? count != 0 : (count & 1) != 0;
      if (!was_inside && inside) start = cross[i];
      else if (was_inside && !inside) fill_span(cv, &gc, ink, y, ceil_of(start - 0.5), ceil_of(cross[i] - 0.5));
    }
  }
}

void Server::draw_arc(Canvas& cv, const Gc& gc, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t a1, int32_t a2, bool fill) {
  constexpr double kPi = 3.14159265358979323846;
  if (w == 0 || h == 0 || w > 32767 || h > 32767) return;
  const bool full = a2 >= 360 * 64 || a2 <= -360 * 64;
  double start = a1 / 64.0 * kPi / 180.0, extent = a2 / 64.0 * kPi / 180.0;
  if (extent < 0) {
    start += extent;
    extent = -extent;
  }
  const double rx = w / 2.0, ry = h / 2.0, cx = x + rx, cy = y + ry;
  auto in_sector = [&](double px, double py) {
    if (full) return true;
    double a = angle_of(-(py - cy) / ry, (px - cx) / rx) - start;
    while (a < 0) a += 2 * kPi;
    while (a >= 2 * kPi) a -= 2 * kPi;
    return a <= extent;
  };
  if (fill) {
    const Ink ink = ink_of(gc);
    // chord: the side of the line between the arc's ends that holds its middle
    const double sx = cx + rx * fmath::cos(start), sy = cy - ry * fmath::sin(start);
    const double ex = cx + rx * fmath::cos(start + extent), ey = cy - ry * fmath::sin(start + extent);
    const double mx = cx + rx * fmath::cos(start + extent / 2), my = cy - ry * fmath::sin(start + extent / 2);
    const double side = (ex - sx) * (my - sy) - (ey - sy) * (mx - sx);
    const Rect bounds = intersect({x, y, static_cast<int32_t>(w), static_cast<int32_t>(h)}, cv.clip.bounds());
    for (int32_t py = bounds.y; py < bounds.y + bounds.h; ++py) {
      const double dy = (py + 0.5 - cy) / ry;
      if (dy * dy >= 1) continue;
      const double half = rx * fmath::sqrt(1 - dy * dy);
      int32_t xa = ceil_of(cx - half - 0.5), xb = ceil_of(cx + half - 0.5);      // centres in [cx - half, cx + half)
      xa = max32(xa, bounds.x);
      xb = min32(xb, bounds.x + bounds.w);
      if (full) {
        fill_span(cv, &gc, ink, py, xa, xb);
        continue;
      }
      int32_t run = -1;
      for (int32_t px = xa; px <= xb; ++px) {
        bool in = px < xb;
        if (in) {
          const double qx = px + 0.5, qy = py + 0.5;
          if (gc.arc_mode == 0) in = side == 0 || ((ex - sx) * (qy - sy) - (ey - sy) * (qx - sx)) * side >= 0;      // ArcChord
          else in = in_sector(qx, qy);                                                                              // ArcPieSlice
        }
        if (in && run < 0) run = px;
        if (!in && run >= 0) {
          fill_span(cv, &gc, ink, py, run, px);
          run = -1;
        }
      }
    }
    return;
  }
  // the outline: a polyline through points on the ellipse
  const double perimeter = 2 * kPi * fmath::sqrt((rx * rx + ry * ry) / 2);
  uint32_t steps = static_cast<uint32_t>(perimeter * (full ? 1 : extent / (2 * kPi)) / 2) + 8;
  if (steps > 4096) steps = 4096;
  const double span = full ? 2 * kPi : extent;
  int32_t px = 0, py = 0;
  for (uint32_t i = 0; i <= steps; ++i) {
    const double t = start + span * i / steps;
    const int32_t qx = round_of(cx + rx * fmath::cos(t) - 0.5), qy = round_of(cy - ry * fmath::sin(t) - 0.5);
    if (i > 0 && (qx != px || qy != py)) draw_line(cv, gc, px, py, qx, qy, i == steps);
    px = qx;
    py = qy;
  }
}

void Server::draw_text(Canvas& cv, const Gc& gc, int32_t x, int32_t y, ConstByteView chars, bool sixteen, bool image) {
  const uint64_t count = sixteen ? chars.size() / 2 : chars.size();
  const int32_t top = y - kTextAscent;
  Ink ink = ink_of(gc);
  if (image) {      // ImageText: the cells in the background first; both plain copies whatever the GC's function and fill
    Ink back;
    back.foreground = gc.background;
    back.plane_mask = gc.plane_mask;
    fill_rect(cv, nullptr, back, {x, top, static_cast<int32_t>(count * 8), 16});
    ink.function = 3;
    ink.fill_style = 0;
  }
  for (uint64_t i = 0; i < count; ++i) {
    const uint32_t code = sixteen ? (uint32_t{chars[2 * i]} << 8 | chars[2 * i + 1]) : chars[i];
    uint8_t rows[hardware::Font8x16::kHeight];
    hardware::Font8x16::glyph_of(static_cast<char32_t>(code), rows);
    const int32_t gx = x + static_cast<int32_t>(i * 8);
    for (int32_t r = 0; r < 16; ++r) {
      const uint8_t bits = rows[r];
      for (int32_t b = 0; b < 8;) {
        if (!(bits & (0x80 >> b))) { ++b; continue; }
        int32_t e = b;
        while (e < 8 && (bits & (0x80 >> e))) ++e;
        fill_span(cv, image ? nullptr : &gc, ink, top + r, gx + b, gx + e);
        b = e;
      }
    }
  }
}

// ------------------------------------------------------------------ backgrounds, borders and exposures

void Server::paint_background(Window& w, const Rect& area) {
  Window* source = &w;
  int32_t sx = 0, sy = 0;      // where the background's tile starts, in w's coordinates
  while (source->background == 3 && source->parent) {      // ParentRelative: the parent's, aligned with the parent
    sx -= source->x + static_cast<int32_t>(source->border);
    sy -= source->y + static_cast<int32_t>(source->border);
    source = source->parent;
  }
  if (source->background == 0 || source->background == 3) return;
  Canvas cv(allocator_);
  if (!canvas_of(&w, nullptr, nullptr, false, &cv)) return;
  cv.clip.intersect(area);
  Picture& pic = *cv.picture;
  for (const Rect& r : cv.clip.rects()) {
    for (int32_t y = r.y; y < r.y + r.h; ++y) {
      const uint64_t row = static_cast<uint64_t>(y + cv.oy) * pic.width;
      for (int32_t x = r.x; x < r.x + r.w; ++x) {
        uint32_t v = source->background_pixel;
        if (source->background == 2 && source->background_picture) v = tiled(*source->background_picture, x - sx, y - sy);
        pic.pixels[row + static_cast<uint64_t>(x + cv.ox)] = v;
      }
    }
    cv.dirty = cv.drew ? unite(cv.dirty, r) : r;
    cv.drew = true;
  }
  finish(cv);
}

void Server::paint_border(Window& w) {
  if (w.border == 0 || !w.parent || w.parent == root_ || w.input_only) return;
  Canvas cv(allocator_);
  if (!canvas_of(w.parent, nullptr, nullptr, true, &cv)) return;
  const int32_t b = static_cast<int32_t>(w.border);
  cv.clip.intersect({w.x, w.y, static_cast<int32_t>(w.width) + 2 * b, static_cast<int32_t>(w.height) + 2 * b});
  cv.clip.subtract({w.x + b, w.y + b, static_cast<int32_t>(w.width), static_cast<int32_t>(w.height)});
  bool above = false;
  for (Window* s : w.parent->children.view()) {
    if (s == &w) { above = true; continue; }
    if (above && s->mapped) cv.clip.subtract({s->x, s->y, static_cast<int32_t>(s->width + 2 * s->border), static_cast<int32_t>(s->height + 2 * s->border)});
  }
  Ink ink;
  ink.foreground = pixel_value(cv.depth, w.border_pixel);
  fill_rect(cv, nullptr, ink, cv.clip.bounds());
  finish(cv);
}

void Server::send_expose(Window& w, const Rect& r, uint16_t count) {
  Packet p;
  p.u8(0, event::kExpose);
  p.u32(4, w.id);
  p.u16(8, static_cast<uint16_t>(r.x));
  p.u16(10, static_cast<uint16_t>(r.y));
  p.u16(12, static_cast<uint16_t>(r.w));
  p.u16(14, static_cast<uint16_t>(r.h));
  p.u16(16, count);
  deliver(w, mask::kExposure, p);
}

void Server::expose_tree(Window& w) {
  if (w.input_only || !viewable(&w)) return;
  paint_border(w);
  const Rect all{0, 0, static_cast<int32_t>(w.width), static_cast<int32_t>(w.height)};
  paint_background(w, all);
  if (selected(w, mask::kExposure)) send_expose(w, all, 0);
  for (uint64_t i = 0; i < w.children.count(); ++i) {
    Window* ch = w.children[i];
    if (ch->mapped) expose_tree(*ch);
  }
}

void Server::expose_area(Window& w, const Rect& area) {
  if (w.input_only || !viewable(&w)) return;
  const Rect inside = intersect(area, {0, 0, static_cast<int32_t>(w.width), static_cast<int32_t>(w.height)});
  if (!inside.empty()) {
    Canvas cv(allocator_);
    if (canvas_of(&w, nullptr, nullptr, false, &cv)) {
      cv.clip.intersect(inside);
      if (!cv.clip.empty()) {
        paint_background(w, inside);
        if (selected(w, mask::kExposure)) {
          const uint64_t n = cv.clip.rects().size();
          uint64_t i = 0;
          for (const Rect& r : cv.clip.rects()) send_expose(w, r, static_cast<uint16_t>(n - 1 - i++ > 0xFFFF ? 0xFFFF : n - i));
        }
      }
    }
  }
  for (uint64_t i = 0; i < w.children.count(); ++i) {
    Window* ch = w.children[i];
    if (!ch->mapped) continue;
    const int32_t b = static_cast<int32_t>(ch->border);
    const Rect outer{ch->x, ch->y, static_cast<int32_t>(ch->width) + 2 * b, static_cast<int32_t>(ch->height) + 2 * b};
    if (intersect(outer, area).empty()) continue;
    paint_border(*ch);
    expose_area(*ch, {area.x - ch->x - b, area.y - ch->y - b, area.w, area.h});
  }
}

// ------------------------------------------------------------------ requests

void Server::poly_request(Client& c, uint8_t opcode, uint8_t data, WireReader& r) {
  const uint32_t drawable = r.u32(), gc_id = r.u32();
  if (!r.ok()) { error(c, error::kLength, 0, opcode); return; }
  Window* w;
  Pixmap* px;
  if (!drawable_of(c, drawable, opcode, &w, &px)) return;
  Gc* gc = gc_of(c, gc_id, opcode);
  if (!gc) return;
  if (w && w->input_only) { error(c, error::kMatch, drawable, opcode); return; }
  const uint8_t depth = w ? w->depth : px->picture->depth;
  if (depth != gc->depth) { error(c, error::kMatch, gc_id, opcode); return; }
  Canvas cv(allocator_);
  const bool can_draw = canvas_of(w, px, gc, gc->subwindow_mode == 1, &cv);
  const Ink ink = ink_of(*gc);
  switch (opcode) {
    case 64: case 65: {      // PolyPoint, PolyLine (coordinate mode: 0 Origin, 1 Previous)
      if (data > 1) { error(c, error::kValue, data, opcode); return; }
      if (!can_draw) return;
      int32_t lx = 0, ly = 0;
      bool first = true;
      const uint64_t n = r.remaining() / 4;
      for (uint64_t i = 0; i < n; ++i) {
        int32_t x = r.i16(), y = r.i16();
        if (data == 1 && !first) { x += lx; y += ly; }
        if (opcode == 64) fill_span(cv, gc, ink, y, x, x + 1);
        else if (!first) draw_line(cv, *gc, lx, ly, x, y, i + 1 == n && gc->cap_style != 0);
        else if (n == 1) fill_span(cv, gc, ink, y, x, x + 1);
        lx = x;
        ly = y;
        first = false;
      }
      break;
    }
    case 66: {      // PolySegment
      if (!can_draw) return;
      while (r.remaining() >= 8) {
        const int16_t x1 = r.i16(), y1 = r.i16(), x2 = r.i16(), y2 = r.i16();
        draw_line(cv, *gc, x1, y1, x2, y2, gc->cap_style != 0);
      }
      break;
    }
    case 67: case 70: {      // PolyRectangle, PolyFillRectangle
      if (!can_draw) return;
      while (r.remaining() >= 8) {
        const int16_t x = r.i16(), y = r.i16();
        const uint16_t width = r.u16(), height = r.u16();
        if (opcode == 70) {
          fill_rect(cv, gc, ink, {x, y, width, height});
        } else if (gc->line_width <= 1) {      // the outline covers [x, x + width] x [y, y + height]
          fill_rect(cv, gc, ink, {x, y, width + 1, 1});
          if (height > 0) fill_rect(cv, gc, ink, {x, y + height, width + 1, 1});
          if (height > 1) {
            fill_rect(cv, gc, ink, {x, y + 1, 1, height - 1});
            if (width > 0) fill_rect(cv, gc, ink, {x + width, y + 1, 1, height - 1});
          }
        } else {
          const int32_t lw = static_cast<int32_t>(gc->line_width), h = lw / 2;
          fill_rect(cv, gc, ink, {x - h, y - h, width + lw, lw});
          fill_rect(cv, gc, ink, {x - h, y + height - h, width + lw, lw});
          if (height > lw) {
            fill_rect(cv, gc, ink, {x - h, y - h + lw, lw, height - lw});
            fill_rect(cv, gc, ink, {x + width - h, y - h + lw, lw, height - lw});
          }
        }
      }
      break;
    }
    case 68: case 71: {      // PolyArc, PolyFillArc
      if (!can_draw) return;
      while (r.remaining() >= 12) {
        const int16_t x = r.i16(), y = r.i16();
        const uint16_t width = r.u16(), height = r.u16();
        const int16_t a1 = r.i16(), a2 = r.i16();
        draw_arc(cv, *gc, x, y, width, height, a1, a2, opcode == 71);
      }
      break;
    }
    case 69: {      // FillPoly: shape, coordinate mode
      r.u8();
      const uint8_t mode = r.u8();
      r.skip(2);
      if (!r.ok()) { error(c, error::kLength, 0, 69); return; }
      if (mode > 1) { error(c, error::kValue, mode, 69); return; }
      if (!can_draw) return;
      const uint64_t n = r.remaining() / 4;
      List<int32_t> xs(allocator_), ys(allocator_);
      if (!xs.reserve(n) || !ys.reserve(n)) { error(c, error::kAlloc, 0, 69); return; }
      int32_t lx = 0, ly = 0;
      for (uint64_t i = 0; i < n; ++i) {
        int32_t x = r.i16(), y = r.i16();
        if (mode == 1 && i > 0) { x += lx; y += ly; }
        xs.push_back(x);
        ys.push_back(y);
        lx = x;
        ly = y;
      }
      fill_polygon(cv, *gc, xs.view(), ys.view(), gc->fill_rule == 1);
      break;
    }
    case 74: case 75: {      // PolyText8, PolyText16: items of (length, delta, string) or a font change (length 255)
      int32_t x = r.i16();
      const int32_t y = r.i16();
      const bool sixteen = opcode == 75;
      while (r.remaining() >= 2) {
        const uint8_t n = r.u8();
        if (n == 255) {
          ConstByteView f = r.take(4);
          if (!r.ok()) break;
          const uint32_t font = uint32_t{f[0]} << 24 | uint32_t{f[1]} << 16 | uint32_t{f[2]} << 8 | f[3];      // (always most significant byte first)
          Resource* fr = resources_.find(font);
          if (!fr || fr->kind != Kind::Font) { error(c, error::kFont, font, opcode); break; }
          gc->font = font;
          continue;
        }
        x += r.i8();
        ConstByteView text = r.take(sixteen ? uint64_t{n} * 2 : n);
        if (!r.ok()) break;
        if (can_draw) draw_text(cv, *gc, x, y, text, sixteen, false);
        x += 8 * n;
      }
      break;
    }
    case 76: case 77: {      // ImageText8, ImageText16
      const int16_t x = r.i16(), y = r.i16();
      ConstByteView text = r.take(opcode == 77 ? uint64_t{data} * 2 : data);
      if (!r.ok()) { error(c, error::kLength, 0, opcode); return; }
      if (can_draw) draw_text(cv, *gc, x, y, text, opcode == 77, true);
      break;
    }
    default: break;
  }
  if (can_draw) finish(cv);
}

void Server::copy_area(Client& c, WireReader& r, bool plane) {
  const uint8_t major = plane ? 63 : 62;
  const uint32_t src_id = r.u32(), dst_id = r.u32(), gc_id = r.u32();
  const int16_t sx = r.i16(), sy = r.i16(), dx = r.i16(), dy = r.i16();
  const uint16_t width = r.u16(), height = r.u16();
  const uint32_t bit_plane = plane ? r.u32() : 0;
  if (!r.ok()) { error(c, error::kLength, 0, major); return; }
  Window *sw, *dw;
  Pixmap *sp, *dp;
  if (!drawable_of(c, src_id, major, &sw, &sp) || !drawable_of(c, dst_id, major, &dw, &dp)) return;
  Gc* gc = gc_of(c, gc_id, major);
  if (!gc) return;
  const uint8_t sdepth = sw ? sw->depth : sp->picture->depth, ddepth = dw ? dw->depth : dp->picture->depth;
  if ((sw && sw->input_only) || (dw && dw->input_only) || ddepth != gc->depth || (!plane && sdepth != ddepth)) {
    error(c, error::kMatch, 0, major);
    return;
  }
  if (plane && (bit_plane == 0 || (bit_plane & (bit_plane - 1)) != 0 || (sdepth < 32 && bit_plane >= (1u << sdepth)))) {
    error(c, error::kValue, bit_plane, major);
    return;
  }
  Canvas src(allocator_), dst(allocator_);
  const bool have_src = canvas_of(sw, sp, nullptr, gc->subwindow_mode == 1, &src);
  const bool have_dst = canvas_of(dw, dp, gc, gc->subwindow_mode == 1, &dst);
  if (have_src && have_dst && width > 0 && height > 0) {
    // the source first, whole (source and destination may overlap)
    List<uint32_t> pixels(allocator_);
    if (!pixels.reserve(uint64_t{width} * height)) { error(c, error::kAlloc, 0, major); return; }
    const Rect readable = src.clip.bounds();
    for (int32_t y = 0; y < height; ++y)
      for (int32_t x = 0; x < width; ++x) {
        const int32_t qx = sx + x, qy = sy + y;
        uint32_t v = 0;
        if (contains(readable, qx, qy))
          v = pixel_value(src.depth, src.picture->pixels[static_cast<uint64_t>(qy + src.oy) * src.picture->width + static_cast<uint64_t>(qx + src.ox)]);
        pixels.push_back(v);
      }
    Ink ink = ink_of(*gc);
    ink.fill_style = 0;
    if (plane)      // CopyPlane: the bit chooses the foreground or the background
      for (uint32_t& v : pixels.view()) v = (v & bit_plane) ? gc->foreground : gc->background;
    for (int32_t y = 0; y < height; ++y)
      fill_span(dst, gc, ink, dy + y, dx, dx + width, pixels.view().subview(uint64_t(y) * width, width));
    finish(dst);
  }
  if (gc->graphics_exposures) {
    Packet p;
    p.u8(0, event::kNoExpose);
    p.u32(4, dst_id);
    p.u16(8, 0);
    p.u8(10, major);
    send(c, p);
  }
}

void Server::put_image(Client& c, uint8_t format, WireReader& r, uint64_t length) {
  (void)length;
  const uint32_t drawable = r.u32(), gc_id = r.u32();
  const uint16_t width = r.u16(), height = r.u16();
  const int16_t dx = r.i16(), dy = r.i16();
  const uint8_t left_pad = r.u8(), depth = r.u8();
  r.skip(2);
  if (!r.ok()) { error(c, error::kLength, 0, 72); return; }
  Window* w;
  Pixmap* px;
  if (!drawable_of(c, drawable, 72, &w, &px)) return;
  Gc* gc = gc_of(c, gc_id, 72);
  if (!gc) return;
  if (w && w->input_only) { error(c, error::kMatch, drawable, 72); return; }
  const uint8_t ddepth = w ? w->depth : px->picture->depth;
  if (ddepth != gc->depth) { error(c, error::kMatch, gc_id, 72); return; }
  if (format > 2) { error(c, error::kValue, format, 72); return; }
  if ((format == 0 && depth != 1) || (format != 0 && depth != ddepth) || (format == 2 && left_pad != 0) || left_pad >= 32) {
    error(c, error::kMatch, 0, 72);
    return;
  }
  // the bytes one row (of one plane) takes
  const uint32_t bpp = depth == 1 ? 1 : depth == 8 ? 8 : 32;
  const uint64_t row_bits = format == 2 ? uint64_t{width} * bpp : uint64_t{width} + left_pad;
  const uint64_t row = (row_bits + 31) / 32 * 4;
  const uint32_t planes = format == 1 ? depth : 1;
  const uint64_t need = row * height * planes;
  if (need > r.remaining()) { error(c, error::kLength, 0, 72); return; }
  ConstByteView bits = r.take(need);
  Canvas cv(allocator_);
  if (!canvas_of(w, px, gc, gc->subwindow_mode == 1, &cv) || width == 0 || height == 0) return;
  Ink ink = ink_of(*gc);
  ink.fill_style = 0;
  auto bit_at = [&](uint64_t plane_base, uint32_t y, uint64_t bit) {      // (bitmap bit order: least significant first)
    const uint64_t at = plane_base + uint64_t{y} * row + bit / 8;
    return (bits[at] >> (bit % 8)) & 1u;
  };
  List<uint32_t> values(allocator_);
  if (!values.reserve(width)) { error(c, error::kAlloc, 0, 72); return; }
  for (uint32_t y = 0; y < height; ++y) {
    values.clear();
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t v;
      if (format == 0) {
        v = bit_at(0, y, uint64_t{x} + left_pad) ? gc->foreground : gc->background;
      } else if (format == 1) {
        v = 0;
        for (uint32_t p = 0; p < planes; ++p) v = (v << 1) | bit_at(uint64_t{p} * row * height, y, uint64_t{x} + left_pad);      // (most significant plane first)
      } else if (bpp == 1) {
        v = bit_at(0, y, x);
      } else if (bpp == 8) {
        v = bits[uint64_t{y} * row + x];
      } else {
        const uint64_t at = uint64_t{y} * row + uint64_t{x} * 4;
        v = uint32_t{bits[at]} | uint32_t{bits[at + 1]} << 8 | uint32_t{bits[at + 2]} << 16 | uint32_t{bits[at + 3]} << 24;      // (image byte order: LSBFirst)
      }
      values.push_back(v);
    }
    fill_span(cv, gc, ink, dy + static_cast<int32_t>(y), dx, dx + static_cast<int32_t>(width), values.view());
  }
  finish(cv);
}

void Server::get_image(Client& c, uint8_t format, WireReader& r) {
  const uint32_t drawable = r.u32();
  const int16_t x = r.i16(), y = r.i16();
  const uint16_t width = r.u16(), height = r.u16();
  const uint32_t plane_mask = r.u32();
  if (!r.ok()) { error(c, error::kLength, 0, 73); return; }
  if (format != 1 && format != 2) { error(c, error::kValue, format, 73); return; }
  Window* w;
  Pixmap* px;
  if (!drawable_of(c, drawable, 73, &w, &px)) return;
  if (w && w->input_only) { error(c, error::kMatch, drawable, 73); return; }
  const uint8_t depth = w ? w->depth : px->picture->depth;
  const uint32_t dw = w ? w->width : px->picture->width, dh = w ? w->height : px->picture->height;
  if (x < 0 || y < 0 || uint32_t(x) + width > dw || uint32_t(y) + height > dh || (w && !viewable(w))) { error(c, error::kMatch, 0, 73); return; }
  Canvas cv(allocator_);
  const bool readable = canvas_of(w, px, nullptr, true, &cv);
  auto value_at = [&](int32_t qx, int32_t qy) -> uint32_t {
    if (!readable) return 0;
    const int64_t picture_x = int64_t{qx} + cv.ox, picture_y = int64_t{qy} + cv.oy;
    if (picture_x < 0 || picture_y < 0 || picture_x >= cv.picture->width || picture_y >= cv.picture->height) return 0;
    return pixel_value(depth, cv.picture->pixels[static_cast<uint64_t>(picture_y) * cv.picture->width + static_cast<uint64_t>(picture_x)]);
  };
  const uint32_t bpp = depth == 1 ? 1 : depth == 8 ? 8 : 32;
  const uint32_t dm = depth_mask(depth);
  ByteBuffer bytes(allocator_);
  if (format == 2) {
    const uint64_t row = (uint64_t{width} * bpp + 31) / 32 * 4;
    if (row * height > kMaxRequestBytes || !bytes.put_zeros(row * height)) { error(c, error::kAlloc, 0, 73); return; }
    ByteView out = bytes.writable_view();
    for (uint32_t j = 0; j < height; ++j)
      for (uint32_t i = 0; i < width; ++i) {
        const uint32_t v = value_at(x + int32_t(i), y + int32_t(j)) & plane_mask & dm;
        const uint64_t base = uint64_t{j} * row;
        if (bpp == 1) out[base + i / 8] |= static_cast<uint8_t>((v & 1) << (i % 8));
        else if (bpp == 8) out[base + i] = static_cast<uint8_t>(v);
        else for (uint32_t k = 0; k < 4; ++k) out[base + uint64_t{i} * 4 + k] = static_cast<uint8_t>(v >> (8 * k));
      }
  } else {
    const uint64_t row = (uint64_t{width} + 31) / 32 * 4;
    uint32_t planes = 0;
    for (int32_t p = depth - 1; p >= 0; --p) if (plane_mask & (1u << p)) ++planes;
    if (row * height * planes > kMaxRequestBytes || !bytes.put_zeros(row * height * planes)) { error(c, error::kAlloc, 0, 73); return; }
    ByteView out = bytes.writable_view();
    uint64_t base = 0;
    for (int32_t p = depth - 1; p >= 0; --p) {
      if (!(plane_mask & (1u << p))) continue;
      for (uint32_t j = 0; j < height; ++j)
        for (uint32_t i = 0; i < width; ++i)
          if ((value_at(x + int32_t(i), y + int32_t(j)) >> p) & 1u) out[base + uint64_t{j} * row + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
      base += row * height;
    }
  }
  WireWriter o = reply(c, depth, static_cast<uint32_t>(bytes.size() / 4));
  o.u32(w ? w->visual : 0);
  o.zeros(20);
  o.bytes(bytes.view());
}

}  // namespace x11
