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

namespace font {

// One font file embedded in the image (a WOFF 1.0 subset made by scripts/make_fonts.py), the way Font8x16 embeds
// its glyphs. See LICENSE-Liberation.txt and LICENSE-Noto.txt for the licences of the fonts.
struct BundledFontInfo {
  const char* family;  // "TCOS Sans", "TCOS Serif", "TCOS Mono" or "TCOS Fallback"
  uint16_t weight;     // 400 or 700
  bool italic;
  ConstByteView woff;
};

// The generated data files (FontData*.cpp), one family each. Sans, serif and mono come in regular, bold, italic
// and bold italic; the fallback is a single wide-coverage face (Latin extended, Greek, Cyrillic, punctuation,
// arrows, mathematical operators, box drawing, shapes).
ArrayView<const BundledFontInfo> bundled_sans_faces();
ArrayView<const BundledFontInfo> bundled_serif_faces();
ArrayView<const BundledFontInfo> bundled_mono_faces();
ArrayView<const BundledFontInfo> bundled_fallback_faces();

}  // namespace font
