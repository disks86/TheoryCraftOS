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

// What layout needs to know about text without knowing about fonts: how tall a line of it is and how wide a piece of it is. The font
// stack (src/Font) implements this; the tests use FixedWidthMeasurer, whose numbers are easy to calculate by hand.
#pragma once
#include "Browser/Css.hpp"
#include "Layout/LayoutUnit.hpp"
#include "Utility/StringView.hpp"

namespace layout {

// The vertical metrics of a font at the size a style asks for: ascent and descent are the distances from the baseline to the top
// and bottom of the font's content area, line_gap the extra space the font recommends between lines.
struct TextMetrics {
  LayoutUnit ascent, descent, line_gap;
  LayoutUnit height() const { return ascent + descent; }
};

class ITextMeasurer {
 public:
  virtual TextMetrics metrics(const browser::Style& style) = 0;
  // The advance width of `text` (UTF-8 without line breaks) set in the style's font: weight, style, size, letter and word spacing
  // included.
  virtual LayoutUnit width(const browser::Style& style, StringView text) = 0;

 protected:
  ITextMeasurer() = default;
  ~ITextMeasurer() = default;
};

// Every character (code point) is half the font size wide, the ascent is 80% and the descent 20% of the font size, there is no line gap:
// a 16px font gives 8px characters on lines of 16px (line-height normal), the old character cell.
class FixedWidthMeasurer final : public ITextMeasurer {
 public:
  TextMetrics metrics(const browser::Style& style) override {
    TextMetrics m;
    const int32_t px100 = static_cast<int32_t>(style.font_px) * 100;
    m.ascent = LayoutUnit::from_px100(px100 * 80 / 100);
    m.descent = LayoutUnit::from_px100(px100 * 20 / 100);
    return m;
  }
  LayoutUnit width(const browser::Style& style, StringView text) override {
    int64_t characters = 0;
    for (uint64_t i = 0; i < text.size(); ++i)
      if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++characters;   // (UTF-8 continuation bytes are not characters)
    return LayoutUnit::from_px100(characters * style.font_px * 50);
  }
};

}  // namespace layout
