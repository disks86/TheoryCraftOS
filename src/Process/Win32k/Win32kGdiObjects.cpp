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

// GDI objects: pens, brushes, fonts, palettes, bitmaps, colour spaces; selecting them into device contexts; deleting them; GetObject.
namespace process::win32k::gdi {

namespace {

constexpr uint32_t kMaxBitmapBytes = 1u << 28;

void put_u32(uint8_t* p, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put_u16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
uint32_t get_u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t get_u16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }

Object* owned(Call& c, uint32_t h, uint8_t type) {
  Object* o = find_object(h, type);
  if (!o || (!o->stock && o->pid != c.pid())) { c.set_last_error(kErrorInvalidHandle); return nullptr; }
  return o;
}

}  // namespace

void fill_default_colors(uint32_t bpp, List<uint32_t>* out) {
  out->clear();
  if (bpp == 1) { out->push_back(0); out->push_back(0xFFFFFF); return; }
  static const uint8_t vga[16][3] = {{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0}, {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
                                     {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
  if (bpp == 4) { for (const auto& e : vga) out->push_back(make_rgb(e[0], e[1], e[2])); return; }
  if (bpp != 8) return;
  for (int i = 0; i < 8; ++i) out->push_back(make_rgb(vga[i][0], vga[i][1], vga[i][2]));
  out->push_back(make_rgb(192, 220, 192));
  out->push_back(make_rgb(166, 202, 240));
  for (uint32_t r = 0; r < 6; ++r) for (uint32_t g = 0; g < 6; ++g) for (uint32_t b = 0; b < 6; ++b) {
    if (out->count() >= 246) break;
    out->push_back(make_rgb(r * 51, g * 51, b * 51));
  }
  while (out->count() < 246) out->push_back(0);
  out->push_back(make_rgb(255, 251, 240));
  out->push_back(make_rgb(160, 160, 164));
  for (int i = 8; i < 16; ++i) out->push_back(make_rgb(vga[i][0], vga[i][1], vga[i][2]));
}

Bitmap* new_bitmap(uint64_t pid, int32_t w, int32_t h, uint32_t bpp) {
  if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return nullptr;
  const uint64_t stride = dib_stride(uint32_t(w), bpp);
  if (stride * uint32_t(h) > kMaxBitmapBytes) return nullptr;
  Bitmap* b = make_object<Bitmap>(kObjBitmap, pid);
  if (!b) return nullptr;
  b->width = w;
  b->height = h;
  b->bpp = bpp;
  b->stride = uint32_t(stride);
  if (!b->data.put_zeros(stride * uint32_t(h))) { destroy_object(b->handle); return nullptr; }
  if (bpp <= 8) fill_default_colors(bpp, &b->colors);
  return b;
}

bool dc_select_object(Dc& dc, Object* obj, uint32_t* previous) {
  uint32_t* slot = nullptr;
  switch (obj->type) {
    case kObjBrush: slot = &dc.s.brush; break;
    case kObjPen: case kObjExtPen: slot = &dc.s.pen; break;
    case kObjFont: slot = &dc.s.font; break;
    case kObjPalette: slot = &dc.s.palette; break;
    case kObjBitmap:
      if (!dc.memory) return false;
      slot = &dc.bitmap;
      if (obj->selected > 0 && obj->handle != dc.bitmap) return false;
      break;
    default: return false;
  }
  *previous = *slot;
  if (Object* old = find_object(*slot)) if (old->selected > 0) --old->selected;
  *slot = obj->handle;
  ++obj->selected;
  return true;
}

namespace {

uint64_t select_generic(Call& c, uint8_t type) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  Object* o = owned(c, c.arg32(1), type);
  if (!o) return 0;
  uint32_t prev = 0;
  if (!dc_select_object(*dc, o, &prev)) return c.fail(kErrorInvalidParameter);
  return prev;
}

uint64_t NtGdiSelectBitmap(Call& c) { return select_generic(c, kObjBitmap); }
uint64_t NtGdiSelectBrush(Call& c) { return select_generic(c, kObjBrush); }
uint64_t NtGdiSelectPen(Call& c) { return select_generic(c, kObjPen); }
uint64_t NtGdiSelectFont(Call& c) { return select_generic(c, kObjFont); }

uint64_t NtGdiCreateSolidBrush(Call& c) {
  Brush* b = make_object<Brush>(kObjBrush, c.pid());
  if (!b) return c.fail(kErrorNotEnoughMemory);
  b->style = 0;
  b->color = c.arg32(0);
  return b->handle;
}

uint64_t NtGdiCreateHatchBrushInternal(Call& c) {
  const uint32_t style = c.arg32(0);
  if (style > 5) return c.fail(kErrorInvalidParameter);
  Brush* b = make_object<Brush>(kObjBrush, c.pid());
  if (!b) return c.fail(kErrorNotEnoughMemory);
  b->style = 2;
  b->hatch = style;
  b->color = c.arg32(1);
  return b->handle;
}

uint64_t NtGdiCreatePatternBrushInternal(Call& c) {
  Bitmap* bm = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  if (!bm) return 0;
  const bool is_8x8 = c.arg_bool(2);
  Brush* b = make_object<Brush>(kObjBrush, c.pid());
  if (!b) return c.fail(kErrorNotEnoughMemory);
  dib_sync_in(*bm);
  Surface s = surface_of(*bm);
  b->style = is_8x8 ? 7 : 3;
  b->mono = bm->bpp == 1;
  b->pw = bm->width;
  b->ph = bm->height;
  for (int32_t y = 0; y < bm->height; ++y) {
    for (int32_t x = 0; x < bm->width; ++x) {
      uint32_t v = s.get(x, y);
      if (b->mono) v = (v & 0xFFFFFF) ? 1 : 0;
      if (!b->pattern.push_back(v)) { destroy_object(b->handle); return c.fail(kErrorNotEnoughMemory); }
    }
  }
  return b->handle;
}

uint64_t NtGdiCreatePen(Call& c) {
  const uint32_t style = c.arg32(0);
  const int32_t width = c.argi(1);
  if (style > 6) return c.fail(kErrorInvalidParameter);
  Pen* p = make_object<Pen>(kObjPen, c.pid());
  if (!p) return c.fail(kErrorNotEnoughMemory);
  p->style = style;
  p->width = width < 0 ? uint32_t(-width) : uint32_t(width);
  if (style == 5) p->width = 1;
  p->color = c.arg32(2);
  return p->handle;
}

uint64_t NtGdiExtCreatePen(Call& c) {
  const uint32_t style = c.arg32(0), width = c.arg32(1), brush_style = c.arg32(2), count = c.arg32(6);
  const uint64_t bits = c.arg(7);
  const uint32_t kind = style & 0xF, type = style & 0xF0000;
  if (kind > 8 || (type != 0 && type != 0x10000) || brush_style > 9) return c.fail(kErrorInvalidParameter);
  if (type == 0 && width != 1 && kind != 5) return c.fail(kErrorInvalidParameter);      // a cosmetic pen is one pixel wide
  if (kind == 7) { if (count == 0 || count > 16) return c.fail(kErrorInvalidParameter); }
  else if (count != 0) return c.fail(kErrorInvalidParameter);
  if (kind == 8) return c.fail(kErrorInvalidParameter);
  Pen* p = make_object<Pen>(kObjExtPen, c.pid());
  if (!p) return c.fail(kErrorNotEnoughMemory);
  p->ext = true;
  p->style = style;
  p->width = width;
  p->brush_style = brush_style;
  p->color = c.arg32(3);
  p->hatch = uint32_t(c.arg(5));
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t d = 0;
    if (!c.read_u32(bits + uint64_t{i} * 4, &d)) { destroy_object(p->handle); return c.fail(kErrorInvalidParameter); }
    p->dashes.push_back(d);
  }
  if (Brush* br = find_brush(c.arg32(10))) {
    if (br->style == 0) p->color = br->color;
    p->brush_style = br->style;
  }
  return p->handle;
}

uint64_t NtGdiHfontCreate(Call& c) {
  uint8_t raw[92] = {};
  if (!c.read(c.arg(0), ByteView(raw, 92))) return c.fail(kErrorInvalidParameter);
  Font* f = make_object<Font>(kObjFont, c.pid());
  if (!f) return c.fail(kErrorNotEnoughMemory);
  f->lf_height = int32_t(get_u32(raw));
  f->lf_width = int32_t(get_u32(raw + 4));
  f->lf_escapement = int32_t(get_u32(raw + 8));
  f->lf_orientation = int32_t(get_u32(raw + 12));
  f->height = uint32_t(iabs(f->lf_height));
  f->width = uint32_t(iabs(f->lf_width));
  f->escapement = uint32_t(f->lf_escapement);
  f->orientation = uint32_t(f->lf_orientation);
  f->weight = get_u32(raw + 16);
  f->italic = raw[20]; f->underline = raw[21]; f->strikeout = raw[22]; f->charset = raw[23];
  f->out_precision = raw[24]; f->clip_precision = raw[25]; f->quality = raw[26]; f->pitch_family = raw[27];
  for (int i = 0; i < 31; ++i) f->face[i] = char16_t(get_u16(raw + 28 + 2 * i));
  f->face[31] = 0;
  // an ENUMLOGFONTEXDVW also has the full name, style and script
  if (c.arg32(1) >= 92 + 64 + 32 + 32 + 32 && c.arg32(1) < 4096) {
    uint8_t extra[128] = {};
    if (c.read(c.arg(0) + 92, ByteView(extra, 128))) for (int i = 0; i < 63; ++i) f->full_name[i] = char16_t(get_u16(extra + 2 * i));
  }
  return f->handle;
}

uint64_t NtGdiCreatePaletteInternal(Call& c) {
  const uint32_t count = c.arg32(1);
  if (count == 0 || count > 65536) return c.fail(kErrorInvalidParameter);
  Palette* p = make_object<Palette>(kObjPalette, c.pid());
  if (!p) return c.fail(kErrorNotEnoughMemory);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t e = 0;
    if (!c.read_u32(c.arg(0) + 4 + uint64_t{i} * 4, &e)) { destroy_object(p->handle); return c.fail(kErrorInvalidParameter); }
    p->entries.push_back(e);
  }
  return p->handle;
}

uint64_t NtGdiCreateHalftonePalette(Call& c) {
  Palette* p = make_object<Palette>(kObjPalette, c.pid());
  if (!p) return c.fail(kErrorNotEnoughMemory);
  List<uint32_t> d(*alloc());
  fill_default_colors(8, &d);
  for (uint32_t e : d.view()) p->entries.push_back(e & 0xFFFFFF);
  return p->handle;
}

uint64_t NtGdiResizePalette(Call& c) {
  Palette* p = static_cast<Palette*>(owned(c, c.arg32(0), kObjPalette));
  const uint32_t n = c.arg32(1);
  if (!p || n == 0 || n > 65536) return 0;
  while (p->entries.count() > n) p->entries.remove_at(p->entries.count() - 1);
  while (p->entries.count() < n) p->entries.push_back(0);
  return 1;
}

uint32_t nearest_in(ArrayView<const uint32_t> colors, ColorRef c) {
  uint32_t best = 0;
  uint64_t bd = UINT64_MAX;
  for (uint64_t i = 0; i < colors.size(); ++i) {
    const int64_t dr = int64_t(cr_r(colors[i])) - cr_r(c), dg = int64_t(cr_g(colors[i])) - cr_g(c), db = int64_t(cr_b(colors[i])) - cr_b(c);
    const uint64_t d = uint64_t(dr * dr + dg * dg + db * db);
    if (d < bd) { bd = d; best = uint32_t(i); if (d == 0) break; }
  }
  return best;
}

uint64_t NtGdiGetNearestPaletteIndex(Call& c) {
  Palette* p = static_cast<Palette*>(owned(c, c.arg32(0), kObjPalette));
  if (!p || p->entries.count() == 0) return kClrInvalid;
  return nearest_in(ArrayView<const uint32_t>(p->entries.view()), c.arg32(1));
}

uint64_t NtGdiGetNearestColor(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return kClrInvalid;
  const ColorRef col = c.arg32(1) & 0xFFFFFF;
  Bitmap* bm = nullptr;
  Surface s = dc_surface(*dc, &bm);
  if (!s.valid() || s.bpp >= 24) return col;
  if (s.bpp == 16) return make_rgb(cr_r(col) & 0xF8, cr_g(col) & 0xF8, cr_b(col) & 0xF8);
  return s.colors.size() ? s.colors[nearest_in(s.colors, col)] & 0xFFFFFF : col;
}

uint64_t NtGdiDoPalette(Call& c) {
  const uint32_t handle = c.arg32(0), func = c.arg32(4);
  const uint32_t start = c.arg32(1) & 0xFFFF, count = c.arg32(2) & 0xFFFF;
  const uint64_t entries = c.arg(3);
  const bool inbound = c.arg_bool(5);
  if (func == 3) {      // GetSystemPaletteEntries: the handle is a DC
    if (!dc_arg(c, handle)) return 0;
    List<uint32_t> d(*alloc());
    fill_default_colors(8, &d);
    if (entries == 0) return 256;
    uint32_t done = 0;
    for (uint32_t i = 0; i < count && start + i < 256; ++i, ++done) if (!c.write_u32(entries + uint64_t{i} * 4, d[start + i] & 0xFFFFFF)) return 0;
    return done;
  }
  if (func == 4 || func == 5) {      // Get/SetDIBColorTable: the handle is a memory DC
    Dc* dc = dc_arg(c, handle);
    Bitmap* bm = dc ? find_bitmap(dc->bitmap) : nullptr;
    if (!bm || !bm->dib || bm->bpp > 8) return 0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < count && start + i < bm->colors.count(); ++i, ++done) {
      if (func == 4) {
        const uint32_t e = bm->colors[start + i];
        if (!c.write_u32(entries + uint64_t{i} * 4, cr_b(e) | cr_g(e) << 8 | cr_r(e) << 16)) return 0;
      } else {
        uint32_t q = 0;
        if (!c.read_u32(entries + uint64_t{i} * 4, &q)) return 0;
        bm->colors[start + i] = make_rgb((q >> 16) & 0xFF, (q >> 8) & 0xFF, q & 0xFF);
      }
    }
    return done;
  }
  Palette* p = static_cast<Palette*>(owned(c, handle, kObjPalette));
  if (!p || func > 2) return 0;
  if (func == 2 && entries == 0) return p->entries.count();      // GetPaletteEntries with no buffer: the size
  if (start >= p->entries.count()) return 0;
  uint32_t done = 0;
  for (uint32_t i = 0; i < count && start + i < p->entries.count(); ++i, ++done) {
    if (func == 2) { if (!c.write_u32(entries + uint64_t{i} * 4, p->entries[start + i])) return 0; }
    else {
      uint32_t e = 0;
      if (!inbound || !c.read_u32(entries + uint64_t{i} * 4, &e)) return 0;
      if (func == 0 && (p->entries[start + i] >> 24 & 1) == 0) continue;      // AnimatePalette changes only PC_RESERVED entries
      p->entries[start + i] = e;
    }
  }
  return done;
}

uint64_t NtGdiGetSystemPaletteUse(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  return dc ? dc->system_palette_use : 0;
}
uint64_t NtGdiSetSystemPaletteUse(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t use = c.arg32(1);
  if (!dc || use > 2) return 0;
  const uint32_t prev = dc->system_palette_use;
  dc->system_palette_use = use;
  return prev;
}

uint64_t NtGdiCreateCompatibleBitmap(Call& c) {
  Dc* dc = c.arg32(0) ? dc_arg(c, c.arg32(0)) : nullptr;
  if (c.arg32(0) && !dc) return 0;
  const int32_t w = c.argi(1), h = c.argi(2);
  Bitmap* b = nullptr;
  if (w <= 0 || h <= 0) {
    b = new_bitmap(c.pid(), 1, 1, 1);
  } else {
    uint32_t bpp = 32;
    if (dc && dc->memory) if (Bitmap* cur = find_bitmap(dc->bitmap)) bpp = cur->bpp;
    b = new_bitmap(c.pid(), w, h, bpp);
    if (b && dc && dc->memory) if (Bitmap* cur = find_bitmap(dc->bitmap)) if (cur->bpp <= 8) for (uint32_t i = 0; i < cur->colors.count() && i < b->colors.count(); ++i) b->colors[i] = cur->colors[i];
  }
  return b ? b->handle : c.fail(kErrorNotEnoughMemory);
}

uint64_t NtGdiCreateBitmap(Call& c) {
  const int32_t w = c.argi(0), h = c.argi(1);
  const uint32_t planes = c.arg32(2), bpp = c.arg32(3);
  const uint64_t bits = c.arg(4);
  if (w < 0 || h < 0) return c.fail(kErrorInvalidParameter);
  uint32_t depth = planes * bpp;
  if (planes == 0 || bpp == 0) depth = 1;
  Bitmap* b = (w == 0 || h == 0) ? new_bitmap(c.pid(), 1, 1, 1) : new_bitmap(c.pid(), w, h, depth);
  if (!b) return c.fail(kErrorInvalidParameter);
  if (bits != 0 && w > 0 && h > 0) {
    const uint64_t src_stride = ((uint64_t(w) * b->bpp + 15) / 16) * 2;
    for (int32_t y = 0; y < h; ++y) {
      uint8_t* row = b->data.writable_view().subview(uint64_t(y) * b->stride, src_stride <= b->stride ? src_stride : b->stride).data();
      if (!c.read(bits + uint64_t(y) * src_stride, ByteView(row, src_stride <= b->stride ? src_stride : b->stride))) { destroy_object(b->handle); return c.fail(kErrorInvalidParameter); }
    }
  }
  return b->handle;
}

// SetBitmapBits / GetBitmapBits: rows are 16-bit aligned in the caller's buffer.
uint64_t NtGdiSetBitmapBits(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  const uint32_t count = c.arg32(1);
  if (!b) return 0;
  dib_sync_in(*b);
  const uint64_t src_stride = ((uint64_t(b->width) * b->bpp + 15) / 16) * 2;
  uint32_t done = 0;
  for (int32_t y = 0; y < b->height && done < count; ++y) {
    const uint64_t n = src_stride < count - done ? src_stride : count - done;
    const uint64_t take = n < b->stride ? n : b->stride;
    uint8_t* row = b->data.writable_view().subview(uint64_t(b->bottom_up ? b->height - 1 - y : y) * b->stride, take).data();
    if (!c.read(c.arg(2) + done, ByteView(row, take))) return 0;
    done += uint32_t(n);
  }
  dib_sync_out(*b);
  return done;
}

uint64_t NtGdiGetBitmapBits(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  if (!b) return 0;
  dib_sync_in(*b);
  const uint64_t src_stride = ((uint64_t(b->width) * b->bpp + 15) / 16) * 2, total = src_stride * uint32_t(b->height);
  if (c.arg(2) == 0) return total;
  const uint32_t count = c.arg32(1);
  uint32_t done = 0;
  for (int32_t y = 0; y < b->height && done < count; ++y) {
    const uint64_t n = src_stride < count - done ? src_stride : count - done;
    const uint64_t take = n < b->stride ? n : b->stride;
    ConstByteView row = b->data.view().subview(uint64_t(b->bottom_up ? b->height - 1 - y : y) * b->stride, take);
    if (!c.write(c.arg(2) + done, row)) return 0;
    done += uint32_t(n);
  }
  return done;
}

uint64_t NtGdiGetBitmapDimension(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  if (!b) return 0;
  const Size s{b->dim_cx, b->dim_cy};
  return ret_bool(c.write_struct(c.arg(1), s));
}

uint64_t NtGdiSetBitmapDimension(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  if (!b) return 0;
  if (c.arg(3) != 0) { const Size s{b->dim_cx, b->dim_cy}; if (!c.write_struct(c.arg(3), s)) return 0; }
  b->dim_cx = c.argi(1);
  b->dim_cy = c.argi(2);
  return 1;
}

uint64_t NtGdiSetBitmapAttributes(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  if (!b) return 0;
  b->attributes |= c.arg32(1);
  return b->handle;
}
uint64_t NtGdiClearBitmapAttributes(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  if (!b) return 0;
  b->attributes &= ~c.arg32(1);
  return b->handle;
}
uint64_t NtGdiSetBrushAttributes(Call& c) {
  Brush* b = static_cast<Brush*>(owned(c, c.arg32(0), kObjBrush));
  if (!b) return 0;
  b->attributes |= c.arg32(1);
  return b->handle;
}
uint64_t NtGdiClearBrushAttributes(Call& c) {
  Brush* b = static_cast<Brush*>(owned(c, c.arg32(0), kObjBrush));
  if (!b) return 0;
  b->attributes &= ~c.arg32(1);
  return b->handle;
}

uint64_t NtGdiMonoBitmap(Call& c) {
  Bitmap* b = static_cast<Bitmap*>(owned(c, c.arg32(0), kObjBitmap));
  return b && b->bpp == 1 ? 1 : 0;
}



uint64_t NtGdiCreateClientObj(Call& c) {
  const uint32_t type = c.arg32(0);
  if (type == 0 || type > 0x7F) return c.fail(kErrorInvalidParameter);
  Object* o = make_object<ClientObject>(kObjClient, c.pid());
  if (!o) return c.fail(kErrorNotEnoughMemory);
  return o->handle;
}

uint64_t NtGdiDeleteClientObj(Call& c) {
  Object* o = find_object(c.arg32(0), kObjClient);
  if (!o || o->pid != c.pid()) return c.fail(kErrorInvalidHandle);
  return ret_bool(destroy_object(o->handle));
}

uint64_t NtGdiDeleteObjectApp(Call& c) {
  Object* o = find_object(c.arg32(0));
  if (!o || (!o->stock && o->pid != c.pid() && o->pid != 0)) return c.fail(kErrorInvalidHandle);
  if (o->stock) return 1;
  if (o->type == kObjDc) return ret_bool(destroy_object(o->handle));
  if (o->selected > 0) return 0;
  return ret_bool(destroy_object(o->handle));
}

uint64_t NtGdiUnrealizeObject(Call& c) {
  Object* o = find_object(c.arg32(0));
  if (!o || (o->type != kObjBrush && o->type != kObjPalette && o->type != kObjPen)) return c.fail(kErrorInvalidHandle);
  return o->stock ? 0 : 1;
}

void fill_logfont(const Font& f, uint8_t* out) {
  put_u32(out, uint32_t(f.lf_height)); put_u32(out + 4, uint32_t(f.lf_width)); put_u32(out + 8, uint32_t(f.lf_escapement)); put_u32(out + 12, uint32_t(f.lf_orientation));
  put_u32(out + 16, f.weight);
  out[20] = f.italic; out[21] = f.underline; out[22] = f.strikeout; out[23] = f.charset; out[24] = f.out_precision; out[25] = f.clip_precision; out[26] = f.quality; out[27] = f.pitch_family;
  for (int i = 0; i < 32; ++i) put_u16(out + 28 + 2 * i, f.face[i]);
}

uint64_t NtGdiExtGetObjectW(Call& c) {
  Object* o = find_object(c.arg32(0));
  if (!o) return 0;
  const int32_t count = c.argi(1);
  const uint64_t buf = c.arg(2);
  uint8_t out[128] = {};
  uint32_t size = 0;
  switch (o->type) {
    case kObjBitmap: {
      Bitmap& b = *static_cast<Bitmap*>(o);
      size = b.dib ? 104 : 32;
      if (buf == 0) return size;
      if (b.dib && count < 104) size = 32;
      put_u32(out + 4, uint32_t(b.width)); put_u32(out + 8, uint32_t(b.height)); put_u32(out + 12, b.stride);
      put_u16(out + 16, b.planes); put_u16(out + 18, b.bpp);
      if (b.dib) {
        for (int i = 0; i < 8; ++i) out[24 + i] = uint8_t(b.user_address >> (8 * i));
        put_u32(out + 32, 40); put_u32(out + 36, uint32_t(b.width)); put_u32(out + 40, uint32_t(b.bottom_up ? b.height : -b.height));
        put_u16(out + 44, b.planes); put_u16(out + 46, b.bpp); put_u32(out + 48, b.compression); put_u32(out + 52, b.stride * uint32_t(b.height));
        put_u32(out + 56, b.x_ppm); put_u32(out + 60, b.y_ppm); put_u32(out + 64, b.bpp <= 8 ? uint32_t(b.colors.count()) : 0); put_u32(out + 68, b.clr_important);
        if (b.compression == 3) for (int i = 0; i < 3; ++i) put_u32(out + 72 + 4 * i, b.masks[i]);
        for (int i = 0; i < 8; ++i) out[88 + i] = uint8_t(b.section_handle >> (8 * i));
        put_u32(out + 96, b.section_offset);
      }
      break;
    }
    case kObjBrush: {
      Brush& b = *static_cast<Brush*>(o);
      size = 16;
      if (buf == 0) return size;
      put_u32(out, b.style); put_u32(out + 4, b.color);
      put_u32(out + 8, b.style == 2 ? b.hatch : 0);
      break;
    }
    case kObjPen: {
      Pen& p = *static_cast<Pen*>(o);
      size = 16;
      if (buf == 0) return size;
      put_u32(out, p.style & 0xF); put_u32(out + 4, p.width); put_u32(out + 12, p.color);
      break;
    }
    case kObjExtPen: {
      Pen& p = *static_cast<Pen*>(o);
      const uint32_t n = uint32_t(p.dashes.count());
      size = 32 + (n > 1 ? (n - 1) * 4 : 0);
      if (buf == 0) return size;
      if (size > sizeof(out)) return 0;
      put_u32(out, p.style); put_u32(out + 4, p.width); put_u32(out + 8, p.brush_style); put_u32(out + 12, p.color);
      put_u32(out + 24, n);
      for (uint32_t i = 0; i < n; ++i) put_u32(out + 28 + 4 * i, p.dashes[i]);
      break;
    }
    case kObjFont: {
      size = 92;
      if (buf == 0) return size;
      fill_logfont(*static_cast<Font*>(o), out);
      break;
    }
    case kObjPalette: {
      size = 2;
      if (buf == 0) return size;
      put_u16(out, uint32_t(static_cast<Palette*>(o)->entries.count()));
      break;
    }
    default: return 0;
  }
  if (count <= 0) return 0;
  const uint32_t n = size < uint32_t(count) ? size : uint32_t(count);
  if (!c.write(buf, ConstByteView(out, n))) return 0;
  return n;
}

// Colour spaces and colour management are accepted and remembered; nothing is converted.
uint64_t NtGdiCreateColorSpace(Call& c) {
  Object* o = make_object<ClientObject>(kObjColorSpace, c.pid());
  if (!o) return c.fail(kErrorNotEnoughMemory);
  return o->handle;
}
uint64_t NtGdiDeleteColorSpace(Call& c) {
  Object* o = find_object(c.arg32(0), kObjColorSpace);
  if (!o || o->pid != c.pid()) return c.fail(kErrorInvalidHandle);
  return ret_bool(destroy_object(o->handle));
}
uint64_t NtGdiSetColorSpace(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Object* cs = find_object(c.arg32(1), kObjColorSpace);
  if (!dc || !cs) return 0;
  dc->color_space = cs->handle;
  return 1;
}

}  // namespace

void register_object_services() {
  GDI_REGISTER(SelectBitmap, NtGdiSelectBitmap);
  GDI_REGISTER(SelectBrush, NtGdiSelectBrush);
  GDI_REGISTER(SelectPen, NtGdiSelectPen);
  GDI_REGISTER(SelectFont, NtGdiSelectFont);
  GDI_REGISTER(CreateSolidBrush, NtGdiCreateSolidBrush);
  GDI_REGISTER(CreateHatchBrushInternal, NtGdiCreateHatchBrushInternal);
  GDI_REGISTER(CreatePatternBrushInternal, NtGdiCreatePatternBrushInternal);
  GDI_REGISTER(CreatePen, NtGdiCreatePen);
  GDI_REGISTER(ExtCreatePen, NtGdiExtCreatePen);
  GDI_REGISTER(HfontCreate, NtGdiHfontCreate);
  GDI_REGISTER(CreatePaletteInternal, NtGdiCreatePaletteInternal);
  GDI_REGISTER(CreateHalftonePalette, NtGdiCreateHalftonePalette);
  GDI_REGISTER(ResizePalette, NtGdiResizePalette);
  GDI_REGISTER(GetNearestPaletteIndex, NtGdiGetNearestPaletteIndex);
  GDI_REGISTER(GetNearestColor, NtGdiGetNearestColor);
  GDI_REGISTER(DoPalette, NtGdiDoPalette);
  GDI_REGISTER(GetSystemPaletteUse, NtGdiGetSystemPaletteUse);
  GDI_REGISTER(SetSystemPaletteUse, NtGdiSetSystemPaletteUse);
  GDI_REGISTER(CreateCompatibleBitmap, NtGdiCreateCompatibleBitmap);
  GDI_REGISTER(CreateBitmap, NtGdiCreateBitmap);
  GDI_REGISTER(SetBitmapBits, NtGdiSetBitmapBits);
  GDI_REGISTER(GetBitmapBits, NtGdiGetBitmapBits);
  GDI_REGISTER(GetBitmapDimension, NtGdiGetBitmapDimension);
  GDI_REGISTER(SetBitmapDimension, NtGdiSetBitmapDimension);
  GDI_REGISTER(SetBitmapAttributes, NtGdiSetBitmapAttributes);
  GDI_REGISTER(ClearBitmapAttributes, NtGdiClearBitmapAttributes);
  GDI_REGISTER(SetBrushAttributes, NtGdiSetBrushAttributes);
  GDI_REGISTER(ClearBrushAttributes, NtGdiClearBrushAttributes);
  GDI_REGISTER(MonoBitmap, NtGdiMonoBitmap);
  GDI_REGISTER(CreateClientObj, NtGdiCreateClientObj);
  GDI_REGISTER(DeleteClientObj, NtGdiDeleteClientObj);
  GDI_REGISTER(DeleteObjectApp, NtGdiDeleteObjectApp);
  GDI_REGISTER(UnrealizeObject, NtGdiUnrealizeObject);
  GDI_REGISTER(ExtGetObjectW, NtGdiExtGetObjectW);
  GDI_REGISTER(CreateColorSpace, NtGdiCreateColorSpace);
  GDI_REGISTER(DeleteColorSpace, NtGdiDeleteColorSpace);
  GDI_REGISTER(SetColorSpace, NtGdiSetColorSpace);
}

}  // namespace process::win32k::gdi
