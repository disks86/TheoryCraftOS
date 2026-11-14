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

#include "Graphics/Graphics2D/CellTextFace.hpp"
#include "Font/BundledFonts.hpp"
#include "Font/GlyphRaster.hpp"
#include "Font/WoffDecode.hpp"

namespace graphics {

namespace {
constexpr uint64_t kMaxFontBytes = 8u << 20;
constexpr uint32_t kMonoFace = 1, kFallbackFace = 2;

// The first regular (not bold, not italic) face of a family, as a TrueType file.
bool load_regular(ArrayView<const font::BundledFontInfo> faces, ByteBuffer* sfnt, font::TrueTypeFont* font) {
  for (const font::BundledFontInfo& info : faces) {
    if (info.weight != 400 || info.italic) continue;
    return font::font_to_sfnt(info.woff, sfnt, kMaxFontBytes) && font->parse(sfnt->view());
  }
  return false;
}

// Box drawing, block elements and shades join from cell to cell: the 8x16 font's pixels do that, a scalable face's glyphs do not.
bool joins_cells(char32_t c) { return c >= 0x2500 && c <= 0x259F; }
}  // namespace

CellTextFace::CellTextFace(allocators::IAllocator& allocator)
    : allocator_(allocator), mono_face_(allocator), fallback_face_(allocator), mono_(mono_face_.font), cache_(allocator) {
  (void)load_regular(font::bundled_mono_faces(), &mono_face_.sfnt, &mono_face_.font);
  (void)load_regular(font::bundled_fallback_faces(), &fallback_face_.sfnt, &fallback_face_.font);
}

font::Fixed CellTextFace::size_for(const font::TrueTypeFont& f, uint32_t w, uint32_t h) const {
  const uint32_t em = f.units_per_em();
  const uint32_t m = f.glyph_for_codepoint('M');
  const uint32_t advance = m != 0 ? f.advance_units(m) : em / 2;
  const int64_t extent = int64_t{f.metrics().ascent} + f.metrics().descent;
  if (em == 0 || advance == 0 || extent <= 0) return 0;
  const int64_t by_width = int64_t{w} * font::kFixedOne * em / advance;
  const int64_t by_height = int64_t{h} * font::kFixedOne * em / extent;
  const int64_t size = by_width < by_height ? by_width : by_height;
  return size > 0 && size < (int64_t{1} << 20) ? font::Fixed(size) : 0;
}

bool CellTextFace::draw_cell(IRenderer2D& device, char32_t c, const Rect2D& cell, uint32_t color) {
  if (!mono_.valid() || joins_cells(c) || cell.width == 0 || cell.height == 0) return false;
  const font::TrueTypeFont* f = &mono_;
  uint32_t face = kMonoFace;
  uint32_t glyph = mono_.glyph_for_codepoint(uint32_t(c));
  if (glyph == 0 && fallback_face_.font.valid()) {
    glyph = fallback_face_.font.glyph_for_codepoint(uint32_t(c));
    f = &fallback_face_.font;
    face = kFallbackFace;
  }
  if (glyph == 0) return false;
  const font::Fixed size = size_for(*f, cell.width, cell.height);
  if (size == 0) return false;
  // the face's line box centred in the cell, the glyph's advance centred across it
  const font::ScaledMetrics sm = f->scaled_metrics(size);
  const int64_t pad = (int64_t{cell.height} * font::kFixedOne - (int64_t{sm.ascent} + sm.descent)) / 2;
  const int32_t baseline = cell.y + font::fixed_round(font::Fixed(pad + sm.ascent));
  const int64_t slack = int64_t{cell.width} * font::kFixedOne - f->advance(glyph, size);
  const int32_t pen = cell.x + font::fixed_round(font::Fixed(slack > 0 ? slack / 2 : 0));
  const font::GlyphKey key{face, glyph, size, 0};
  const font::GlyphBitmap* bitmap = cache_.find(key);
  if (!bitmap) {
    font::GlyphBitmap fresh(allocator_);
    if (!font::rasterize_glyph(*f, glyph, size, 0, allocator_, &fresh)) return false;
    bitmap = cache_.insert(key, static_cast<font::GlyphBitmap&&>(fresh));
    if (!bitmap) return false;
  }
  if (bitmap->empty()) return true;      // (a space)
  MaskView mask;
  mask.coverage = bitmap->pixels();
  mask.width = bitmap->width;
  mask.height = bitmap->height;
  mask.pitch = bitmap->pitch();
  if (!mask.valid()) return false;
  device.draw_mask(mask, Rect2D{0, 0, mask.width, mask.height}, pen + bitmap->left, baseline - bitmap->top, color);
  return true;
}

}  // namespace graphics
