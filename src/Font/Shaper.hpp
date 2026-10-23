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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Font/FontTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/StringView.hpp"

// Text -> positioned glyphs. The shaper is deliberately simple: UTF-8 is decoded to code points, each code point
// is mapped to a glyph of the first font in the CSS fallback chain that has one, advances come from the font,
// adjacent glyphs of the same font are kerned from its 'kern' table, and letter-spacing / word-spacing are added.
// There is no ligature, mark-positioning, complex-script or bidi handling (a later phase). The entry points are
// FontSet::shape() and FontSet::measure() (FontSet.hpp).
namespace font {

struct ShapeOptions {
  Fixed letter_spacing = 0;  // CSS letter-spacing, added after every glyph (can be negative)
  Fixed word_spacing = 0;    // CSS word-spacing, added after every U+0020 and U+00A0
  bool kerning = true;       // font-kerning: normal (true) / none (false)
};

// One glyph of a run. The glyph is drawn with its origin at the sum of the advances before it.
struct ShapedGlyph {
  uint32_t face = 0;       // FontSet face id, or FontSet::kFaceBitmap / FontSet::kFaceTofu
  uint32_t glyph = 0;      // glyph index in that face (for the two special faces: the code point)
  uint32_t codepoint = 0;  // the Unicode scalar this glyph stands for
  uint32_t cluster = 0;    // byte offset of that character in the UTF-8 text (for caret and selection mapping)
  Fixed advance = 0;       // distance to the next glyph: font advance + kerning + letter/word-spacing
};

class GlyphRun {
 public:
  explicit GlyphRun(allocators::IAllocator& allocator) : glyphs(allocator) {}
  void clear() {
    glyphs.clear();
    width = 0;
  }
  List<ShapedGlyph> glyphs;
  Fixed width = 0;  // sum of the advances
};

// Decodes the Unicode scalar at `*pos` and advances past it. Malformed input (stray continuation byte, overlong
// form, surrogate, beyond U+10FFFF, truncation) yields U+FFFD and consumes one byte. Returns 0xFFFFFFFF at the end.
uint32_t decode_utf8(StringView text, uint64_t* pos);

}  // namespace font
