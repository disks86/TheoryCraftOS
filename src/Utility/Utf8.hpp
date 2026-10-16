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
#include "Utility/FixedString.hpp"

// Appends the UTF-8 encoding of one Unicode code point. False (string unchanged
// as far as that character goes) if it does not fit.
template <uint64_t N>
bool append_utf8(FixedString<N>* out, uint32_t cp) {
  char bytes[4];
  uint32_t n;
  if (cp < 0x80) {
    bytes[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    bytes[0] = static_cast<char>(0xC0 | (cp >> 6));
    bytes[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    bytes[0] = static_cast<char>(0xE0 | (cp >> 12));
    bytes[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    bytes[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    bytes[0] = static_cast<char>(0xF0 | (cp >> 18));
    bytes[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    bytes[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    bytes[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  return out->append(StringView(bytes, n));  // all of it or none of it
}
