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

#include "Paint/DisplayPainter.hpp"

namespace paint {

void DisplayPainter::draw_text(graphics::IRenderer2D& r, font::FontHandle font, StringView text, int32_t x64, int32_t baseline, uint32_t color, int32_t letter64,
                               int32_t word64) {
  font::GlyphRun& run = run_;
  font::ShapeOptions options;
  options.letter_spacing = letter64;
  options.word_spacing = word64;
  if (!fonts_.shape(font, text, options, &run)) return;
  int32_t pen = x64;
  for (uint64_t i = 0; i < run.glyphs.count(); ++i) {
    const font::ShapedGlyph& g = run.glyphs[i];
    const uint32_t subpixel = static_cast<uint32_t>(((pen % 64) + 64) % 64) / 16;
    const font::GlyphBitmap* bitmap = fonts_.glyph_bitmap(font, g.face, g.glyph, subpixel);
    if (bitmap && !bitmap->empty()) {
      graphics::MaskView mask;
      mask.coverage = bitmap->pixels();
      mask.width = bitmap->width;
      mask.height = bitmap->height;
      mask.pitch = bitmap->pitch();
      const int32_t whole = (pen >= 0 ? pen / 64 : -((-pen + 63) / 64));
      // (the glyph's identity - which font set, face, glyph, size, phase - lets a renderer keep its picture)
      uint64_t id = fonts_.serial();
      const uint64_t parts[4] = {g.face, g.glyph, static_cast<uint32_t>(font.size), subpixel};
      for (const uint64_t part : parts) id = (id ^ part) * 1099511628211ull + 0x9E3779B97F4A7C15ull;
      r.draw_glyph(id, mask, whole + bitmap->left, baseline - bitmap->top, color);
    }
    pen += g.advance;
  }
}

font::FontHandle DisplayPainter::font_of(const DisplayList& list, uint32_t index) {
  if (index >= list.font_count()) return fonts_.match(font::FontQuery());
  while (resolved_.count() < list.font_count()) {
    resolved_.push_back(font::FontHandle());
    have_.push_back(0);
  }
  if (!have_[index]) {
    const FontSpec& spec = list.font_spec(index);
    font::FontQuery q;
    q.families = spec.families.view();
    q.weight = spec.weight;
    q.style = spec.style;
    q.stretch = spec.stretch;
    q.size = spec.size;
    q.default_generic = spec.default_generic;
    resolved_[index] = fonts_.match(q);
    have_[index] = 1;
  }
  return resolved_[index];
}

bool DisplayPainter::paint(graphics::IRenderer2D& r, const DisplayList& list, int32_t dx, int32_t dy, const graphics::Rect2D& view) {
  resolved_.clear();   // (the font table belongs to this list: match its fonts again)
  have_.clear();
  r.set_blend(graphics::BlendMode::Alpha);
  r.set_clip(view);
  const IRect view_rect{view.x, view.y, static_cast<int32_t>(view.width), static_cast<int32_t>(view.height)};
  return run(r, list, 0, list.count(), dx, dy, view_rect, 0);
}

}  // namespace paint
