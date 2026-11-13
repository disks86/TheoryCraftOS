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

// File URIs (RFC 8089) as drag and drop carries them (text/uri-list, RFC 2483): the Wayland compositor and the X server hand the desktop's files to
// Linux programs this way and read theirs back.
#pragma once
#include <stdint.h>
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace util {

namespace detail {
inline bool uri_unreserved(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}
inline int uri_hex_value(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace detail

// file://<path> into *out, every byte but the unreserved ones (and '/') percent-encoded, then CR LF (one line of a URI list). False on out-of-memory.
inline bool file_uri(StringView path, String* out) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  if (!out->assign(StringView("file://"))) return false;
  for (const uint8_t c : path.bytes()) {
    if (detail::uri_unreserved(c)) {
      if (!out->push_back(static_cast<char>(c))) return false;
    } else if (!out->push_back('%') || !out->push_back(kHex[c >> 4]) || !out->push_back(kHex[c & 15])) {
      return false;
    }
  }
  return out->append(StringView("\r\n"));
}

// The path of a file URI (file:///x or file://localhost/x), percent-decoded, into *out; false for another scheme or host, a bad escape, a NUL or an
// empty path.
inline bool path_of_file_uri(StringView uri, String* out) {
  StringView rest;
  if (uri.size() >= 8 && uri.first(8) == StringView("file:///")) rest = uri.without_prefix(7);
  else if (uri.size() >= 17 && uri.first(17) == StringView("file://localhost/")) rest = uri.without_prefix(16);
  else return false;
  out->clear();
  const ConstByteView b = rest.bytes();
  for (uint64_t i = 0; i < b.size(); ++i) {
    uint8_t c = b[i];
    if (c == '%') {
      if (b.size() - i < 3) return false;
      const int hi = detail::uri_hex_value(b[i + 1]), lo = detail::uri_hex_value(b[i + 2]);
      if (hi < 0 || lo < 0) return false;
      c = static_cast<uint8_t>(hi * 16 + lo);
      i += 2;
    }
    if (c == 0 || !out->push_back(static_cast<char>(c))) return false;
  }
  return !out->empty();
}

}  // namespace util
