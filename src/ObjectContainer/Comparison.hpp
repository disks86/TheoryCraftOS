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

// Ordering policies for the sorted containers. A "less" functor answers
// `less(a, b)`: does a sort before b? Elements are equivalent when neither sorts
// before the other.
template <class T>
struct DefaultLess {
  bool operator()(const T& a, const T& b) const { return a < b; }
};

// C strings order by content (not by address).
template <>
struct DefaultLess<const char*> {
  bool operator()(const char* a, const char* b) const {
    while (*a && *a == *b) {
      ++a;
      ++b;
    }
    return static_cast<unsigned char>(*a) < static_cast<unsigned char>(*b);
  }
};
