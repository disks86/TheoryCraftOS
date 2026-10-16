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
#include "FileSystem/FileSystemTypes.hpp"
#include "Utility/StringView.hpp"

namespace filesystem {

// Path handling shared by the filesystems (see IFileSystem for the rules).

// Takes the next component off the front of `*rest`. False at the end. A "."
// component is returned like any other: callers skip it.
inline bool next_component(StringView* rest, StringView* component) {
  uint64_t skip = 0;
  while (skip < rest->size() && (*rest)[skip] == '/') ++skip;
  const StringView tail = rest->without_prefix(skip);
  if (tail.empty()) {
    *rest = StringView();
    return false;
  }
  uint64_t length = tail.find('/');
  if (length == StringView::npos) length = tail.size();
  *component = tail.first(length);
  *rest = tail.without_prefix(length);
  return true;
}

inline bool is_dot(StringView component) { return component.size() == 1 && component[0] == '.'; }

// A name a directory entry may have: not empty, not "." or "..", at most 255 bytes, no NUL.
inline bool valid_name(StringView name) {
  if (name.empty() || name.size() > kMaxNameLength) return false;
  if (is_dot(name) || name.equals("..")) return false;
  return !name.contains('\0');
}

// Splits `path` into the directory part and the last real component. False for a
// path with no components (the root). *parent is a prefix of `path`, which still
// has to be walked (it can hold "." or "..").
inline bool split_parent(StringView path, StringView* parent, StringView* leaf) {
  StringView rest = path, component;
  bool found = false;
  uint64_t leaf_at = 0;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    found = true;
    *leaf = component;
    leaf_at = static_cast<uint64_t>(component.data() - path.data());
  }
  if (!found) return false;
  *parent = path.first(leaf_at);
  return true;
}

}  // namespace filesystem
