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

#include "Font/SystemFontService.hpp"
#include "Font/GlyphRaster.hpp"

namespace font {

using namespace process::windowsabi;

namespace {
constexpr int32_t kMaxSize = 700 * kFixedOne;      // (the rasteriser leaves a bigger glyph blank)
}

SystemFontService::SystemFontService(allocators::IAllocator& allocator) : allocator_(allocator), fonts_(allocator) { fonts_.add_bundled_fonts(); }

uint32_t SystemFontService::face_count() { return fonts_.face_count(); }

bool SystemFontService::face_info(uint32_t face, FontFaceInfo* out) {
  FaceInfo info;
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f || !fonts_.face_info(face, &info)) return false;
  const FaceMetrics& m = f->metrics();
  out->units_per_em = m.units_per_em;
  out->ascent = m.ascent;
  out->descent = m.descent;
  out->line_gap = m.line_gap;
  out->x_height = m.x_height;
  out->cap_height = m.cap_height;
  out->glyph_count = f->glyph_count();
  out->weight = info.weight;
  out->stretch = info.stretch;
  out->italic = info.style != FontStyle::Normal;
  out->bold = info.weight >= 600;
  out->bundled = info.bundled;
  out->monospaced = f->advance_units(f->glyph_for_codepoint('i')) == f->advance_units(f->glyph_for_codepoint('W')) && f->glyph_for_codepoint('i') != 0;
  return true;
}

bool SystemFontService::face_string(uint32_t face, uint32_t which, String* out) {
  FaceInfo info;
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f || !fonts_.face_info(face, &info)) return false;
  switch (which) {
    case 0: return out->assign(!info.bundled && !f->family_name().empty() ? f->family_name() : info.family);      // (a program's font is named by its own name table)
    case 1: return out->assign(f->style_name());
    case 2: return out->assign(f->full_name());
    default: return out->assign(f->postscript_name());
  }
}

bool SystemFontService::map_codepoint(uint32_t face, uint32_t codepoint, uint32_t* glyph) {
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f) return false;
  *glyph = f->glyph_for_codepoint(codepoint);
  return true;
}

bool SystemFontService::glyph_box(uint32_t face, uint32_t glyph, FontGlyphBox* out) {
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f || glyph >= f->glyph_count()) return false;
  *out = FontGlyphBox();
  out->advance = int32_t(f->advance_units(glyph));
  Outline outline(allocator_);
  if (!f->load_outline(glyph, &outline) || outline.points.count() == 0) return true;
  int32_t x0 = outline.points[0].x, x1 = x0, y0 = outline.points[0].y, y1 = y0;
  for (const OutlinePoint& p : outline.points.view()) {
    if (p.x < x0) x0 = p.x;
    if (p.x > x1) x1 = p.x;
    if (p.y < y0) y0 = p.y;
    if (p.y > y1) y1 = p.y;
  }
  out->x_min = x0;
  out->y_min = y0;
  out->x_max = x1;
  out->y_max = y1;
  return true;
}

bool SystemFontService::kerning(uint32_t face, uint32_t left, uint32_t right, int32_t* units) {
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f) return false;
  *units = f->kerning_units(left, right);
  return true;
}

bool SystemFontService::rasterize(uint32_t face, uint32_t glyph, int32_t size, uint32_t subpixel, FontBitmap* out) {
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f || size <= 0 || size > kMaxSize || glyph >= f->glyph_count()) return false;
  GlyphBitmap bitmap(allocator_);
  if (!rasterize_glyph(*f, glyph, size, subpixel, allocator_, &bitmap)) return false;
  out->left = bitmap.left;
  out->top = bitmap.top;
  out->width = bitmap.width;
  out->height = bitmap.height;
  out->pixels.clear();
  return out->pixels.append(bitmap.pixels().first(uint64_t{bitmap.width} * bitmap.height));
}

bool SystemFontService::outline(uint32_t face, uint32_t glyph, ByteBuffer* contour_ends, ByteBuffer* points) {
  const TrueTypeFont* f = fonts_.font_of(face);
  if (!f || glyph >= f->glyph_count()) return false;
  Outline outline(allocator_);
  if (!f->load_outline(glyph, &outline)) return false;
  for (uint32_t end : outline.contour_ends.view())
    if (!contour_ends->put32(end)) return false;
  for (const OutlinePoint& p : outline.points.view())
    if (!points->put32(uint32_t(p.x)) || !points->put32(uint32_t(p.y)) || !points->put32(p.on_curve ? 1 : 0)) return false;
  return true;
}

bool SystemFontService::font_data(uint32_t face, uint64_t offset, ByteView out, uint64_t* total, uint64_t* copied) {
  const ConstByteView file = fonts_.sfnt_of(face);
  if (file.empty()) return false;
  *total = file.size();
  *copied = 0;
  if (offset >= file.size()) return true;
  const uint64_t n = file.size() - offset < out.size() ? file.size() - offset : out.size();
  if (n) out.first(n).copy_from(file.subview(offset, n));
  *copied = n;
  return true;
}

bool SystemFontService::add_font(ConstByteView data, uint32_t* face) {
  WebFontDescriptor d;
  d.family = "Program Font";
  d.style_from_file = true;
  return fonts_.add_web_font(d, data, face);
}

}  // namespace font
