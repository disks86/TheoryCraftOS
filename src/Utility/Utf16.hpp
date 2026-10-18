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

// UTF-8 and UTF-16 (little endian, as protocols like SMB carry it).
#pragma once
#include <stdint.h>
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace util {

// Appends `text` (UTF-8) to `out` as UTF-16LE code units. False for malformed UTF-8 or memory shortage.
inline bool append_utf16le(ByteBuffer* out, StringView text) {
  uint64_t i = 0;
  while (i < text.size()) {
    const uint8_t b = static_cast<uint8_t>(text[i]);
    uint32_t cp, extra;
    if (b < 0x80) cp = b, extra = 0;
    else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1;
    else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2;
    else if ((b & 0xF8) == 0xF0) cp = b & 0x07, extra = 3;
    else return false;
    if (i + extra >= text.size() && extra != 0) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t c = static_cast<uint8_t>(text[i + k]);
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return false;
    i += extra + 1;
    if (cp >= 0x10000) {
      const uint32_t v = cp - 0x10000;
      if (!out->put16(0xD800 + (v >> 10)) || !out->put16(0xDC00 + (v & 0x3FF))) return false;
    } else if (!out->put16(cp)) {
      return false;
    }
  }
  return true;
}

// Appends UTF-16LE `units` (a byte count of 2 * characters) to `out` as UTF-8. Unpaired surrogates become U+FFFD. False on memory shortage.
inline bool append_utf8_from_utf16le(String* out, ConstByteView units) {
  const uint64_t count = units.size() / 2;
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t cp = units[2 * i] | (units[2 * i + 1] << 8);
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count) {
      const uint32_t low = units[2 * i + 2] | (units[2 * i + 3] << 8);
      if (low >= 0xDC00 && low < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
    }
    char bytes[4];
    uint32_t n;
    if (cp < 0x80) { bytes[0] = static_cast<char>(cp); n = 1; }
    else if (cp < 0x800) { bytes[0] = static_cast<char>(0xC0 | (cp >> 6)); bytes[1] = static_cast<char>(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { bytes[0] = static_cast<char>(0xE0 | (cp >> 12)); bytes[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); bytes[2] = static_cast<char>(0x80 | (cp & 0x3F)); n = 3; }
    else { bytes[0] = static_cast<char>(0xF0 | (cp >> 18)); bytes[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); bytes[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); bytes[3] = static_cast<char>(0x80 | (cp & 0x3F)); n = 4; }
    if (!out->append(StringView(bytes, n))) return false;
  }
  return true;
}

}  // namespace util
