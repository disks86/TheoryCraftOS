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
#include "Utility/String.hpp"
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
  // Tells the device about the blocks freed since the last time (FileSystem/DiscardBatch), once they are free on the medium: flushes the
  // device first if anything is waiting. Cheap when nothing is. Called now and then (SystemFileSystem's service) so that discards do not
  // wait for a flush() that may never come.
  virtual Status send_discards() { return Status::Ok; }
  // fstrim: discards all the free space of the volume that holds `path` (a plain file system has one; a mount table picks the volume, or
  // takes every volume for its root). *discarded_bytes says how much went to the device. Unsupported where the device cannot discard or the
  // file system cannot list its free space; ReadOnly on a read-only volume.
  virtual Status trim(StringView path, uint64_t* discarded_bytes) {
    (void)path;
    *discarded_bytes = 0;
    return Status::Unsupported;
  }
  // Changes the read-only flag and the times of a file or folder. Unsupported by a file system that keeps none of that; ReadOnly if the volume is read-only.
  virtual Status set_metadata(StringView path, const MetadataChange& change) { (void)path; (void)change; return Status::Unsupported; }

  // Another directory entry for `existing`'s data (CreateHardLink): a write through either name is seen through the other. Unsupported by a file system with no notion of a
  // file separate from its one name, or for a directory (real file systems do not let a program link one).
  virtual Status create_hard_link(StringView existing, StringView link) { (void)existing; (void)link; return Status::Unsupported; }
  // A name that resolves to another path when opened or traversed through (CreateSymbolicLink): `target` starting with '/' is resolved from this filesystem's root, anything
  // else from `link`'s own directory (and, unlike an ordinary path, may use ".." to climb back up from there). `target_is_directory` is only a hint for a target that does
  // not exist yet.
  virtual Status create_symbolic_link(StringView link, StringView target, bool target_is_directory) {
    (void)link; (void)target; (void)target_is_directory;
    return Status::Unsupported;
  }
  // The raw target of a symbolic link, without following it. InvalidArgument if `path` exists but is not itself a symbolic link.
  virtual Status read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
    (void)path; (void)target; (void)target_is_directory;
    return Status::Unsupported;
  }
  // For a file system mounted inside another one's tree (GraftFileSystem): does `path` leave this file system through a symbolic link that points
  // outside it - an absolute target, or ".." above the root (the last component is followed only with `follow_last`)? Ok with *exit empty if it
  // does not (or this file system has no links); otherwise *exit is where the path goes on: "/abs/target/rest" from the root of the tree this file
  // system is mounted in, or "../rest" from its own root (the mount point). Any other status is what stat() would answer for the path.
  virtual Status resolve_exit(StringView path, bool follow_last, String* exit) {
    (void)path; (void)follow_last;
    exit->clear();
    return Status::Ok;
  }

 protected:
  constexpr IFileSystem() = default;
  ~IFileSystem() = default;
};

}  // namespace filesystem
