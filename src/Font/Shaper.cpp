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
#include "Font/FontSet.hpp"
#include "Font/Shaper.hpp"

namespace font {

uint32_t decode_utf8(StringView text, uint64_t* pos) {
  if (*pos >= text.size()) return 0xFFFFFFFFu;
  const uint8_t lead = static_cast<uint8_t>(text[*pos]);
  ++*pos;
  if (lead < 0x80) return lead;
  uint32_t length, cp, minimum;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1F;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0F;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07;
    minimum = 0x10000;
  } else {
    return 0xFFFD;
  }
  if (text.size() - (*pos - 1) < length) return 0xFFFD;
  for (uint32_t i = 1; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[*pos - 1 + i]);
    if ((c & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (c & 0x3F);
  }
  if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
  *pos += length - 1;
  return cp;
}

namespace {

// Code points that produce no glyph and no advance: controls and invisible formatting characters.
bool is_invisible(uint32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return true;
  return cp == 0xAD || cp == 0x34F || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
         (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || (cp >= 0xE0100 && cp <= 0xE01EF);
}

Fixed clamp_fixed(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<Fixed>(v); }

}  // namespace

Fixed FontSet::advance_of(uint32_t face, uint32_t glyph, Fixed size) {
  if (face == kFaceBitmap) return size / 2;           // 8 of 16 em
  if (face == kFaceTofu) return (size * 3 + 2) / 5;   // 0.6 em
  const TrueTypeFont* tt = load(face);
  return tt ? tt->advance(glyph, size) : 0;
}

Fixed FontSet::shape_into(FontHandle font, StringView text, const ShapeOptions& options, GlyphRun* run, bool* ok) {
  *ok = true;
  Instance* instance = instance_at(font.instance);
  int64_t width = 0;
  uint32_t previous_face = kUnresolved, previous_glyph = 0;
  uint64_t pos = 0;
  while (pos < text.size()) {
    const uint32_t cluster = static_cast<uint32_t>(pos);
    uint32_t cp = decode_utf8(text, &pos);
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') cp = ' ';  // layout collapses white space before this; a stray one is a space
    if (is_invisible(cp)) continue;
    uint32_t glyph = 0;
    const uint32_t face = resolve(*instance, cp, &glyph);
    int64_t advance = advance_of(face, glyph, font.size);
    if (options.kerning && face == previous_face && face != kFaceBitmap && face != kFaceTofu) {
      if (const TrueTypeFont* tt = load(face)) {
        const Fixed k = tt->kerning(previous_glyph, glyph, font.size);
        if (k != 0) {
          width += k;
          if (run && run->glyphs.count()) run->glyphs[run->glyphs.count() - 1].advance = clamp_fixed(int64_t{run->glyphs[run->glyphs.count() - 1].advance} + k);
        }
      }
    }
    advance += options.letter_spacing;
    if (cp == ' ' || cp == 0xA0) advance += options.word_spacing;
    width += advance;
    if (run) {
      ShapedGlyph g;
      g.face = face;
      g.glyph = glyph;
      g.codepoint = cp;
      g.cluster = cluster;
      g.advance = clamp_fixed(advance);
      if (!run->glyphs.push_back(g)) {
        *ok = false;
        return clamp_fixed(width);
      }
    }
    previous_face = face;
    previous_glyph = glyph;
  }
  return clamp_fixed(width);
}

bool FontSet::shape(FontHandle font, StringView utf8, const ShapeOptions& options, GlyphRun* run) {
  run->clear();
  bool ok = true;
  const Fixed width = shape_into(font, utf8, options, run, &ok);
  run->width = width;
  return ok;
}

Fixed FontSet::measure(FontHandle font, StringView utf8, const ShapeOptions& options) {
  bool ok = true;
  return shape_into(font, utf8, options, nullptr, &ok);
}

}  // namespace font
