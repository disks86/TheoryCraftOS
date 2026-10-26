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

// Case mapping and normalization of text, on code points (the tables are in UnicodeTables.hpp, made by scripts/gen_unicode_text_tables.py). Included by
// Builtins.cpp only.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Script/UnicodeTables.hpp"

namespace script {
namespace unicode_text {

namespace t = unicode_tables;

// The row of a sorted table of rows (key first) whose key is `cp`, or null.
template <uint32_t N>
inline const uint32_t* find_row(const uint32_t (&table)[N][2], uint32_t count, uint32_t cp) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (table[mid][0] < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < count && table[lo][0] == cp ? table[lo] : nullptr;
}
template <uint32_t N, uint32_t W>
inline const uint32_t* find_wide_row(const uint32_t (&table)[N][W], uint32_t count, uint32_t cp) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (table[mid][0] < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < count && table[lo][0] == cp ? table[lo] : nullptr;
}
template <uint32_t N>
inline bool in_ranges(const uint32_t (&table)[N][2], uint32_t count, uint32_t cp) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (table[mid][1] < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < count && table[lo][0] <= cp;
}

inline bool is_cased(uint32_t cp) { return in_ranges(t::kCased, t::kCasedCount, cp); }
inline bool is_case_ignorable(uint32_t cp) { return in_ranges(t::kCaseIgnorable, t::kCaseIgnorableCount, cp); }

// Upper or lower case of the string `in`, with the multi-character mappings (ß is SS) and the final sigma rule.
inline void convert_case(const List<uint32_t>& in, bool upper, List<uint32_t>* out) {
  const uint64_t n = in.count();
  for (uint64_t i = 0; i < n; ++i) {
    const uint32_t cp = in[i];
    if (cp < 0x80) {
      out->push_back(upper ? (cp >= 'a' && cp <= 'z' ? cp - 32 : cp) : (cp >= 'A' && cp <= 'Z' ? cp + 32 : cp));
      continue;
    }
    if (!upper && cp == 0x3A3) {   // final sigma: after a cased letter and not before one (case-ignorable characters do not count)
      bool before = false, after = false;
      for (uint64_t k = i; k > 0 && !before; --k) {
        const uint32_t c = in[k - 1];
        if (is_case_ignorable(c)) continue;
        before = is_cased(c);
        break;
      }
      for (uint64_t k = i + 1; k < n; ++k) {
        const uint32_t c = in[k];
        if (is_case_ignorable(c)) continue;
        after = is_cased(c);
        break;
      }
      out->push_back(before && !after ? 0x3C2 : 0x3C3);
      continue;
    }
    const uint32_t* multi = upper ? find_wide_row(t::kUpperMulti, t::kUpperMultiCount, cp) : find_wide_row(t::kLowerMulti, t::kLowerMultiCount, cp);
    if (multi) {
      for (int k = 1; k <= 3 && multi[k]; ++k) out->push_back(multi[k]);
      continue;
    }
    const uint32_t* row = upper ? find_row(t::kUpper, t::kUpperCount, cp) : find_row(t::kLower, t::kLowerCount, cp);
    out->push_back(row ? row[1] : cp);
  }
}

inline uint32_t combining_class(uint32_t cp) {
  if (cp < 0x300) return 0;
  const uint32_t* row = find_row(t::kCombining, t::kCombiningCount, cp);
  return row ? row[1] : 0;
}

constexpr uint32_t kHangulBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7, kLCount = 19, kVCount = 21, kTCount = 28, kNCount = kVCount * kTCount,
                   kSCount = kLCount * kNCount;

inline void decompose_into(uint32_t cp, bool compatibility, List<uint32_t>* out) {
  if (cp < 0xA0) {
    out->push_back(cp);
    return;
  }
  if (cp >= kHangulBase && cp < kHangulBase + kSCount) {
    const uint32_t s = cp - kHangulBase;
    out->push_back(kLBase + s / kNCount);
    out->push_back(kVBase + (s % kNCount) / kTCount);
    if (s % kTCount) out->push_back(kTBase + s % kTCount);
    return;
  }
  const uint32_t* row = compatibility ? find_wide_row(t::kCompatibilityDecomposition, t::kCompatibilityDecompositionCount, cp)
                                      : find_wide_row(t::kCanonicalDecomposition, t::kCanonicalDecompositionCount, cp);
  if (!row) {
    out->push_back(cp);
    return;
  }
  const uint32_t* pool = compatibility ? t::kCompatibilityDecompositionPool : t::kCanonicalDecompositionPool;
  for (uint32_t k = 0; k < row[2]; ++k) out->push_back(pool[row[1] + k]);
}

inline bool compose_pair(uint32_t first, uint32_t second, uint32_t* composite) {
  if (first >= kLBase && first < kLBase + kLCount && second >= kVBase && second < kVBase + kVCount) {
    *composite = kHangulBase + ((first - kLBase) * kVCount + (second - kVBase)) * kTCount;
    return true;
  }
  if (first >= kHangulBase && first < kHangulBase + kSCount && (first - kHangulBase) % kTCount == 0 && second > kTBase && second < kTBase + kTCount) {
    *composite = first + (second - kTBase);
    return true;
  }
  uint32_t lo = 0, hi = t::kCompositionCount;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const uint32_t* r = t::kComposition[mid];
    if (r[0] < first || (r[0] == first && r[1] < second)) lo = mid + 1;
    else hi = mid;
  }
  if (lo < t::kCompositionCount && t::kComposition[lo][0] == first && t::kComposition[lo][1] == second) {
    *composite = t::kComposition[lo][2];
    return true;
  }
  return false;
}

// form: 0 NFC, 1 NFD, 2 NFKC, 3 NFKD.
inline void normalize(const List<uint32_t>& in, int form, List<uint32_t>* out, allocators::IAllocator& alloc) {
  const bool compatibility = form >= 2, compose = form == 0 || form == 2;
  List<uint32_t> d(alloc);
  for (uint64_t i = 0; i < in.count(); ++i) decompose_into(in[i], compatibility, &d);
  // canonical ordering: runs of combining marks are sorted by class (stable)
  for (uint64_t i = 1; i < d.count(); ++i) {
    const uint32_t cc = combining_class(d[i]);
    if (cc == 0) continue;
    uint64_t k = i;
    while (k > 0 && combining_class(d[k - 1]) > cc) {
      const uint32_t tmp = d[k - 1];
      d[k - 1] = d[k];
      d[k] = tmp;
      --k;
    }
  }
  if (!compose) {
    for (uint64_t i = 0; i < d.count(); ++i) out->push_back(d[i]);
    return;
  }
  // canonical composition
  uint64_t starter = d.count();   // the index (in `out`) of the last starter, or none
  uint32_t last_class = 0;
  const uint64_t base = out->count();
  for (uint64_t i = 0; i < d.count(); ++i) {
    const uint32_t cp = d[i];
    const uint32_t cc = combining_class(cp);
    if (starter != d.count()) {
      uint32_t composite;
      const bool blocked = last_class != 0 && last_class >= cc;
      if (!blocked && compose_pair((*out)[starter], cp, &composite)) {
        (*out)[starter] = composite;
        continue;
      }
    }
    if (cc == 0) {
      starter = out->count();
      last_class = 0;
    } else {
      last_class = cc;
    }
    out->push_back(cp);
  }
  (void)base;
}

}  // namespace unicode_text
}  // namespace script
