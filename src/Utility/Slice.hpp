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
#include "Utility/Bounds.hpp"

// A range inside some array: where it starts and how many elements it covers.
// A Slice does not know which array it refers to (that is the ArrayView's job:
// view.subview(slice)), so it can be stored, passed around and combined freely,
// e.g. the position of a field inside a packet that is parsed again later.
//
// Slices are plain values; whether one fits an array is checked when it is applied
// (fits_in / ArrayView::subview). All arithmetic here is overflow-safe: a slice
// built from absurd numbers simply never fits anything.
struct Slice {
  uint64_t start = 0;
  uint64_t length = 0;

  constexpr Slice() = default;
  constexpr Slice(uint64_t start_index, uint64_t element_count) : start(start_index), length(element_count) {}
  // The elements from `begin` up to (not including) `end`; empty if end < begin.
  static constexpr Slice between(uint64_t begin, uint64_t end) { return end > begin ? Slice(begin, end - begin) : Slice(begin, 0); }

  constexpr bool empty() const { return length == 0; }
  // One past the last element. Only meaningful for a slice that fits something.
  constexpr uint64_t end() const { return start + length; }
  // True if start + length does not wrap around.
  constexpr bool valid() const { return length <= ~uint64_t{0} - start; }
  constexpr bool contains(uint64_t index) const { return index >= start && index - start < length; }
  // True if every element lies inside an array of `size` elements.
  constexpr bool fits_in(uint64_t size) const { return start <= size && length <= size - start; }
  constexpr bool overlaps(const Slice& other) const {
    return length != 0 && other.length != 0 && contains_start(other);
  }

  // The part of this slice that lies inside an array of `size` elements (possibly empty).
  constexpr Slice clamped_to(uint64_t size) const {
    if (start >= size) return Slice(size, 0);
    return Slice(start, length < size - start ? length : size - start);
  }

  // The `count` elements `offset` elements into this slice. False if they are not all inside it.
  constexpr bool try_sub(uint64_t offset, uint64_t count, Slice* out) const {
    if (offset > length || count > length - offset) return false;
    *out = Slice(start + offset, count);
    return true;
  }
  // Same, but a request outside the slice stops the OS.
  Slice sub(uint64_t offset, uint64_t count) const {
    Slice result;
    if (!try_sub(offset, count, &result)) bounds_violation("Slice::sub", offset + count, length);
    return result;
  }
  // Everything after the first `count` elements.
  Slice dropped(uint64_t count) const { return sub(count, length - (count < length ? count : length)); }

  constexpr bool operator==(const Slice& o) const { return start == o.start && length == o.length; }
  constexpr bool operator!=(const Slice& o) const { return !(*this == o); }

 private:
  constexpr bool contains_start(const Slice& other) const {
    return start < other.start ? other.start - start < length : start - other.start < other.length;
  }
};
