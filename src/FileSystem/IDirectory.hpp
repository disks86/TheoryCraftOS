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

namespace filesystem {

// An open directory, read one entry at a time. "." and ".." are never listed.
// Changing the directory while iterating may skip or repeat entries. Destroy the
// handle before its filesystem.
class IDirectory {
 public:
  // Ok with *entry filled in, EndOfDirectory when there are no more, or an error.
  virtual Status next(DirectoryEntry* entry) = 0;
  // Starts over from the first entry.
  virtual void rewind() = 0;

 protected:
  constexpr IDirectory() = default;
  ~IDirectory() = default;
};

}  // namespace filesystem
