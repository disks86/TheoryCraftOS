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
#include "ObjectContainer/Array.hpp"

namespace network {

// Ranges of TCP sequence numbers [start, end), kept sorted and disjoint (touching ones merge): what a receiver holds beyond a gap, or what a
// sender knows the receiver holds (its SACK scoreboard). Comparisons are on the signed difference, so they work across the wrap at 2^32 for
// ranges within 2^31 of each other - a window's worth always is. At most kMax ranges: one that would need more is not added (add() says so).
class SequenceRanges {
 public:
  static constexpr uint32_t kMax = 8;
  struct Range {
    uint32_t start = 0, end = 0;
  };

  uint32_t count() const { return count_; }
  bool empty() const { return count_ == 0; }
  const Range& at(uint32_t i) const { return ranges_[i]; }
  void clear() { count_ = 0; }

  // Adds [start, end) (merged with what it overlaps or touches). False if it is empty or there is no room for one more range.
  bool add(uint32_t start, uint32_t end) {
    if (!before(start, end)) return false;
    uint32_t i = 0;
    while (i < count_ && before(ranges_[i].end, start)) ++i;   // ranges wholly before it
    uint32_t j = i;
    while (j < count_ && !before(end, ranges_[j].start)) {     // ranges it overlaps or touches: absorbed
      if (before(ranges_[j].start, start)) start = ranges_[j].start;
      if (before(end, ranges_[j].end)) end = ranges_[j].end;
      ++j;
    }
    const uint32_t absorbed = j - i;
    if (absorbed == 0 && count_ == kMax) return false;
    if (absorbed == 0) {
      for (uint32_t k = count_; k > i; --k) ranges_[k] = ranges_[k - 1];
      ++count_;
    } else {
      for (uint32_t k = j; k < count_; ++k) ranges_[k - absorbed + 1] = ranges_[k];
      count_ -= absorbed - 1;
    }
    ranges_[i].start = start;
    ranges_[i].end = end;
    return true;
  }

  // Forgets everything below `sequence` (a range across it is cut there).
  void remove_below(uint32_t sequence) {
    uint32_t drop = 0;
    while (drop < count_ && !before(sequence, ranges_[drop].end)) ++drop;
    for (uint32_t k = drop; k < count_; ++k) ranges_[k - drop] = ranges_[k];
    count_ -= drop;
    if (count_ && before(ranges_[0].start, sequence)) ranges_[0].start = sequence;
  }

  // The range that holds `sequence`, or null.
  const Range* find(uint32_t sequence) const {
    for (uint32_t i = 0; i < count_; ++i)
      if (!before(sequence, ranges_[i].start) && before(sequence, ranges_[i].end)) return &ranges_[i];
    return nullptr;
  }
  bool covers(uint32_t sequence) const { return find(sequence) != nullptr; }

  // Bytes in all ranges.
  uint32_t bytes() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count_; ++i) n += ranges_[i].end - ranges_[i].start;
    return n;
  }
  // The end of the last range (or `fallback` when there is none).
  uint32_t highest(uint32_t fallback) const { return count_ ? ranges_[count_ - 1].end : fallback; }

  // The first gap at or after `from` and below `limit`: [*gap_start, *gap_end). False if there is none.
  bool next_gap(uint32_t from, uint32_t limit, uint32_t* gap_start, uint32_t* gap_end) const {
    uint32_t at = from;
    for (uint32_t i = 0; i < count_ && before(at, limit); ++i) {
      if (!before(at, ranges_[i].end)) continue;          // this range is behind `at`
      if (before(at, ranges_[i].start)) {                 // a gap before it
        *gap_start = at;
        *gap_end = before(ranges_[i].start, limit) ? ranges_[i].start : limit;
        return true;
      }
      at = ranges_[i].end;                                // `at` is inside it: skip to its end
    }
    if (!before(at, limit)) return false;
    *gap_start = at;
    *gap_end = limit;
    return true;
  }

  static bool before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

 private:
  Array<Range, kMax + 1> ranges_;   // (one spare: add() shifts into it before it knows whether a merge frees one)
  uint32_t count_ = 0;
};

}  // namespace network
