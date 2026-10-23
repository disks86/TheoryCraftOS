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

#include "Layout/FontMeasurer.hpp"

namespace layout {

namespace {

font::Fixed fixed_from_px100(int32_t px100) {
  const int64_t v = static_cast<int64_t>(px100) * font::kFixedOne;
  return static_cast<font::Fixed>(v >= 0 ? (v + 50) / 100 : -((-v + 50) / 100));
}

}  // namespace

font::FontHandle FontMeasurer::handle_for(const browser::Style& style) {
  const uint16_t stretch = static_cast<uint16_t>(style.font_stretch_x10 / 10);
  // a direct-mapped cache keyed by what picks a font: where the family text is, weight, stretch, size, italic, and the monospace default of <code>
  const uint32_t key = (style.font_family.offset * 31u + style.font_family.length * 17u + style.font_weight * 7u + style.font_px * 13u + (style.italic ? 5u : 0u) +
                        (style.code ? 3u : 0u) + stretch) % kCacheSize;
  CacheEntry& e = cache_[key];
  if (e.used && e.family_offset == style.font_family.offset && e.family_length == style.font_family.length && e.weight == style.font_weight &&
      e.stretch == stretch && e.size == style.font_px && e.italic == style.italic && e.code == style.code)
    return e.handle;
  font::FontQuery query;
  StringView families = sheet_ ? sheet_->text(style.font_family) : StringView();
  if (families.empty() && style.code) families = StringView("monospace");   // (code, pre, kbd and samp use the monospace font unless the page says otherwise)
  query.families = families;
  query.weight = style.font_weight ? style.font_weight : (style.bold ? 700 : 400);
  query.style = style.italic ? font::FontStyle::Italic : font::FontStyle::Normal;
  query.stretch = stretch ? stretch : 100;
  query.size = font::fixed_from_px(style.font_px ? style.font_px : 16);
  query.default_generic = font::GenericFamily::Serif;
  e.handle = fonts_.match(query);
  e.used = true;
  e.family_offset = style.font_family.offset;
  e.family_length = style.font_family.length;
  e.weight = style.font_weight;
  e.stretch = stretch;
  e.size = style.font_px;
  e.italic = style.italic;
  e.code = style.code;
  return e.handle;
}

font::ShapeOptions FontMeasurer::shape_options(const browser::Style& style) const {
  font::ShapeOptions options;
  options.letter_spacing = fixed_from_px100(style.letter_spacing_px100);
  options.word_spacing = fixed_from_px100(style.word_spacing_px100);
  return options;
}

TextMetrics FontMeasurer::metrics(const browser::Style& style) {
  const font::ScaledMetrics m = fonts_.metrics(handle_for(style));
  TextMetrics out;
  out.ascent = LayoutUnit::from_raw(m.ascent);
  out.descent = LayoutUnit::from_raw(m.descent);
  out.line_gap = LayoutUnit::from_raw(m.line_gap);
  return out;
}

LayoutUnit FontMeasurer::width(const browser::Style& style, StringView text) {
  return LayoutUnit::from_raw(fonts_.measure(handle_for(style), text, shape_options(style)));
}

}  // namespace layout
