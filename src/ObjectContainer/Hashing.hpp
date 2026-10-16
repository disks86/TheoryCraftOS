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

// Hash and equality policies for Dictionary keys. Integers, enums and pointers
// are handled out of the box, as are C strings (compared by content; the
// dictionary stores only the pointer, so the string must outlive the entry).
// For other key types pass your own Hash/Equal functors to Dictionary.

// 64-bit finalizer (splitmix64): spreads the bits so low bits make good buckets.
constexpr uint64_t mix_hash(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

template <class K>
struct DefaultHash {
  uint64_t operator()(const K& key) const { return mix_hash(static_cast<uint64_t>(key)); }
};

template <class K>
struct DefaultHash<K*> {
  uint64_t operator()(K* key) const { return mix_hash(reinterpret_cast<uintptr_t>(key)); }
};

template <>
struct DefaultHash<const char*> {
  uint64_t operator()(const char* s) const {  // FNV-1a
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 0x100000001B3ull;
    return h;
  }
};

template <class K>
struct DefaultEqual {
  bool operator()(const K& a, const K& b) const { return a == b; }
};

template <>
struct DefaultEqual<const char*> {
  bool operator()(const char* a, const char* b) const {
    while (*a && *a == *b) {
      ++a;
      ++b;
    }
    return *a == *b;
  }
};
