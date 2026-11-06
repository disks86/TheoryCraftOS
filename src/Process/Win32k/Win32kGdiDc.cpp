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

// Device contexts: creation and state, the Get/Set families, coordinate mapping, world transforms, clipping, device capabilities.
namespace process::win32k::gdi {

namespace {

constexpr int32_t kInfinite = 0x3FFFFFFF;

int32_t scale_round(int64_t v, int64_t num, int64_t den) {
  if (den == 0) return clamp_i32(v);
  if (den < 0) { num = -num; den = -den; }
  const int64_t n = v * num;      // |v| < 2^32 and |num| <= 2^31: no overflow
  const int64_t half = den / 2;
  return clamp_i32((n >= 0 ? n + half : n - half) / den);
}

bool identity(const Xform& x) { return x.m11 == 1 && x.m12 == 0 && x.m21 == 0 && x.m22 == 1 && x.dx == 0 && x.dy == 0; }

Xform multiply(const Xform& a, const Xform& b) {      // apply a first, then b
  Xform r;
  r.m11 = a.m11 * b.m11 + a.m12 * b.m21;
  r.m12 = a.m11 * b.m12 + a.m12 * b.m22;
  r.m21 = a.m21 * b.m11 + a.m22 * b.m21;
  r.m22 = a.m21 * b.m12 + a.m22 * b.m22;
  r.dx = a.dx * b.m11 + a.dy * b.m21 + b.dx;
  r.dy = a.dx * b.m12 + a.dy * b.m22 + b.dy;
  return r;
}

void set_mapping_extents(DcPod& s, int32_t mode) {
  switch (mode) {
    case 1: s.wnd_ext = Size{1, 1}; s.vport_ext = Size{1, 1}; break;
    case 2: s.wnd_ext = Size{254, 254}; s.vport_ext = Size{96, -96}; break;
    case 3: s.wnd_ext = Size{2540, 2540}; s.vport_ext = Size{96, -96}; break;
    case 4: s.wnd_ext = Size{100, 100}; s.vport_ext = Size{96, -96}; break;
    case 5: s.wnd_ext = Size{1000, 1000}; s.vport_ext = Size{96, -96}; break;
    case 6: s.wnd_ext = Size{1440, 1440}; s.vport_ext = Size{96, -96}; break;
    default: break;
  }
}

}  // namespace

bool DcState::copy_from(const DcState& o) {
  static_cast<DcPod&>(*this) = static_cast<const DcPod&>(o);
  clip.clear();
  meta.clear();
  for (const Rect& r : o.clip.view()) if (!clip.push_back(r)) return false;
  for (const Rect& r : o.meta.view()) if (!meta.push_back(r)) return false;
  return true;
}

bool xform_invert(const Xform& x, Xform* out) {
  const double det = x.m11 * x.m22 - x.m12 * x.m21;
  if (det < 1e-12 && det > -1e-12) return false;
  out->m11 = x.m22 / det;
  out->m12 = -x.m12 / det;
  out->m21 = -x.m21 / det;
  out->m22 = x.m11 / det;
  out->dx = (x.m21 * x.dy - x.m22 * x.dx) / det;
  out->dy = (x.m12 * x.dx - x.m11 * x.dy) / det;
  return true;
}

bool dc_world_active(const Dc& dc) { return !identity(dc.s.world); }
bool dc_is_axis_aligned(const Dc& dc) { return !dc_world_active(dc) || (dc.s.world.m12 == 0 && dc.s.world.m21 == 0); }

DPoint lp_to_dp_f(const Dc& dc, double x, double y) {
  const DcPod& s = dc.s;
  if (dc_world_active(dc)) {
    const double nx = x * s.world.m11 + y * s.world.m21 + s.world.dx, ny = x * s.world.m12 + y * s.world.m22 + s.world.dy;
    x = nx;
    y = ny;
  }
  DPoint r;
  r.x = (x - s.wnd_org.x) * s.vport_ext.cx / (s.wnd_ext.cx ? s.wnd_ext.cx : 1) + s.vport_org.x;
  r.y = (y - s.wnd_org.y) * s.vport_ext.cy / (s.wnd_ext.cy ? s.wnd_ext.cy : 1) + s.vport_org.y;
  return r;
}

DPoint dp_to_lp_f(const Dc& dc, double x, double y) {
  const DcPod& s = dc.s;
  x = (x - s.vport_org.x) * s.wnd_ext.cx / (s.vport_ext.cx ? s.vport_ext.cx : 1) + s.wnd_org.x;
  y = (y - s.vport_org.y) * s.wnd_ext.cy / (s.vport_ext.cy ? s.vport_ext.cy : 1) + s.wnd_org.y;
  Xform inv;
  if (dc_world_active(dc) && xform_invert(s.world, &inv)) {
    const double nx = x * inv.m11 + y * inv.m21 + inv.dx, ny = x * inv.m12 + y * inv.m22 + inv.dy;
    x = nx;
    y = ny;
  }
  return DPoint{x, y};
}

Point lp_to_dp(const Dc& dc, Point p) {
  const DcPod& s = dc.s;
  if (!dc_world_active(dc)) {
    return Point{sat_add(scale_round(int64_t{p.x} - s.wnd_org.x, s.vport_ext.cx, s.wnd_ext.cx ? s.wnd_ext.cx : 1), s.vport_org.x),
                 sat_add(scale_round(int64_t{p.y} - s.wnd_org.y, s.vport_ext.cy, s.wnd_ext.cy ? s.wnd_ext.cy : 1), s.vport_org.y)};
  }
  const DPoint d = lp_to_dp_f(dc, p.x, p.y);
  return Point{round_i32(d.x), round_i32(d.y)};
}

Point dp_to_lp(const Dc& dc, Point p) {
  const DcPod& s = dc.s;
  if (!dc_world_active(dc)) {
    return Point{sat_add(scale_round(int64_t{p.x} - s.vport_org.x, s.wnd_ext.cx, s.vport_ext.cx ? s.vport_ext.cx : 1), s.wnd_org.x),
                 sat_add(scale_round(int64_t{p.y} - s.vport_org.y, s.wnd_ext.cy, s.vport_ext.cy ? s.vport_ext.cy : 1), s.wnd_org.y)};
  }
  const DPoint d = dp_to_lp_f(dc, p.x, p.y);
  return Point{round_i32(d.x), round_i32(d.y)};
}

Rect lp_to_dp_rect(const Dc& dc, const Rect& r) {
  const Point a = lp_to_dp(dc, Point{r.left, r.top}), b = lp_to_dp(dc, Point{r.right, r.bottom});
  return rect_normalized(Rect{a.x, a.y, b.x, b.y});
}

void dc_apply_default_state(Dc& dc) {
  // keeps the clip lists (cleared) and the allocator-bound members
  const DcPod fresh;
  static_cast<DcPod&>(dc.s) = fresh;
  dc.s.clip.clear();
  dc.s.meta.clear();
  dc.s.pen = stock_handle(7);
  dc.s.brush = stock_handle(0);
  dc.s.font = stock_handle(13);
  dc.s.palette = stock_handle(15);
}

Dc* new_dc(uint64_t pid) {
  Dc* dc = make_object<Dc>(kObjDc, pid);
  if (!dc) return nullptr;
  dc_apply_default_state(*dc);
  return dc;
}

void dc_release_selection(Dc& dc) {
  const uint32_t hs[5] = {dc.s.pen, dc.s.brush, dc.s.font, dc.s.palette, dc.bitmap};
  for (uint32_t h : hs) if (Object* o = find_object(h)) if (o->selected > 0) --o->selected;
  dc.s.pen = dc.s.brush = dc.s.font = dc.s.palette = dc.bitmap = 0;
}

Surface dc_surface(Dc& dc, Bitmap** bitmap) {
  *bitmap = nullptr;
  if (dc.screen && dc.surface) {      // a window with a window surface draws there (nowhere once the surface is gone)
    Bitmap* b = window_surface_bitmap(dc.surface);
    if (!b) return Surface();
    *bitmap = b;
    return surface_of(*b);
  }
  if (dc.screen) {
    Bitmap& b = screen_bitmap();
    *bitmap = &b;
    return surface_of(b);
  }
  if (dc.memory) {
    Bitmap* b = find_bitmap(dc.bitmap);
    if (b) { *bitmap = b; return surface_of(*b); }
  }
  return Surface();
}

namespace {

bool translate(ArrayView<const Rect> in, int32_t dx, int32_t dy, List<Rect>* out) {
  out->clear();
  for (const Rect& r : in) if (!out->push_back(Rect{sat_add(r.left, dx), sat_add(r.top, dy), sat_add(r.right, dx), sat_add(r.bottom, dy)})) return false;
  return true;
}

}  // namespace

bool dc_effective_clip(Dc& dc, const Surface& s, List<Rect>* out) {
  allocators::IAllocator* al = alloc();
  if (!al) return false;
  List<Rect> base(*al), tmp(*al), t2(*al);
  const Rect bounds = s.bounds();
  if (dc.screen) {
    for (const Rect& r : dc.surface_clip.view()) { const Rect c = rect_and(r, bounds); if (!rect_empty(c) && !base.push_back(c)) return false; }
  } else if (!base.push_back(bounds)) {
    return false;
  }
  if (dc.s.has_clip) {
    if (!translate(dc.s.clip.view(), dc.origin_x, dc.origin_y, &t2) || !rgn_combine(base.view(), t2.view(), 1, &tmp)) return false;
    if (!rgn_assign(base, tmp.view())) return false;
  }
  if (dc.s.has_meta) {
    if (!translate(dc.s.meta.view(), dc.origin_x, dc.origin_y, &t2) || !rgn_combine(base.view(), t2.view(), 1, &tmp)) return false;
    if (!rgn_assign(base, tmp.view())) return false;
  }
  out->clear();
  for (const Rect& r : base.view()) if (!out->push_back(r)) return false;
  return true;
}

namespace {

// The visible region of a DC in its own device coordinates.
bool clip_device(Dc& dc, List<Rect>* out) {
  Bitmap* bm = nullptr;
  Surface s = dc_surface(dc, &bm);
  List<Rect> eff(*alloc());
  if (!s.valid()) {      // an information DC has no surface: everything is visible
    out->clear();
    return out->push_back(Rect{-kInfinite, -kInfinite, kInfinite, kInfinite});
  }
  if (!dc_effective_clip(dc, s, &eff)) return false;
  return translate(eff.view(), -dc.origin_x, -dc.origin_y, out);
}

uint64_t ret_region(const List<Rect>& r) { return uint64_t(region_complexity(r.view())); }

// ---- Creation.

uint64_t NtGdiCreateCompatibleDC(Call& c) {
  if (c.arg32(0) && !dc_arg(c, c.arg32(0))) return 0;
  Dc* dc = new_dc(c.pid());
  if (!dc) return c.fail(kErrorNotEnoughMemory);
  dc->memory = true;
  dc->bitmap = default_bitmap_handle();
  if (Object* o = find_object(dc->bitmap)) ++o->selected;
  return dc->handle;
}

uint64_t NtGdiOpenDCW(Call& c) {
  WideString name(*alloc());
  bool display = c.arg_bool(4);
  if (c.arg(0) != 0 && !c.read_unicode_string(c.arg(0), &name)) return c.fail(kErrorInvalidParameter);
  static const char kDisplay[] = "DISPLAY";
  if (name.empty()) display = true;
  else {
    uint64_t i = 0;
    while (i < name.size() && (name[i] == u'\\' || name[i] == u'.')) ++i;
    bool same = name.size() - i >= 7;
    for (uint32_t k = 0; same && k < 7; ++k) same = WideString::fold(name[i + k], true) == char16_t(kDisplay[k]);
    if (same) display = true;
  }
  Dc* dc = new_dc(c.pid());
  if (!dc) return c.fail(kErrorNotEnoughMemory);
  if (display) {
    dc->screen = true;
    dc->surface_clip.push_back(Rect{0, 0, int32_t(screen_width()), int32_t(screen_height())});
  } else {
    dc->printer = true;
    dc->info = true;
    dc->device_kind = 1;
  }
  return dc->handle;
}

uint64_t NtGdiMakeInfoDC(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  dc->info = c.arg_bool(1);
  return 1;
}

uint64_t NtGdiResetDC(Call& c) { return dc_arg(c, c.arg32(0)) ? 1 : 0; }
uint64_t NtGdiCancelDC(Call& c) { return dc_arg(c, c.arg32(0)) ? 1 : 0; }
uint64_t NtGdiCombineTransform(Call& c) {
  float a[6], b[6];
  if (!c.read_array(c.arg(1), a, 6) || !c.read_array(c.arg(2), b, 6)) return 0;
  const Xform r = multiply(Xform{a[0], a[1], a[2], a[3], a[4], a[5]}, Xform{b[0], b[1], b[2], b[3], b[4], b[5]});
  const float out[6] = {float(r.m11), float(r.m12), float(r.m21), float(r.m22), float(r.dx), float(r.dy)};
  return ret_bool(c.write_array(c.arg(0), out, 6));
}

uint64_t NtGdiGetDCforBitmap(Call& c) {
  Bitmap* b = find_bitmap(c.arg32(0));
  if (!b || (!b->stock && b->pid != c.pid())) return c.fail(kErrorInvalidHandle);
  Dc* dc = find_dc_with_bitmap(b->handle);
  return dc ? dc->handle : 0;
}

uint64_t NtGdiComputeXformCoefficients(Call& c) { return dc_arg(c, c.arg32(0)) ? 1 : 0; }

// ---- Save / restore.

bool reselect(Dc& dc, uint32_t* slot, uint32_t wanted, uint8_t type, int fallback_stock) {
  Object* o = find_object(wanted, type);
  if (!o) {
    wanted = fallback_stock >= 0 ? stock_handle(fallback_stock) : 0;
    o = find_object(wanted);
  }
  if (*slot == wanted) return true;
  if (Object* old = find_object(*slot)) if (old->selected > 0) --old->selected;
  *slot = wanted;
  if (o) ++o->selected;
  (void)dc;
  return true;
}

uint64_t NtGdiSaveDC(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (dc->saved.count() >= 8192) return c.fail(kErrorNotEnoughMemory);
  UniquePointer<DcState> st = UniquePointer<DcState>::make(*alloc(), *alloc());
  if (!st || !st->copy_from(dc->s) || !dc->saved.push_back(util::move(st))) return c.fail(kErrorNotEnoughMemory);
  return dc->saved.count();
}

uint64_t NtGdiRestoreDC(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  int64_t level = c.argi(1);
  const int64_t n = int64_t(dc->saved.count());
  if (level < 0) level = n + level + 1;
  if (level < 1 || level > n) return c.fail(kErrorInvalidParameter);
  DcState target(*alloc());
  if (!target.copy_from(*dc->saved[uint64_t(level - 1)])) return c.fail(kErrorNotEnoughMemory);
  while (int64_t(dc->saved.count()) >= level) dc->saved.remove_at(dc->saved.count() - 1);
  const uint32_t pen = target.pen, brush = target.brush, font = target.font, pal = target.palette;
  uint32_t cur_pen = dc->s.pen, cur_brush = dc->s.brush, cur_font = dc->s.font, cur_pal = dc->s.palette;
  if (!dc->s.copy_from(target)) return 0;
  dc->s.pen = cur_pen; dc->s.brush = cur_brush; dc->s.font = cur_font; dc->s.palette = cur_pal;
  reselect(*dc, &dc->s.pen, pen, kObjPen, 7);
  reselect(*dc, &dc->s.brush, brush, kObjBrush, 0);
  reselect(*dc, &dc->s.font, font, kObjFont, 13);
  reselect(*dc, &dc->s.palette, pal, kObjPalette, 15);
  return 1;
}

// ---- The attribute families.

enum : uint32_t {
  kSetMapMode = 8, kSetBkColor = 100, kSetBkMode, kSetTextColor, kSetDCBrushColor, kSetDCPenColor, kSetGraphicsMode, kSetROP2, kSetTextAlign,
  // not in Wine's header: the settings Wine's gdi32 keeps in the shared DC_ATTR, which this kernel has to be told about
  kSetPolyFillMode, kSetStretchBltMode, kSetArcDirection, kSetRelAbs, kSetCharExtra,
  kSetWndOrgX = 120, kSetWndOrgY, kSetWndExtX, kSetWndExtY, kSetVportOrgX, kSetVportOrgY, kSetVportExtX, kSetVportExtY
};

uint64_t NtGdiGetAndSetDCDword(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const uint32_t method = c.arg32(1), value = c.arg32(2);
  DcPod& s = dc->s;
  uint32_t prev = 0;
  switch (method) {
    case kSetMapMode:
      if (value < 1 || value > 8) return c.fail(kErrorInvalidParameter);
      prev = uint32_t(s.map_mode);
      s.map_mode = int32_t(value);
      set_mapping_extents(s, int32_t(value));
      break;
    case kSetBkColor: prev = s.bk_color; s.bk_color = value; break;
    case kSetBkMode: if (value != 1 && value != 2) return c.fail(kErrorInvalidParameter); prev = uint32_t(s.bk_mode); s.bk_mode = int32_t(value); break;
    case kSetTextColor: prev = s.text_color; s.text_color = value; break;
    case kSetDCBrushColor: prev = s.dc_brush_color; s.dc_brush_color = value; break;
    case kSetDCPenColor: prev = s.dc_pen_color; s.dc_pen_color = value; break;
    case kSetGraphicsMode:
      if (value != 1 && value != 2) return c.fail(kErrorInvalidParameter);
      prev = uint32_t(s.graphics_mode);
      if (value == 1) s.world = Xform();      // back to compatible mode: the world transform is gone
      s.graphics_mode = int32_t(value);
      break;
    case kSetROP2: if (value < 1 || value > 16) return c.fail(kErrorInvalidParameter); prev = uint32_t(s.rop2); s.rop2 = int32_t(value); break;
    case kSetTextAlign: prev = uint32_t(s.text_align); s.text_align = int32_t(value & 0x1FF); break;
    case kSetPolyFillMode: if (value != 1 && value != 2) return c.fail(kErrorInvalidParameter); prev = uint32_t(s.poly_fill); s.poly_fill = int32_t(value); break;
    case kSetStretchBltMode: if (value < 1 || value > 4) return c.fail(kErrorInvalidParameter); prev = uint32_t(s.stretch); s.stretch = int32_t(value); break;
    case kSetArcDirection: if (value != 1 && value != 2) return c.fail(kErrorInvalidParameter); prev = uint32_t(s.arc_dir); s.arc_dir = int32_t(value); break;
    case kSetRelAbs: prev = uint32_t(s.rel_abs); s.rel_abs = int32_t(value); break;
    case kSetCharExtra: prev = uint32_t(s.char_extra); s.char_extra = int32_t(value); break;
    case kSetWndOrgX: prev = uint32_t(s.wnd_org.x); s.wnd_org.x = int32_t(value); break;
    case kSetWndOrgY: prev = uint32_t(s.wnd_org.y); s.wnd_org.y = int32_t(value); break;
    case kSetWndExtX: prev = uint32_t(s.wnd_ext.cx); s.wnd_ext.cx = int32_t(value); if (s.map_mode != 7 && s.map_mode != 8) s.map_mode = 8; break;
    case kSetWndExtY: prev = uint32_t(s.wnd_ext.cy); s.wnd_ext.cy = int32_t(value); if (s.map_mode != 7 && s.map_mode != 8) s.map_mode = 8; break;
    case kSetVportOrgX: prev = uint32_t(s.vport_org.x); s.vport_org.x = int32_t(value); break;
    case kSetVportOrgY: prev = uint32_t(s.vport_org.y); s.vport_org.y = int32_t(value); break;
    case kSetVportExtX: prev = uint32_t(s.vport_ext.cx); s.vport_ext.cx = int32_t(value); if (s.map_mode != 7 && s.map_mode != 8) s.map_mode = 8; break;
    case kSetVportExtY: prev = uint32_t(s.vport_ext.cy); s.vport_ext.cy = int32_t(value); if (s.map_mode != 7 && s.map_mode != 8) s.map_mode = 8; break;
    default: return c.fail(kErrorInvalidParameter);
  }
  if ((s.wnd_ext.cx == 0 || s.wnd_ext.cy == 0) && method >= kSetWndExtX && method <= kSetWndExtY) { /* a zero extent makes the mapping unusable: LPtoDP treats it as 1 */ }
  if (c.arg(3) != 0 && !c.write_u32(c.arg(3), prev)) return 0;
  return 1;
}

uint64_t NtGdiGetDCDword(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const DcPod& s = dc->s;
  uint32_t v = 0;
  switch (c.arg32(1)) {
    case 0: v = uint32_t(s.arc_dir); break;
    case 1: v = s.bk_color; break;
    case 2: v = uint32_t(s.bk_mode); break;
    case 3: v = s.dc_brush_color; break;
    case 4: v = s.dc_pen_color; break;
    case 5: v = uint32_t(s.graphics_mode); break;
    case 6: v = uint32_t(s.layout); break;
    case 7: v = uint32_t(s.poly_fill); break;
    case 8: v = uint32_t(s.rop2); break;
    case 9: v = s.text_color; break;
    case 10: v = dc->memory ? 1 : 0; break;
    case 11: v = 0; break;
    case 12: v = uint32_t(s.stretch); break;
    case 13: v = uint32_t(s.rel_abs); break;
    case 14: v = uint32_t(s.char_extra); break;
    case 15: v = uint32_t(s.text_align); break;
    case 16: v = uint32_t(s.map_mode); break;
    case 20: v = uint32_t(s.wnd_org.x); break;
    case 21: v = uint32_t(s.wnd_org.y); break;
    case 22: v = uint32_t(s.wnd_ext.cx); break;
    case 23: v = uint32_t(s.wnd_ext.cy); break;
    case 24: v = uint32_t(s.vport_org.x); break;
    case 25: v = uint32_t(s.vport_org.y); break;
    case 26: v = uint32_t(s.vport_ext.cx); break;
    case 27: v = uint32_t(s.vport_ext.cy); break;
    default: return c.fail(kErrorInvalidParameter);
  }
  return ret_bool(c.write_u32(c.arg(2), v));
}

uint64_t NtGdiGetDCPoint(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  Point p;
  switch (c.arg32(1)) {
    case 0: p = dc->s.brush_org; break;
    case 1: p = dc->cur_pos; break;
    case 2: p = Point{dc->origin_x, dc->origin_y}; break;
    case 3: p = dc->s.vport_org; break;
    case 4: p = dc->s.wnd_org; break;
    case 5: p = Point{dc->s.vport_ext.cx, dc->s.vport_ext.cy}; break;
    case 6: p = Point{dc->s.wnd_ext.cx, dc->s.wnd_ext.cy}; break;
    default: return c.fail(kErrorInvalidParameter);
  }
  return ret_bool(c.write_struct(c.arg(2), p));
}

uint64_t NtGdiSetBrushOrg(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (c.arg(3) != 0 && !c.write_struct(c.arg(3), dc->s.brush_org)) return 0;
  dc->s.brush_org = Point{c.argi(1), c.argi(2)};
  return 1;
}

uint64_t NtGdiMoveTo(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (c.arg(3) != 0 && !c.write_struct(c.arg(3), dc->cur_pos)) return 0;
  dc->cur_pos = Point{c.argi(1), c.argi(2)};
  if (path_recording(*dc)) path_move(*dc, lp_to_dp(*dc, dc->cur_pos));
  return 1;
}

uint64_t NtGdiModifyWorldTransform(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t mode = c.arg32(2);
  if (!dc) return 0;
  if (mode == 1) { dc->s.world = Xform(); return 1; }
  if (dc->s.graphics_mode != 2) return c.fail(kErrorInvalidParameter);
  float m[6];
  if (mode < 1 || mode > 4 || !c.read_array(c.arg(1), m, 6)) return c.fail(kErrorInvalidParameter);
  Xform x{m[0], m[1], m[2], m[3], m[4], m[5]}, result;
  switch (mode) {
    case 2: result = multiply(x, dc->s.world); break;
    case 3: result = multiply(dc->s.world, x); break;
    default: result = x; break;
  }
  Xform inv;
  if (!xform_invert(result, &inv)) return c.fail(kErrorInvalidParameter);
  dc->s.world = result;
  return 1;
}

uint64_t NtGdiGetTransform(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  Xform x;
  const uint32_t which = c.arg32(1);
  const DcPod& s = dc->s;
  Xform page;
  page.m11 = double(s.vport_ext.cx) / (s.wnd_ext.cx ? s.wnd_ext.cx : 1);
  page.m22 = double(s.vport_ext.cy) / (s.wnd_ext.cy ? s.wnd_ext.cy : 1);
  page.dx = s.vport_org.x - s.wnd_org.x * page.m11;
  page.dy = s.vport_org.y - s.wnd_org.y * page.m22;
  switch (which) {
    case 0x203: x = s.world; break;
    case 0x204: x = page; break;
    case 0x205: if (!xform_invert(page, &x)) return 0; break;
    case 0x206: x = multiply(s.world, page); break;
    default: return c.fail(kErrorInvalidParameter);
  }
  const float m[6] = {float(x.m11), float(x.m12), float(x.m21), float(x.m22), float(x.dx), float(x.dy)};
  return ret_bool(c.write_array(c.arg(2), m, 6));
}

uint64_t NtGdiTransformPoints(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const int32_t count = c.argi(3), mode = c.argi(4);
  if (!dc || count < 0 || count > (1 << 20) || (mode != 0 && mode != 1)) return 0;
  Xform inv;
  if (mode == 1 && dc_world_active(*dc) && !xform_invert(dc->s.world, &inv)) return 0;
  for (int32_t i = 0; i < count; ++i) {
    Point p;
    if (!c.read_struct(c.arg(1) + uint64_t(i) * 8, &p)) return 0;
    p = mode == 0 ? lp_to_dp(*dc, p) : dp_to_lp(*dc, p);
    if (!c.write_struct(c.arg(2) + uint64_t(i) * 8, p)) return 0;
  }
  return 1;
}

uint64_t NtGdiSetVirtualResolution(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  dc->virt_res_x = c.argi(1); dc->virt_res_y = c.argi(2); dc->virt_size_x = c.argi(3); dc->virt_size_y = c.argi(4);
  return 1;
}

uint64_t NtGdiScaleExt(Call& c, bool viewport) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const int32_t xn = c.argi(1), xd = c.argi(2), yn = c.argi(3), yd = c.argi(4);
  Size& e = viewport ? dc->s.vport_ext : dc->s.wnd_ext;
  if (c.arg(5) != 0 && !c.write_struct(c.arg(5), e)) return 0;
  if (xd == 0 || yd == 0) return 0;
  if (dc->s.map_mode != 7 && dc->s.map_mode != 8) return 1;
  e.cx = scale_round(e.cx, xn, xd);
  e.cy = scale_round(e.cy, yn, yd);
  return 1;
}
uint64_t NtGdiScaleViewportExtEx(Call& c) { return NtGdiScaleExt(c, true); }
uint64_t NtGdiScaleWindowExtEx(Call& c) { return NtGdiScaleExt(c, false); }

uint64_t NtGdiMirrorWindowOrg(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  return 1;
}

uint64_t NtGdiSetLayout(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return kClrInvalid;
  const uint32_t prev = uint32_t(dc->s.layout), layout = c.arg32(2);
  if (layout & ~0xFu) return kClrInvalid;
  dc->s.layout = int32_t(layout);
  return prev;
}

uint64_t NtGdiSetMiterLimit(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const float limit = __builtin_bit_cast(float, c.arg32(1));
  if (limit < 1.0f) return 0;
  if (c.arg(2) != 0 && !c.write_array(c.arg(2), &dc->s.miter_limit, 1)) return 0;
  dc->s.miter_limit = limit;
  return 1;
}

uint64_t NtGdiGetMiterLimit(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  return dc && c.write_array(c.arg(1), &dc->s.miter_limit, 1) ? 1 : 0;
}

uint64_t NtGdiSetTextJustification(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  dc->s.just_extra = c.argi(1) < 0 ? 0 : c.argi(1);
  dc->s.just_breaks = c.argi(2);
  return 1;
}

uint64_t NtGdiGetDCObject(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  switch (c.arg32(1)) {
    case 1: case 11: return dc->s.pen;
    case 2: return dc->s.brush;
    case 5: return dc->s.palette;
    case 6: return dc->s.font;
    case 7: return dc->memory ? dc->bitmap : 0;
    default: return c.fail(kErrorInvalidParameter);
  }
}

uint64_t NtGdiSetBoundsRect(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t flags = c.arg32(2);
  if (!dc || (flags & ~0xFu)) return 0;
  const uint32_t prev = (rect_empty(dc->bounds) ? 0 : 2u) | ((dc->bounds_flags & 4) ? 4u : 8u);
  if (flags & 1) dc->bounds = Rect();
  if (flags & 2) {
    Rect r;
    if (c.arg(1) != 0 && c.read_struct(c.arg(1), &r)) dc->bounds = rect_union(dc->bounds, rect_normalized(r));
  }
  if (flags & 4) dc->bounds_flags |= 4;
  if (flags & 8) dc->bounds_flags &= ~4u;
  return prev;
}

uint64_t NtGdiGetBoundsRect(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (c.arg(1) != 0 && !c.write_struct(c.arg(1), dc->bounds)) return 0;
  const uint32_t r = (rect_empty(dc->bounds) ? 0 : 2u) | ((dc->bounds_flags & 4) ? 4u : 8u);
  if (c.arg32(2) & 1) dc->bounds = Rect();
  return r;
}

// ---- Clipping.

uint64_t NtGdiExtSelectClipRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const int32_t mode = c.argi(2);
  if (!dc) return 0;
  allocators::IAllocator& al = *alloc();
  if (c.arg32(1) == 0) {
    if (mode != 5) return 0;
    dc->s.has_clip = false;
    dc->s.clip.clear();
    return 2;
  }
  Region* r = find_region(c.arg32(1));
  if (!r || mode < 1 || mode > 5) return c.fail(kErrorInvalidHandle);
  List<Rect> result(al), whole(al);
  if (mode == 5) {
    if (!rgn_assign(result, r->rects.view())) return 0;
  } else {
    if (dc->s.has_clip) { if (!rgn_assign(whole, dc->s.clip.view())) return 0; }
    else if (!whole.push_back(Rect{-kInfinite, -kInfinite, kInfinite, kInfinite})) return 0;
    if (!rgn_combine(whole.view(), r->rects.view(), mode, &result)) return 0;
  }
  dc->s.has_clip = true;
  if (!rgn_assign(dc->s.clip, result.view())) return 0;
  return ret_region(dc->s.clip);
}

uint64_t clip_with_rect(Call& c, bool exclude) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const Rect r = lp_to_dp_rect(*dc, Rect{c.argi(1), c.argi(2), c.argi(3), c.argi(4)});
  allocators::IAllocator& al = *alloc();
  List<Rect> whole(al), one(al), out(al);
  if (dc->s.has_clip) { if (!rgn_assign(whole, dc->s.clip.view())) return 0; }
  else if (!whole.push_back(Rect{-kInfinite, -kInfinite, kInfinite, kInfinite})) return 0;
  if (!rgn_from_rect(&one, r) || !rgn_combine(whole.view(), one.view(), exclude ? 4 : 1, &out)) return 0;
  dc->s.has_clip = true;
  if (!rgn_assign(dc->s.clip, out.view())) return 0;
  return ret_region(dc->s.clip);
}
uint64_t NtGdiIntersectClipRect(Call& c) { return clip_with_rect(c, false); }
uint64_t NtGdiExcludeClipRect(Call& c) { return clip_with_rect(c, true); }

uint64_t NtGdiOffsetClipRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  if (!dc->s.has_clip) return 2;
  const Point a = lp_to_dp(*dc, Point{0, 0}), b = lp_to_dp(*dc, Point{c.argi(1), c.argi(2)});
  rgn_offset(dc->s.clip, sat_sub(b.x, a.x), sat_sub(b.y, a.y));
  return ret_region(dc->s.clip);
}

uint64_t NtGdiSetMetaRgn(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  allocators::IAllocator& al = *alloc();
  List<Rect> out(al);
  if (dc->s.has_clip && dc->s.has_meta) { if (!rgn_combine(dc->s.clip.view(), dc->s.meta.view(), 1, &out)) return 0; }
  else if (dc->s.has_clip) { if (!rgn_assign(out, dc->s.clip.view())) return 0; }
  else if (dc->s.has_meta) { if (!rgn_assign(out, dc->s.meta.view())) return 0; }
  else return 2;
  dc->s.has_meta = true;
  if (!rgn_assign(dc->s.meta, out.view())) return 0;
  dc->s.has_clip = false;
  dc->s.clip.clear();
  return ret_region(dc->s.meta);
}

uint64_t NtGdiGetAppClipBox(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  List<Rect> vis(*alloc());
  if (!clip_device(*dc, &vis)) return 0;
  Rect b = rgn_bound(vis.view());
  Rect out;
  if (!rect_empty(b)) {
    if (b.left <= -kInfinite / 2) { b.left = -kInfinite; b.top = -kInfinite; b.right = kInfinite; b.bottom = kInfinite; }
    const Point p = dp_to_lp(*dc, Point{b.left, b.top}), q = dp_to_lp(*dc, Point{b.right, b.bottom});
    out = rect_normalized(Rect{p.x, p.y, q.x, q.y});
  }
  if (!c.write_struct(c.arg(1), out)) return 0;
  return ret_region(vis);
}

uint64_t NtGdiPtVisible(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  List<Rect> vis(*alloc());
  const Point p = lp_to_dp(*dc, Point{c.argi(1), c.argi(2)});
  return clip_device(*dc, &vis) && rgn_has_point(vis.view(), p.x, p.y) ? 1 : 0;
}

uint64_t NtGdiRectVisible(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Rect r;
  if (!dc || !c.read_struct(c.arg(1), &r)) return 0;
  List<Rect> vis(*alloc());
  return clip_device(*dc, &vis) && rgn_hits_rect(vis.view(), lp_to_dp_rect(*dc, r)) ? 1 : 0;
}

// ---- Device capabilities.

int32_t device_cap(Dc& dc, int32_t cap) {
  Bitmap* bm = nullptr;
  Surface s = dc_surface(dc, &bm);
  const int32_t w = s.valid() && dc.memory ? s.width : int32_t(screen_width()), h = s.valid() && dc.memory ? s.height : int32_t(screen_height());
  const uint32_t bpp = s.valid() ? s.bpp : 32;
  switch (cap) {
    case 0: return 0x0400;
    case 2: return dc.printer ? 2 : 1;
    case 4: return w * 254 / 960;
    case 6: return h * 254 / 960;
    case 8: return w;
    case 10: return h;
    case 12: return int32_t(bpp >= 24 ? 32 : bpp);
    case 14: return 1;
    case 16: return -1;
    case 18: return 16;
    case 24: return bpp > 8 ? -1 : bpp == 8 ? 20 : int32_t(1u << bpp);
    case 28: return 0xFF;
    case 30: return 0xFE;
    case 32: return 0xFF;
    case 34: return 0x7E19;
    case 36: return 1;
    case 38: return bpp <= 8 ? 0x7F99 | 0x100 : 0x7E99;
    case 40: case 42: return 36;
    case 44: return 51;
    case 45: return 0x37;
    case 88: case 90: return 96;
    case 104: return bpp == 8 ? 256 : 0;
    case 106: return bpp == 8 ? 20 : 0;
    case 108: return bpp >= 24 ? 24 : int32_t(bpp);
    case 116: return 60;
    case 117: return int32_t(screen_height());
    case 118: return int32_t(screen_width());
    default: return 0;
  }
}

uint64_t NtGdiGetDeviceCaps(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  return ret_i32(device_cap(*dc, c.argi(1)));
}

uint64_t NtGdiSetSizeDevice(Call& c) { return dc_arg(c, c.arg32(0)) ? 1 : 0; }

}  // namespace

void register_dc_services() {
  GDI_REGISTER(CreateCompatibleDC, NtGdiCreateCompatibleDC);
  GDI_REGISTER(OpenDCW, NtGdiOpenDCW);
  GDI_REGISTER(MakeInfoDC, NtGdiMakeInfoDC);
  GDI_REGISTER(ResetDC, NtGdiResetDC);
  GDI_REGISTER(CancelDC, NtGdiCancelDC);
  GDI_REGISTER(ComputeXformCoefficients, NtGdiComputeXformCoefficients);
  GDI_REGISTER(CombineTransform, NtGdiCombineTransform);
  GDI_REGISTER(GetDCforBitmap, NtGdiGetDCforBitmap);
  GDI_REGISTER(SaveDC, NtGdiSaveDC);
  GDI_REGISTER(RestoreDC, NtGdiRestoreDC);
  GDI_REGISTER(GetAndSetDCDword, NtGdiGetAndSetDCDword);
  GDI_REGISTER(GetDCDword, NtGdiGetDCDword);
  GDI_REGISTER(GetDCPoint, NtGdiGetDCPoint);
  GDI_REGISTER(SetBrushOrg, NtGdiSetBrushOrg);
  GDI_REGISTER(MoveTo, NtGdiMoveTo);
  GDI_REGISTER(ModifyWorldTransform, NtGdiModifyWorldTransform);
  GDI_REGISTER(GetTransform, NtGdiGetTransform);
  GDI_REGISTER(TransformPoints, NtGdiTransformPoints);
  GDI_REGISTER(SetVirtualResolution, NtGdiSetVirtualResolution);
  GDI_REGISTER(ScaleViewportExtEx, NtGdiScaleViewportExtEx);
  GDI_REGISTER(ScaleWindowExtEx, NtGdiScaleWindowExtEx);
  GDI_REGISTER(MirrorWindowOrg, NtGdiMirrorWindowOrg);
  GDI_REGISTER(SetLayout, NtGdiSetLayout);
  GDI_REGISTER(SetMiterLimit, NtGdiSetMiterLimit);
  GDI_REGISTER(GetMiterLimit, NtGdiGetMiterLimit);
  GDI_REGISTER(SetTextJustification, NtGdiSetTextJustification);
  GDI_REGISTER(GetDCObject, NtGdiGetDCObject);
  GDI_REGISTER(SetBoundsRect, NtGdiSetBoundsRect);
  GDI_REGISTER(GetBoundsRect, NtGdiGetBoundsRect);
  GDI_REGISTER(ExtSelectClipRgn, NtGdiExtSelectClipRgn);
  GDI_REGISTER(IntersectClipRect, NtGdiIntersectClipRect);
  GDI_REGISTER(ExcludeClipRect, NtGdiExcludeClipRect);
  GDI_REGISTER(OffsetClipRgn, NtGdiOffsetClipRgn);
  GDI_REGISTER(SetMetaRgn, NtGdiSetMetaRgn);
  GDI_REGISTER(GetAppClipBox, NtGdiGetAppClipBox);
  GDI_REGISTER(PtVisible, NtGdiPtVisible);
  GDI_REGISTER(RectVisible, NtGdiRectVisible);
  GDI_REGISTER(GetDeviceCaps, NtGdiGetDeviceCaps);
  GDI_REGISTER(SetSizeDevice, NtGdiSetSizeDevice);
}

}  // namespace process::win32k::gdi
