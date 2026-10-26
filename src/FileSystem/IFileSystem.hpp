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
#include "FileSystem/IDirectory.hpp"
#include "FileSystem/IFile.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// A mounted filesystem: a tree of directories and files addressed by path.
// Implementations live in subfolders by kind (Ram/, Fat/). Every call is safe
// from any thread; calls complete before they return.
//
// Paths are absolute from this filesystem's root: components separated by '/',
// repeated and trailing separators ignored, "." skipped, ".." not supported
// (InvalidPath). Names are at most 255 bytes of UTF-8. Whether names differ by
// case is up to the filesystem (RAM: yes, FAT: no).
//
//   UniquePointer<IFile> file;
//   if (fs.open_file("/etc/motd", OpenMode::Read, &file) == Status::Ok) {
//     uint8_t bytes[64]; uint64_t got;
//     file->read(0, ByteView(bytes), &got);
//   }
//
// A filesystem that cannot do something (a read-only one asked to write) returns
// ReadOnly or Unsupported. Open files and directories keep a filesystem busy:
// destroy them before it.
class IFileSystem {
 public:
  virtual FileSystemInfo info() const = 0;

  virtual Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) = 0;
  virtual Status open_directory(StringView path, UniquePointer<IDirectory>* directory) = 0;
  virtual Status stat(StringView path, DirectoryEntry* entry) = 0;

  // The parent must exist. AlreadyExists if the name is taken.
  virtual Status create_directory(StringView path) = 0;
  // Removes a file or an empty directory. Busy while it is open.
  virtual Status remove(StringView path) = 0;
  // Moves or renames a file or directory (to another directory too). `to` must
  // not exist (AlreadyExists), nor be inside `from`.
  virtual Status rename(StringView from, StringView to) = 0;

  // Pushes everything to the medium.
  virtual Status flush() = 0;
  // Changes the read-only flag and the times of a file or folder. Unsupported by a file system that keeps none of that; ReadOnly if the volume is read-only.
  virtual Status set_metadata(StringView path, const MetadataChange& change) { (void)path; (void)change; return Status::Unsupported; }

 protected:
  constexpr IFileSystem() = default;
  ~IFileSystem() = default;
};

}  // namespace filesystem
