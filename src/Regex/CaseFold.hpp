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

// Case-insensitive matching beyond ASCII: the code points that match a given one when a pattern ignores case. Without the u flag two characters match when their
// upper case (one code point, and not ASCII for a non-ASCII character) is the same; with it, when their simple case folding is (approximated by lower(upper(c)),
// which gives K (Kelvin sign) -> k, long s -> s, final sigma -> sigma). The data is the simple case mapping of src/Script/UnicodeTables.hpp.
#pragma once
#include <stdint.h>
#include "Script/UnicodeTables.hpp"

namespace regex {
namespace casefold {

namespace t = script::unicode_tables;

template <uint32_t N>
constexpr uint32_t map_of(const uint32_t (&table)[N][2], uint32_t count, uint32_t cp) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (table[mid][0] < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < count && table[lo][0] == cp ? table[lo][1] : cp;
}

constexpr uint32_t simple_upper(uint32_t cp) { return map_of(t::kUpper, t::kUpperCount, cp); }
constexpr uint32_t simple_lower(uint32_t cp) { return map_of(t::kLower, t::kLowerCount, cp); }

// The representative of a character's class of equal-ignoring-case characters.
constexpr uint32_t canonical(uint32_t cp, bool unicode) {
  if (unicode) {
    if (cp == 0x130 || cp == 0x131) return cp;   // (the dotted and dotless i fold only in the Turkic mapping)
    return simple_lower(simple_upper(cp));
  }
  const uint32_t u = simple_upper(cp);
  return cp >= 0x80 && u < 0x80 ? cp : u;
}

constexpr uint32_t kMaxVariants = 8;

// cp and every other code point with the same canonical form, in out (at most kMaxVariants); the count.
constexpr uint32_t variants(uint32_t cp, bool unicode, uint32_t (&out)[kMaxVariants]) {
  uint32_t n = 0;
  auto add = [&](uint32_t c) {
    for (uint32_t i = 0; i < n; ++i)
      if (out[i] == c) return;
    if (n < kMaxVariants) out[n++] = c;
  };
  add(cp);
  const uint32_t key = canonical(cp, unicode);
  add(key);
  auto consider = [&](uint32_t c) {
    if (canonical(c, unicode) == key) add(c);
  };
  for (uint32_t i = 0; i < t::kUpperCount; ++i) {
    consider(t::kUpper[i][0]);
    consider(t::kUpper[i][1]);
  }
  for (uint32_t i = 0; i < t::kLowerCount; ++i) {
    consider(t::kLower[i][0]);
    consider(t::kLower[i][1]);
  }
  return n;
}

}  // namespace casefold
}  // namespace regex
