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
#include "Utility/FixedString.hpp"

namespace filesystem {

// Result of every filesystem operation (there are no exceptions). Ok means the
// operation completed; anything else leaves the filesystem as it was.
enum class Status : uint8_t {
  Ok,
  EndOfDirectory,   // IDirectory::next(): no more entries (not an error)
  NotFound,
  AlreadyExists,
  NotADirectory,
  IsADirectory,
  NotEmpty,         // removing a directory that still has entries
  Busy,             // removing or replacing something that is still open
  ReadOnly,
  NoSpace,
  InvalidPath,      // empty name, ".." component, name too long, ...
  InvalidArgument,
  IoError,          // the storage device failed
  Corrupt,          // the on-disk structures make no sense
  Unsupported,
  OutOfMemory,
  NoMedia,          // the medium is gone: a removable volume that was unplugged or ejected
  AccessDenied,     // permission refused (a network share that wants other rights)
  LoginRequired,    // a network server wants a user name and password and none was given
  LoginFailed,      // the user name or password was refused
  Network,          // the network failed: the server could not be found or reached, or the connection broke
};

const char* status_name(Status status);

enum class NodeType : uint8_t { File, Directory };

// How IFileSystem::open_file() treats the path.
enum class OpenMode : uint8_t {
  Read,            // existing file, read only
  ReadWrite,       // existing file
  CreateOrOpen,    // read/write; created empty if missing
  CreateTruncate,  // read/write; created, or emptied if it exists
};

constexpr uint64_t kMaxNameLength = 255;

// One name in a directory (also what IFileSystem::stat() reports).
struct DirectoryEntry {
  FixedString<kMaxNameLength + 1> name;  // the last component only; empty for the root
  NodeType type = NodeType::File;
  uint64_t size = 0;                     // bytes; 0 for directories
  bool read_only = false;
};

struct FileSystemInfo {
  FixedString<16> type_name;  // "ramfs", "FAT32", ...
  FixedString<32> label;      // volume label; may be empty
  uint64_t total_bytes = 0;   // 0 = unbounded (a RAM filesystem without a limit)
  uint64_t free_bytes = 0;    // 0 when unknown (read-only filesystems do not count)
  bool read_only = false;
};

}  // namespace filesystem
