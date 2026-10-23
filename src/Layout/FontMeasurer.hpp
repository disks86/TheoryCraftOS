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

// The text measurer the browser uses: widths and heights come from the font stack (src/Font), chosen by the family list, weight, style, stretch and
// size a style asks for. Font units and LayoutUnit are both 1/64 px, so the numbers pass through unchanged.
#pragma once
#include "Browser/Css.hpp"
#include "Font/FontSet.hpp"
#include "Layout/TextMeasure.hpp"

namespace layout {

class FontMeasurer final : public ITextMeasurer {
 public:
  // `sheet` is where the families of a style are kept as text (Style::font_family); nullptr: every style gets the default font.
  FontMeasurer(font::FontSet& fonts, const browser::StyleSheet* sheet) : fonts_(fonts), sheet_(sheet) {}

  TextMetrics metrics(const browser::Style& style) override;
  LayoutUnit width(const browser::Style& style, StringView text) override;

  // The font a style asks for (cached: styles of a page are few).
  font::FontHandle handle_for(const browser::Style& style);
  font::ShapeOptions shape_options(const browser::Style& style) const;

 private:
  struct CacheEntry {
    bool used = false;
    uint32_t family_offset = 0, family_length = 0;
    uint16_t weight = 0, stretch = 0, size = 0;
    bool italic = false, code = false;
    font::FontHandle handle;
  };
  static constexpr uint32_t kCacheSize = 64;

  font::FontSet& fonts_;
  const browser::StyleSheet* sheet_;
  CacheEntry cache_[kCacheSize];
};

}  // namespace layout
