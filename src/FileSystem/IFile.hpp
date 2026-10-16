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
#include "FileSystem/FileSystemTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace filesystem {

// An open file. Access is by absolute offset (no hidden position), so a handle
// is safe to share. Handles hold their filesystem: close one by destroying its
// UniquePointer, and do that before the filesystem itself goes away.
class IFile {
 public:
  virtual uint64_t size() const = 0;

  // Reads up to buffer.size() bytes at `offset`. *transferred is how many were
  // read - fewer than asked only at the end of the file (0 at or past it).
  virtual Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) = 0;
  // Writes `data` at `offset`, growing the file; a gap past the old end reads
  // as zeros. On failure *transferred says how much did get written.
  virtual Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) = 0;
  // Sets the length: shorter cuts, longer adds zeros.
  virtual Status resize(uint64_t new_size) = 0;
  // Pushes this file's changes to the medium.
  virtual Status flush() = 0;

 protected:
  constexpr IFile() = default;
  ~IFile() = default;
};

}  // namespace filesystem
