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
#include "Utility/ArrayView.hpp"

namespace hardware {

// Fixed 8x16 font. glyph() returns 16 rows, one byte each, MSB = leftmost
// pixel; characters outside printable ASCII render as '?'.
// glyph_of() takes a Unicode character: ASCII as glyph(), the letters of Latin-1 Supplement and Latin Extended-A (U+00C0..U+017F) made of
// their base letter and an accent, a few symbols and punctuation marks as their nearest ASCII look-alike, '?' otherwise.
struct Font8x16 {
  static constexpr uint32_t kWidth = 8;
  static constexpr uint32_t kHeight = 16;
  static ConstByteView glyph(char c);  // kHeight bytes
  static void glyph_of(char32_t c, uint8_t (&out)[kHeight]);
};

// The next character of UTF-8 text (an invalid or cut-off sequence is one U+FFFD and one byte); advances `at`. 0 at the end (a NUL byte).
char32_t utf8_next(const char*& at);
// How many characters (not bytes) UTF-8 text has: what fixed-width text is as wide as.
uint64_t utf8_length(ConstByteView text);

}  // namespace hardware
