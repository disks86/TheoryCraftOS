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

#include "Desktop/Playlist.hpp"

namespace desktop {

bool Playlist::add(StringView path) {
  String copy(allocator_);
  return copy.assign(path) && items_.push_back(static_cast<String&&>(copy));
}

uint64_t Playlist::index_of(StringView path) const {
  for (uint64_t i = 0; i < items_.count(); ++i)
    if (items_[i].view() == path) return i;
  return kNone;
}

uint64_t Playlist::following(bool wrap) const {
  if (items_.count() == 0) return kNone;
  if (current_ == kNone) return 0;
  if (current_ + 1 < items_.count()) return current_ + 1;
  return wrap ? 0 : kNone;
}

uint64_t Playlist::preceding(bool wrap) const {
  if (items_.count() == 0) return kNone;
  if (current_ == kNone) return items_.count() - 1;
  if (current_ > 0) return current_ - 1;
  return wrap ? items_.count() - 1 : kNone;
}

StringView Playlist::folder_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  if (slash == StringView::npos) return StringView();
  return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

}  // namespace desktop
