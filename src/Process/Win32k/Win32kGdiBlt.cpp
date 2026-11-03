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

// Block transfers: BitBlt / StretchBlt / MaskBlt / PlgBlt / PatBlt, TransparentBlt, AlphaBlend and GradientFill.
namespace process::win32k::gdi {

namespace {

bool rop_needs_source(uint32_t rop) { const uint32_t t = (rop >> 16) & 0xFF; return ((t & 0xCC) >> 2) != (t & 0x33); }
bool rop_needs_pattern(uint32_t rop) { const uint32_t t = (rop >> 16) & 0xFF; return (t & 0x0F) != (t >> 4); }

int64_t map_index(int64_t d, int64_t dn, int64_t sn, bool flip) {
  int64_t i = (2 * d + 1) * sn / (2 * dn);
  if (i >= sn) i = sn - 1;
  return flip ? sn - 1 - i : i;
}

uint32_t blend_channel(uint32_t s, uint32_t d, uint32_t sa) { return (s * sa + d * (255 - sa) + 127) / 255; }

}  // namespace

bool blit_surface(Dc& dst, const Rect& drect, const Surface& src, const Rect& srect, const BlitParams& params) {
  Painter p(dst);
  if (!p.ok()) return true;
  const Rect nd = rect_normalized(drect), ns = rect_normalized(srect);
  const int64_t dw = int64_t{nd.right} - nd.left, dh = int64_t{nd.bottom} - nd.top, sw = int64_t{ns.right} - ns.left, sh = int64_t{ns.bottom} - ns.top;
  if (dw == 0 || dh == 0) return true;
  const bool use_rop = params.mode == 0;
  const bool needs_src = !use_rop || rop_needs_source(params.rop) || (params.mask != nullptr && rop_needs_source(params.rop_back));
  if (needs_src && (!src.valid() || sw == 0 || sh == 0)) return false;
  const bool flip_x = (drect.right < drect.left) != (srect.right < srect.left), flip_y = (drect.bottom < drect.top) != (srect.bottom < srect.top);
  PatternSource pat;
  pattern_of_dc(dst, &pat);
  const bool pattern_used = use_rop && (rop_needs_pattern(params.rop) || (params.mask && rop_needs_pattern(params.rop_back)));
  const bool pat_solid = !pat.brush || pat.brush->style == 0;
  Surface s2 = src;
  ByteBuffer copy(*alloc());
  if (src.valid() && src.bytes.data() == p.surface().bytes.data()) {      // source and destination are the same pixels: read from a copy
    if (!copy.append(ConstByteView(src.bytes))) return false;
    s2.bytes = copy.writable_view();
  }
  const bool mono_src = s2.bpp == 1 && s2.colors.size() >= 2;
  Surface& d = p.surface();
  for (const Rect& cr : p.clip().view()) {
    const Rect q = rect_and(Rect{sat_add(nd.left, p.ox()), sat_add(nd.top, p.oy()), sat_add(nd.right, p.ox()), sat_add(nd.bottom, p.oy())}, cr);
    if (rect_empty(q)) continue;
    for (int32_t y = q.top; y < q.bottom; ++y) {
      const int64_t dy = int64_t{y} - p.oy() - nd.top;
      const int64_t sy = needs_src ? ns.top + map_index(dy, dh, sh, flip_y) : 0;
      for (int32_t x = q.left; x < q.right; ++x) {
        const int64_t dx = int64_t{x} - p.ox() - nd.left;
        ColorRef s = 0;
        if (needs_src) {
          const int64_t sx = ns.left + map_index(dx, dw, sw, flip_x);
          if (sx < 0 || sy < 0 || sx >= s2.width || sy >= s2.height) continue;
          s = s2.get(int32_t(sx), int32_t(sy));
          if (use_rop && mono_src) s = (s & 0xFFFFFF) != (s2.colors[0] & 0xFFFFFF) ? params.back : params.text;
        }
        const ColorRef dcol = d.get(x, y);
        ColorRef out;
        if (params.mode == 1) {
          const uint32_t ca = (params.blend >> 16) & 0xFF;
          uint32_t sa = ca;
          uint32_t sr = cr_r(s), sg = cr_g(s), sb = cr_b(s);
          if (params.blend >> 24 & 1) {
            const uint32_t pa = s2.get_raw32(int32_t(ns.left + map_index(dx, dw, sw, flip_x)), int32_t(sy)) >> 24;
            sa = pa * ca / 255;
            sr = sr * ca / 255; sg = sg * ca / 255; sb = sb * ca / 255;
            out = make_rgb(sr + cr_r(dcol) * (255 - sa) / 255, sg + cr_g(dcol) * (255 - sa) / 255, sb + cr_b(dcol) * (255 - sa) / 255);
          } else {
            out = make_rgb(blend_channel(sr, cr_r(dcol), sa), blend_channel(sg, cr_g(dcol), sa), blend_channel(sb, cr_b(dcol), sa));
          }
        } else if (params.mode == 2) {
          if ((s & 0xFFFFFF) == (params.key & 0xFFFFFF)) continue;
          out = s;
        } else {
          uint32_t rop = params.rop;
          if (params.mask) {
            const int64_t mx = int64_t{params.mask_x} + dx, my = int64_t{params.mask_y} + dy;
            const bool set = mx >= 0 && my >= 0 && mx < params.mask->width && my < params.mask->height && (params.mask->get(int32_t(mx), int32_t(my)) & 0xFFFFFF) != (params.mask->colors.size() ? params.mask->colors[0] & 0xFFFFFF : 0);
            if (!set) rop = params.rop_back;
          }
          bool transparent = false;
          const ColorRef pc = !pattern_used ? 0 : pat_solid ? pat.solid : pat.at(x - p.ox(), y - p.oy(), &transparent);
          out = rop3_apply(rop, transparent ? pat.back : pc, s, dcol);
        }
        d.put(x, y, out);
      }
    }
    p.mark(Rect{q.left - p.ox(), q.top - p.oy(), q.right - p.ox(), q.bottom - p.oy()});
  }
  return true;
}

bool stretch_surface(Dc& dst, const Rect& drect, const Surface& src, const Rect& srect, uint32_t rop, ColorRef text, ColorRef back) {
  BlitParams bp;
  bp.rop = rop;
  bp.text = text & 0xFFFFFF;
  bp.back = back & 0xFFFFFF;
  return blit_surface(dst, drect, src, srect, bp);
}

namespace {

struct Source {
  Dc* dc = nullptr;
  Surface s;
  Bitmap* bm = nullptr;
};

// The surface of a source DC; false if the handle is not a DC with pixels.
bool source_dc(Call& c, uint32_t handle, Source* out) {
  out->dc = dc_arg(c, handle);
  if (!out->dc) return false;
  out->s = dc_surface(*out->dc, &out->bm);
  if (out->bm && out->bm->dib) dib_sync_in(*out->bm);
  return out->s.valid();
}

Rect shifted(const Rect& r, int32_t dx, int32_t dy) { return Rect{sat_add(r.left, dx), sat_add(r.top, dy), sat_add(r.right, dx), sat_add(r.bottom, dy)}; }

uint64_t NtGdiBitBlt(Call& c) {
  Dc* dst = dc_arg(c, c.arg32(0));
  if (!dst) return 0;
  const int32_t x = c.argi(1), y = c.argi(2), w = c.argi(3), h = c.argi(4), xs = c.argi(6), ys = c.argi(7);
  const uint32_t rop = c.arg32(8);
  const Rect dr = lp_to_dp_rect(*dst, Rect{x, y, sat_add(x, w), sat_add(y, h)});
  Rect sr;
  Surface empty;
  if (rop_needs_source(rop)) {
    Source src;
    if (c.arg32(5) == 0) return c.fail(kErrorInvalidHandle);
    if (!source_dc(c, c.arg32(5), &src)) return 0;
    const Point ps = lp_to_dp(*src.dc, Point{xs, ys});
    sr = shifted(Rect{ps.x, ps.y, sat_add(ps.x, dr.right - dr.left), sat_add(ps.y, dr.bottom - dr.top)}, src.dc->origin_x, src.dc->origin_y);
    return ret_bool(stretch_surface(*dst, dr, src.s, sr, rop, dst->s.text_color, dst->s.bk_color));
  }
  return ret_bool(stretch_surface(*dst, dr, empty, Rect(), rop, dst->s.text_color, dst->s.bk_color));
}

uint64_t NtGdiStretchBlt(Call& c) {
  Dc* dst = dc_arg(c, c.arg32(0));
  if (!dst) return 0;
  const uint32_t rop = c.arg32(10);
  const Point da = lp_to_dp(*dst, Point{c.argi(1), c.argi(2)}), db = lp_to_dp(*dst, Point{sat_add(c.argi(1), c.argi(3)), sat_add(c.argi(2), c.argi(4))});
  const Rect dr{da.x, da.y, db.x, db.y};
  Surface empty;
  if (!rop_needs_source(rop)) return ret_bool(stretch_surface(*dst, dr, empty, Rect(), rop, dst->s.text_color, dst->s.bk_color));
  Source src;
  if (c.arg32(5) == 0) return c.fail(kErrorInvalidHandle);
  if (!source_dc(c, c.arg32(5), &src)) return 0;
  const Point sa = lp_to_dp(*src.dc, Point{c.argi(6), c.argi(7)}), sb = lp_to_dp(*src.dc, Point{sat_add(c.argi(6), c.argi(8)), sat_add(c.argi(7), c.argi(9))});
  const Rect sr = shifted(Rect{sa.x, sa.y, sb.x, sb.y}, src.dc->origin_x, src.dc->origin_y);
  return ret_bool(stretch_surface(*dst, dr, src.s, sr, rop, dst->s.text_color, dst->s.bk_color));
}

uint64_t NtGdiPatBlt(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t rop = c.arg32(5);
  if (!dc) return 0;
  if (rop_needs_source(rop)) return c.fail(kErrorInvalidParameter);
  Painter p(*dc);
  if (!p.ok()) return 1;
  PatternSource src;
  pattern_of_dc(*dc, &src);
  const Rect r = lp_to_dp_rect(*dc, Rect{c.argi(1), c.argi(2), sat_add(c.argi(1), c.argi(3)), sat_add(c.argi(2), c.argi(4))});
  fill_rect_pattern(p, r, src, rop);
  return 1;
}

uint64_t NtGdiPolyPatBlt(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t rop = c.arg32(1), count = c.arg32(3);
  if (!dc || count > (1u << 16) || rop_needs_source(rop)) return 0;
  Painter p(*dc);
  if (!p.ok()) return 1;
  for (uint32_t i = 0; i < count; ++i) {
    int32_t v[4];
    uint32_t brush = 0;
    if (!c.read_array(c.arg(2) + uint64_t{i} * 24, v, 4) || !c.read_u32(c.arg(2) + uint64_t{i} * 24 + 16, &brush)) return 0;
    PatternSource src;
    pattern_of_brush(*dc, brush ? find_brush(brush) : find_brush(dc->s.brush), &src);
    const Rect r = lp_to_dp_rect(*dc, Rect{v[0], v[1], sat_add(v[0], v[2]), sat_add(v[1], v[3])});
    fill_rect_pattern(p, r, src, rop);
  }
  return 1;
}

uint64_t NtGdiMaskBlt(Call& c) {
  Dc* dst = dc_arg(c, c.arg32(0));
  if (!dst) return 0;
  const uint32_t rop = c.arg32(11);
  Bitmap* mask_bm = c.arg32(8) ? find_bitmap(c.arg32(8)) : nullptr;
  if (c.arg32(8) && (!mask_bm || mask_bm->bpp != 1)) return c.fail(kErrorInvalidParameter);
  const Point da = lp_to_dp(*dst, Point{c.argi(1), c.argi(2)}), db = lp_to_dp(*dst, Point{sat_add(c.argi(1), c.argi(3)), sat_add(c.argi(2), c.argi(4))});
  const Rect dr{da.x, da.y, db.x, db.y};
  BlitParams bp;
  bp.rop = (rop & 0x00FF0000);
  bp.rop_back = (rop >> 8) & 0x00FF0000;
  bp.text = dst->s.text_color & 0xFFFFFF;
  bp.back = dst->s.bk_color & 0xFFFFFF;
  Surface mask;
  if (mask_bm) { mask = surface_of(*mask_bm); bp.mask = &mask; bp.mask_x = c.argi(9); bp.mask_y = c.argi(10); }
  else bp.rop_back = bp.rop;
  Surface none;
  if (!rop_needs_source(bp.rop) && !rop_needs_source(bp.rop_back)) return ret_bool(blit_surface(*dst, dr, none, Rect(), bp));
  Source src;
  if (c.arg32(5) == 0) return c.fail(kErrorInvalidHandle);
  if (!source_dc(c, c.arg32(5), &src)) return 0;
  const Point ps = lp_to_dp(*src.dc, Point{c.argi(6), c.argi(7)});
  const Rect sr = shifted(Rect{ps.x, ps.y, sat_add(ps.x, db.x - da.x), sat_add(ps.y, db.y - da.y)}, src.dc->origin_x, src.dc->origin_y);
  return ret_bool(blit_surface(*dst, dr, src.s, sr, bp));
}

uint64_t NtGdiTransparentBlt(Call& c) {
  Dc* dst = dc_arg(c, c.arg32(0));
  Source src;
  if (!dst || !source_dc(c, c.arg32(5), &src)) return 0;
  const Point da = lp_to_dp(*dst, Point{c.argi(1), c.argi(2)}), db = lp_to_dp(*dst, Point{sat_add(c.argi(1), c.argi(3)), sat_add(c.argi(2), c.argi(4))});
  const Point sa = lp_to_dp(*src.dc, Point{c.argi(6), c.argi(7)}), sb = lp_to_dp(*src.dc, Point{sat_add(c.argi(6), c.argi(8)), sat_add(c.argi(7), c.argi(9))});
  BlitParams bp;
  bp.mode = 2;
  bp.key = resolve_color(*dst, c.arg32(10));
  return ret_bool(blit_surface(*dst, Rect{da.x, da.y, db.x, db.y}, src.s, shifted(Rect{sa.x, sa.y, sb.x, sb.y}, src.dc->origin_x, src.dc->origin_y), bp));
}

uint64_t NtGdiAlphaBlend(Call& c) {
  Dc* dst = dc_arg(c, c.arg32(0));
  Source src;
  if (!dst || !source_dc(c, c.arg32(5), &src)) return 0;
  const uint32_t fn = c.arg32(10);
  if ((fn & 0xFF) != 0 || ((fn >> 24) & 0xFE) != 0) return c.fail(kErrorInvalidParameter);
  const Point da = lp_to_dp(*dst, Point{c.argi(1), c.argi(2)}), db = lp_to_dp(*dst, Point{sat_add(c.argi(1), c.argi(3)), sat_add(c.argi(2), c.argi(4))});
  const Point sa = lp_to_dp(*src.dc, Point{c.argi(6), c.argi(7)}), sb = lp_to_dp(*src.dc, Point{sat_add(c.argi(6), c.argi(8)), sat_add(c.argi(7), c.argi(9))});
  if (src.s.bpp != 32 && (fn >> 24 & 1)) return c.fail(kErrorInvalidParameter);
  BlitParams bp;
  bp.mode = 1;
  bp.blend = fn;
  return ret_bool(blit_surface(*dst, Rect{da.x, da.y, db.x, db.y}, src.s, shifted(Rect{sa.x, sa.y, sb.x, sb.y}, src.dc->origin_x, src.dc->origin_y), bp));
}

struct Vertex { int32_t x, y; uint16_t r, g, b, a; };

ColorRef lerp_color(const Vertex& a, const Vertex& b, double t) {
  auto ch = [&](uint16_t x, uint16_t y) { const double v = (x + (double(y) - x) * t) / 257.0; return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); };
  return make_rgb(ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b));
}

uint64_t NtGdiGradientFill(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t nvert = c.arg32(2), ngrad = c.arg32(4), mode = c.arg32(5);
  if (!dc || nvert == 0 || nvert > 65536 || ngrad == 0 || ngrad > 65536 || mode > 2) return c.fail(kErrorInvalidParameter);
  List<Vertex> v(*alloc());
  for (uint32_t i = 0; i < nvert; ++i) {
    Vertex t;
    if (!c.read_struct(c.arg(1) + uint64_t{i} * 16, &t)) return 0;
    v.push_back(t);
  }
  Painter p(*dc);
  for (uint32_t g = 0; g < ngrad; ++g) {
    uint32_t idx[3] = {};
    const uint32_t per = mode == 2 ? 3 : 2;
    for (uint32_t k = 0; k < per; ++k) if (!c.read_u32(c.arg(3) + (uint64_t{g} * per + k) * 4, &idx[k]) || idx[k] >= nvert) return c.fail(kErrorInvalidParameter);
    if (!p.ok()) continue;
    if (mode < 2) {
      const Vertex &ul = v[idx[0]], &lr = v[idx[1]];
      const Point a = lp_to_dp(*dc, Point{ul.x, ul.y}), b = lp_to_dp(*dc, Point{lr.x, lr.y});
      const Rect r = rect_normalized(Rect{a.x, a.y, b.x, b.y});
      for (int32_t y = r.top; y < r.bottom; ++y) {
        for (int32_t x = r.left; x < r.right; ++x) {
          const double t = mode == 0 ? (r.right - r.left > 1 ? double(x - r.left) / (r.right - r.left - 1) : 0) : (r.bottom - r.top > 1 ? double(y - r.top) / (r.bottom - r.top - 1) : 0);
          p.plot_raw(x, y, lerp_color(ul, lr, t));
        }
      }
    } else {
      Point q[3];
      for (int k = 0; k < 3; ++k) q[k] = lp_to_dp(*dc, Point{v[idx[k]].x, v[idx[k]].y});
      Rect bb{q[0].x, q[0].y, q[0].x + 1, q[0].y + 1};
      for (int k = 1; k < 3; ++k) bb = rect_union(bb, Rect{q[k].x, q[k].y, q[k].x + 1, q[k].y + 1});
      const double den = double(q[1].y - q[2].y) * (q[0].x - q[2].x) + double(q[2].x - q[1].x) * (q[0].y - q[2].y);
      if (den == 0) continue;
      for (int32_t y = bb.top; y < bb.bottom; ++y) {
        for (int32_t x = bb.left; x < bb.right; ++x) {
          const double px = x + 0.5, py = y + 0.5;
          const double l0 = (double(q[1].y - q[2].y) * (px - q[2].x) + double(q[2].x - q[1].x) * (py - q[2].y)) / den;
          const double l1 = (double(q[2].y - q[0].y) * (px - q[2].x) + double(q[0].x - q[2].x) * (py - q[2].y)) / den;
          const double l2 = 1 - l0 - l1;
          if (l0 < 0 || l1 < 0 || l2 < 0) continue;
          auto ch = [&](uint16_t a, uint16_t b, uint16_t cc) { const double val = (l0 * a + l1 * b + l2 * cc) / 257.0; return uint32_t(val < 0 ? 0 : val > 255 ? 255 : val + 0.5); };
          const Vertex &a = v[idx[0]], &b = v[idx[1]], &cc = v[idx[2]];
          p.plot_raw(x, y, make_rgb(ch(a.r, b.r, cc.r), ch(a.g, b.g, cc.g), ch(a.b, b.b, cc.b)));
        }
      }
    }
  }
  return 1;
}

uint64_t NtGdiPlgBlt(Call& c) {
  Dc* dst = dc_arg(c, c.arg32(0));
  Source src;
  if (!dst || !source_dc(c, c.arg32(2), &src)) return 0;
  Point pts[3];
  for (int i = 0; i < 3; ++i) if (!c.read_struct(c.arg(1) + uint64_t(i) * 8, &pts[i])) return 0;
  const int32_t xs = c.argi(3), ys = c.argi(4), w = c.argi(5), h = c.argi(6);
  if (w <= 0 || h <= 0) return 0;
  Bitmap* mask_bm = c.arg32(7) ? find_bitmap(c.arg32(7)) : nullptr;
  if (c.arg32(7) && (!mask_bm || mask_bm->bpp != 1)) return c.fail(kErrorInvalidParameter);
  Surface mask;
  if (mask_bm) mask = surface_of(*mask_bm);
  const int32_t xm = c.argi(8), ym = c.argi(9);
  Point d[3];
  for (int i = 0; i < 3; ++i) d[i] = lp_to_dp(*dst, pts[i]);
  // dest(u, v) = d0 + u * (d1 - d0) + v * (d2 - d0), u and v in [0, 1) over the source rectangle
  const double ux = double(d[1].x) - d[0].x, uy = double(d[1].y) - d[0].y, vx = double(d[2].x) - d[0].x, vy = double(d[2].y) - d[0].y;
  const double det = ux * vy - uy * vx;
  if (det == 0) return 0;
  const Point s0 = lp_to_dp(*src.dc, Point{xs, ys});
  Painter p(*dst);
  if (!p.ok()) return 1;
  Rect bb{d[0].x, d[0].y, d[0].x + 1, d[0].y + 1};
  const Point d3{d[1].x + d[2].x - d[0].x, d[1].y + d[2].y - d[0].y};
  const Point corners[3] = {d[1], d[2], d3};
  for (const Point& q : corners) bb = rect_union(bb, Rect{q.x, q.y, q.x + 1, q.y + 1});
  if (int64_t{bb.right} - bb.left > 16384 || int64_t{bb.bottom} - bb.top > 16384) return 0;
  for (int32_t y = bb.top; y < bb.bottom; ++y) {
    for (int32_t x = bb.left; x < bb.right; ++x) {
      const double px = x + 0.5 - d[0].x, py = y + 0.5 - d[0].y;
      const double u = (px * vy - py * vx) / det, v = (ux * py - uy * px) / det;
      if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
      const int32_t sx = int32_t(u * w), sy = int32_t(v * h);
      if (mask_bm) {
        const int32_t mx = xm + sx, my = ym + sy;
        if (mx < 0 || my < 0 || mx >= mask.width || my >= mask.height || (mask.get(mx, my) & 0xFFFFFF) == (mask.colors.size() ? mask.colors[0] & 0xFFFFFF : 0)) continue;
      }
      const int32_t fx = s0.x + sx + src.dc->origin_x, fy = s0.y + sy + src.dc->origin_y;
      if (fx < 0 || fy < 0 || fx >= src.s.width || fy >= src.s.height) continue;
      p.plot_raw(x, y, src.s.get(fx, fy));
    }
  }
  return 1;
}

}  // namespace

void register_blt_services() {
  GDI_REGISTER(BitBlt, NtGdiBitBlt);
  GDI_REGISTER(StretchBlt, NtGdiStretchBlt);
  GDI_REGISTER(PatBlt, NtGdiPatBlt);
  GDI_REGISTER(PolyPatBlt, NtGdiPolyPatBlt);
  GDI_REGISTER(MaskBlt, NtGdiMaskBlt);
  GDI_REGISTER(TransparentBlt, NtGdiTransparentBlt);
  GDI_REGISTER(AlphaBlend, NtGdiAlphaBlend);
  GDI_REGISTER(GradientFill, NtGdiGradientFill);
  GDI_REGISTER(PlgBlt, NtGdiPlgBlt);
}

}  // namespace process::win32k::gdi
