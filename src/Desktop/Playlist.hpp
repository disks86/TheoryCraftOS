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

// The list of files the media player goes through with its next / previous track buttons: paths, one of them the current track.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

class Playlist {
 public:
  static constexpr uint64_t kNone = ~uint64_t{0};
  explicit Playlist(allocators::IAllocator& allocator) : allocator_(allocator), items_(allocator) {}

  bool add(StringView path);
  void clear() { items_.clear(); current_ = kNone; }
  uint64_t count() const { return items_.count(); }
  StringView path(uint64_t index) const { return index < items_.count() ? items_[index].view() : StringView(); }
  // The track now playing (kNone if there is none).
  uint64_t current() const { return current_; }
  bool set_current(uint64_t index) {
    if (index >= items_.count()) return false;
    current_ = index;
    return true;
  }
  uint64_t index_of(StringView path) const;
  // The index after / before the current track, or kNone at the end / start of the list (`wrap`: the other end instead).
  uint64_t following(bool wrap) const;
  uint64_t preceding(bool wrap) const;

  // The folder part of a path ("/Desktop/Audio" for "/Desktop/Audio/a.mp3"; "/" for a file in the root), empty if the path has no folder.
  static StringView folder_of(StringView path);

 private:
  allocators::IAllocator& allocator_;
  List<String> items_;
  uint64_t current_ = kNone;
};

}  // namespace desktop
