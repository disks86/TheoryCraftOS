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

// What a Linux program's drag carries, as the desktop's items: one per line, files as system paths. Shared by the Wayland compositor and the X server.
#pragma once
#include <stdint.h>
#include "FileSystem/SystemLayout.hpp"
#include "Utility/FileUri.hpp"
#include "Utility/String.hpp"

namespace desktop {

// `data` is a URI list (`uri`: text/uri-list, RFC 2483: file URIs only, percent-decoded, made system paths with layout::system_path_of_linux;
// comments, other schemes and names with a line break left out) or text (one item: lines joined with spaces, a NUL ends it). False when
// nothing is left (or on out-of-memory).
inline bool drop_items_of(ConstByteView data, bool uri, String* out) {
  out->clear();
  const StringView all(reinterpret_cast<const char*>(data.data()), data.size());      // (bytes of text: a view of them as characters)
  if (uri) {
    for (uint64_t at = 0; at < all.size();) {
      uint64_t end = at;
      while (end < all.size() && all[end] != '\n') ++end;
      StringView line = all.substr(at, end - at);
      at = end + 1;
      if (!line.empty() && line[line.size() - 1] == '\r') line = line.first(line.size() - 1);
      if (line.empty() || line[0] == '#') continue;
      String path(out->allocator()), system(out->allocator());
      if (!util::path_of_file_uri(line, &path) || !filesystem::layout::system_path_of_linux(path.view(), &system)) continue;
      bool clean = true;
      for (char ch : system.view()) if (ch == '\n') clean = false;
      if (!clean) continue;
      if ((!out->empty() && !out->push_back('\n')) || !out->append(system.view())) return false;
    }
  } else {
    for (char ch : all) {
      if (ch == 0) break;
      if (!out->push_back(ch == '\n' ? ' ' : ch)) break;
    }
  }
  return !out->empty();
}

}  // namespace desktop
