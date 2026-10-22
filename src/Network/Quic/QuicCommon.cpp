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

#include "Network/Quic/QuicCommon.hpp"

namespace network::quic {

bool RangeSet::add(uint64_t first, uint64_t last) {
  if (first > last) return true;
  // find where it goes: ranges before it that end more than one below `first`
  uint64_t i = 0;
  while (i < ranges_.count() && ranges_[i].last + 1 < first) ++i;
  if (i == ranges_.count() || ranges_[i].first > last + 1) return ranges_.insert(i, Range{first, last});
  // it touches ranges_[i]; absorb every range it reaches
  uint64_t merged_first = ranges_[i].first < first ? ranges_[i].first : first, merged_last = ranges_[i].last > last ? ranges_[i].last : last;
  uint64_t j = i + 1;
  while (j < ranges_.count() && ranges_[j].first <= merged_last + 1) {
    if (ranges_[j].last > merged_last) merged_last = ranges_[j].last;
    ++j;
  }
  ranges_[i].first = merged_first;
  ranges_[i].last = merged_last;
  for (uint64_t k = i + 1; k < j; ++k) ranges_.remove_at(i + 1);
  return true;
}

void RangeSet::remove_below(uint64_t v) {
  while (ranges_.count() > 0 && ranges_[0].last < v) ranges_.remove_at(0);
  if (ranges_.count() > 0 && ranges_[0].first < v) ranges_[0].first = v;
}

bool RangeSet::remove(uint64_t first, uint64_t last) {
  if (first > last) return true;
  for (uint64_t i = 0; i < ranges_.count();) {
    Range& r = ranges_[i];
    if (r.last < first || r.first > last) {
      ++i;
    } else if (r.first >= first && r.last <= last) {   // inside the removed part
      ranges_.remove_at(i);
    } else if (r.first < first && r.last > last) {     // the removed part is inside it: two are left
      const Range tail{last + 1, r.last};
      r.last = first - 1;
      if (!ranges_.insert(i + 1, tail)) return false;
      i += 2;
    } else if (r.first < first) {                       // its end is removed
      r.last = first - 1;
      ++i;
    } else {                                            // its start is removed
      r.first = last + 1;
      ++i;
    }
  }
  return true;
}

}  // namespace network::quic
