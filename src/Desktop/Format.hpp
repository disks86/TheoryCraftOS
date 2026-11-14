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

// Text formatting shared by the desktop applications.
#pragma once
#include "FileSystem/FileSystemTypes.hpp"
#include "Utility/String.hpp"

namespace desktop {

// "512 B", "4.5 KB", "12.0 MB", "1.2 GB". Replaces the contents of `out`; false if memory ran out.
inline bool format_size(uint64_t bytes, String* out) {
  out->clear();
  if (bytes < 1024) return out->append_uint(bytes) && out->append(" B");
  const char* unit = " KB";
  uint64_t scale = 1024;
  if (bytes >= 1024ull * 1024 * 1024) { unit = " GB"; scale = 1024ull * 1024 * 1024; }
  else if (bytes >= 1024ull * 1024) { unit = " MB"; scale = 1024ull * 1024; }
  const uint64_t tenths = bytes / (scale / 10);
  return out->append_uint(tenths / 10) && out->push_back('.') && out->append_uint(tenths % 10) && out->append(unit);
}

// "2026-10-25 14:03" (local time, like the taskbar clock) for a time in seconds since 1970 UTC; empty for 0 (unknown). Replaces the contents of `out`; false if memory ran out.
inline bool format_unix_time(uint64_t seconds, String* out) {
  out->clear();
  if (seconds == 0) return true;
  const int64_t local = static_cast<int64_t>(seconds) + filesystem::local_offset_seconds();
  const filesystem::CivilTime t = filesystem::civil_from_unix(local > 0 ? static_cast<uint64_t>(local) : 0);
  auto two = [&](uint32_t v) { return (v < 10 ? out->push_back('0') : true) && out->append_uint(v); };
  return out->append_uint(static_cast<uint32_t>(t.year)) && out->push_back('-') && two(t.month) && out->push_back('-') && two(t.day) &&
         out->push_back(' ') && two(t.hour) && out->push_back(':') && two(t.minute);
}

// One whole sentence with placeholders, as the interface guidelines ask for messages (a sentence is never glued together from pieces, since
// the words come in another order in other languages): %1, %2 and %3 in `pattern` are replaced by the arguments and %% is a percent sign.
//   format_message(&text, "\"%1\" could not be opened because %2.", name, reason)
// Replaces the contents of `out`; false if memory ran out.
inline bool format_message(String* out, StringView pattern, StringView first = StringView(), StringView second = StringView(), StringView third = StringView()) {
  out->clear();
  for (uint64_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    const char next = i + 1 < pattern.size() ? pattern[i + 1] : 0;
    if (c != '%' || (next != '%' && (next < '1' || next > '3'))) {
      if (!out->push_back(c)) return false;
      continue;
    }
    ++i;
    if (!(next == '%' ? out->push_back('%') : out->append(next == '1' ? first : next == '2' ? second : third))) return false;
  }
  return true;
}

// Appends a number given in tenths: 123 -> "12.3".
inline bool append_tenths(String* out, uint64_t tenths) {
  return out->append_uint(tenths / 10) && out->push_back('.') && out->append_uint(tenths % 10);
}

}  // namespace desktop
