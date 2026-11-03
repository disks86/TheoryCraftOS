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

// Paths: the figures recorded between BeginPath and EndPath (points in device coordinates), and what can be done with a finished path.
namespace process::win32k::gdi {

constexpr uint32_t kErrorCannotComplete = 1003;

bool path_recording(const Dc& dc) { return dc.path.recording; }

namespace {
// points and types always grow together (a failed allocation leaves both as they were)
void add(Path& path, Point p, uint8_t type) {
  if (!path.points.push_back(p)) return;
  if (!path.types.push_back(type)) path.points.remove_at(path.points.count() - 1);
}
}  // namespace

void path_move(Dc& dc, Point p) {
  Path& path = dc.path;
  add(path, p, kPtMoveTo);
  path.figure_open = true;
  path.have_start = true;
  path.start = p;
}

namespace {
void ensure_figure(Dc& dc) {
  if (dc.path.figure_open) return;
  path_move(dc, dc.path.have_start ? dc.path.start : lp_to_dp(dc, dc.cur_pos));
}
}  // namespace

void path_line(Dc& dc, Point p) {
  ensure_figure(dc);
  add(dc.path, p, kPtLineTo);
}

void path_bezier(Dc& dc, Point c1, Point c2, Point end) {
  ensure_figure(dc);
  add(dc.path, c1, kPtBezierTo);
  add(dc.path, c2, kPtBezierTo);
  add(dc.path, end, kPtBezierTo);
}

void path_close(Dc& dc) {
  Path& path = dc.path;
  if (!path.figure_open || path.types.count() == 0) return;
  path.types[path.types.count() - 1] = uint8_t(path.types[path.types.count() - 1] | kPtCloseFigure);
  path.figure_open = false;
}

// The figures as polylines (beziers flattened): `counts` the points of each, `closed` whether it was closed.
bool path_flatten(const Path& path, List<Point>* pts, List<uint32_t>* counts, List<uint8_t>* closed) {
  uint64_t start = pts->count();
  uint64_t i = 0;
  Point cur;
  auto finish = [&](bool is_closed) {
    const uint64_t n = pts->count() - start;
    if (n > 0) { counts->push_back(uint32_t(n)); closed->push_back(is_closed ? 1 : 0); }
    start = pts->count();
  };
  while (i < path.points.count()) {
    const uint8_t t = path.types[i] & 0x7E;
    if (t == kPtMoveTo) {
      finish(false);
      cur = path.points[i];
      if (!pts->push_back(cur)) return false;
      ++i;
      if (path.types[i - 1] & kPtCloseFigure) finish(true);
    } else if (t == kPtLineTo) {
      cur = path.points[i];
      if (!pts->push_back(cur)) return false;
      if (path.types[i] & kPtCloseFigure) finish(true);
      ++i;
    } else if (t == kPtBezierTo && i + 2 < path.points.count()) {
      flatten_bezier(cur, path.points[i], path.points[i + 1], path.points[i + 2], pts);
      cur = path.points[i + 2];
      if (path.types[i + 2] & kPtCloseFigure) finish(true);
      i += 3;
    } else {
      ++i;
    }
  }
  finish(false);
  return true;
}

namespace {

Dc* finished_path_dc(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return nullptr;
  if (dc->path.recording || !dc->path.valid) { c.set_last_error(kErrorCannotComplete); return nullptr; }
  return dc;
}

void discard(Dc& dc) {
  dc.path.points.clear();
  dc.path.types.clear();
  dc.path.valid = false;
  dc.path.recording = false;
  dc.path.figure_open = false;
  dc.path.have_start = false;
}

uint64_t NtGdiBeginPath(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  discard(*dc);
  dc->path.recording = true;
  return 1;
}

uint64_t NtGdiEndPath(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (!dc->path.recording) return c.fail(kErrorCannotComplete);
  dc->path.recording = false;
  dc->path.valid = true;
  dc->path.figure_open = false;
  return 1;
}

uint64_t NtGdiAbortPath(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  discard(*dc);
  return 1;
}

uint64_t NtGdiCloseFigure(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (!dc->path.recording) return c.fail(kErrorCannotComplete);
  path_close(*dc);
  return 1;
}

struct Flat {
  explicit Flat(allocators::IAllocator& a) : pts(a), counts(a), closed(a) {}
  List<Point> pts;
  List<uint32_t> counts;
  List<uint8_t> closed;
};

uint64_t do_path_paint(Call& c, bool fill, bool stroke) {
  Dc* dc = finished_path_dc(c);
  if (!dc) return 0;
  Flat f(*alloc());
  if (!path_flatten(dc->path, &f.pts, &f.counts, &f.closed)) return 0;
  Painter p(*dc);
  if (p.ok()) {
    if (fill) {
      PatternSource src;
      pattern_of_dc(*dc, &src);
      fill_polygons(p, f.pts.view(), f.counts.view(), dc->s.poly_fill == 2, src);
    }
    if (stroke) {
      uint64_t base = 0;
      for (uint64_t i = 0; i < f.counts.count(); ++i) {
        stroke_polyline(p, f.pts.view().subview(base, f.counts[i]), fill || f.closed[i]);
        base += f.counts[i];
      }
    }
  }
  discard(*dc);
  return 1;
}

uint64_t NtGdiFillPath(Call& c) { return do_path_paint(c, true, false); }
uint64_t NtGdiStrokePath(Call& c) { return do_path_paint(c, false, true); }
uint64_t NtGdiStrokeAndFillPath(Call& c) { return do_path_paint(c, true, true); }

bool path_region(Dc& dc, List<Rect>* out) {
  Flat f(*alloc());
  if (!path_flatten(dc.path, &f.pts, &f.counts, &f.closed)) return false;
  return rgn_polygon(out, f.pts.view(), f.counts.view(), dc.s.poly_fill == 2, Rect{-65536, -65536, 65536, 65536});
}

uint64_t NtGdiPathToRegion(Call& c) {
  Dc* dc = finished_path_dc(c);
  if (!dc) return 0;
  List<Rect> region(*alloc());
  if (!path_region(*dc, &region)) return 0;
  discard(*dc);
  Region* r = new_region(c.pid(), region.view());
  return r ? r->handle : 0;
}

uint64_t NtGdiSelectClipPath(Call& c) {
  Dc* dc = finished_path_dc(c);
  const int32_t mode = c.argi(1);
  if (!dc) return 0;
  if (mode < 1 || mode > 5) return c.fail(kErrorInvalidParameter);
  allocators::IAllocator& al = *alloc();
  List<Rect> region(al), result(al), whole(al);
  if (!path_region(*dc, &region)) return 0;
  discard(*dc);
  if (mode == 5) {
    if (!rgn_assign(result, region.view())) return 0;
  } else {
    if (dc->s.has_clip) { if (!rgn_assign(whole, dc->s.clip.view())) return 0; }
    else if (!whole.push_back(Rect{-0x3FFFFFFF, -0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF})) return 0;
    if (!rgn_combine(whole.view(), region.view(), mode, &result)) return 0;
  }
  dc->s.has_clip = true;
  return ret_bool(rgn_assign(dc->s.clip, result.view()));
}

uint64_t NtGdiFlattenPath(Call& c) {
  Dc* dc = finished_path_dc(c);
  if (!dc) return 0;
  Path flat(*alloc());
  // rebuild: each figure's polyline
  Flat f(*alloc());
  if (!path_flatten(dc->path, &f.pts, &f.counts, &f.closed)) return 0;
  uint64_t base = 0;
  for (uint64_t i = 0; i < f.counts.count(); ++i) {
    for (uint32_t k = 0; k < f.counts[i]; ++k) {
      uint8_t t = k == 0 ? kPtMoveTo : kPtLineTo;
      if (k + 1 == f.counts[i] && f.closed[i]) t = uint8_t(t | kPtCloseFigure);
      add(flat, f.pts[base + k], t);
    }
    base += f.counts[i];
  }
  dc->path.points.clear();
  dc->path.types.clear();
  for (uint64_t i = 0; i < flat.points.count(); ++i) add(dc->path, flat.points[i], flat.types[i]);
  return 1;
}

uint64_t NtGdiWidenPath(Call& c) {
  Dc* dc = finished_path_dc(c);
  if (!dc) return 0;
  if (pen_is_null(*dc)) return c.fail(kErrorCannotComplete);
  Flat f(*alloc());
  if (!path_flatten(dc->path, &f.pts, &f.counts, &f.closed)) return 0;
  List<Rect> region(*alloc()), piece(*alloc()), merged(*alloc());
  uint64_t base = 0;
  for (uint64_t i = 0; i < f.counts.count(); ++i) {
    if (!stroke_to_region(*dc, f.pts.view().subview(base, f.counts[i]), f.closed[i] != 0, &piece)) return 0;
    if (!rgn_combine(region.view(), piece.view(), 2, &merged) || !rgn_assign(region, merged.view())) return 0;
    base += f.counts[i];
  }
  dc->path.points.clear();
  dc->path.types.clear();
  for (const Rect& r : region.view()) {
    add(dc->path, Point{r.left, r.top}, kPtMoveTo);
    add(dc->path, Point{r.right, r.top}, kPtLineTo);
    add(dc->path, Point{r.right, r.bottom}, kPtLineTo);
    add(dc->path, Point{r.left, r.bottom}, uint8_t(kPtLineTo | kPtCloseFigure));
  }
  return 1;
}

uint64_t NtGdiGetPath(Call& c) {
  Dc* dc = finished_path_dc(c);
  if (!dc) return ret_i32(-1);
  const int32_t size = c.argi(3);
  const uint64_t n = dc->path.points.count();
  if (size == 0) return n;
  if (size < 0 || uint64_t(size) < n) { c.set_last_error(kErrorInvalidParameter); return ret_i32(-1); }
  for (uint64_t i = 0; i < n; ++i) {
    const Point lp = dp_to_lp(*dc, dc->path.points[i]);
    if (!c.write_struct(c.arg(1) + i * 8, lp) || !c.write(c.arg(2) + i, ConstByteView(&dc->path.types[i], 1))) return ret_i32(-1);
  }
  return n;
}

}  // namespace

void register_path_services() {
  GDI_REGISTER(BeginPath, NtGdiBeginPath);
  GDI_REGISTER(EndPath, NtGdiEndPath);
  GDI_REGISTER(AbortPath, NtGdiAbortPath);
  GDI_REGISTER(CloseFigure, NtGdiCloseFigure);
  GDI_REGISTER(FillPath, NtGdiFillPath);
  GDI_REGISTER(StrokePath, NtGdiStrokePath);
  GDI_REGISTER(StrokeAndFillPath, NtGdiStrokeAndFillPath);
  GDI_REGISTER(PathToRegion, NtGdiPathToRegion);
  GDI_REGISTER(SelectClipPath, NtGdiSelectClipPath);
  GDI_REGISTER(FlattenPath, NtGdiFlattenPath);
  GDI_REGISTER(WidenPath, NtGdiWidenPath);
  GDI_REGISTER(GetPath, NtGdiGetPath);
}

}  // namespace process::win32k::gdi
