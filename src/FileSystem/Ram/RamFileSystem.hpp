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
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// A filesystem that lives in memory from an IAllocator: nothing is persistent,
// and everything is freed with the filesystem. Useful as a scratch space and as a
// root to mount other things under. Names are case sensitive.
//
// `limit_bytes` caps the total size of all files (writes beyond it return
// NoSpace); 0 means only the allocator's memory limits it. Open files and
// directories must be destroyed before the filesystem.
//
// A hard link (create_hard_link) is another directory entry whose file Node shares the same Content as the original: the data, size, read-only flag and times are the
// same object, seen through either name. A symbolic link (create_symbolic_link) is its own kind of Node holding only a target path string; resolve() follows it (a
// relative target against the link's own directory, an absolute one from the root) while walking a path, up to a hop limit.
class RamFileSystem final : public IFileSystem {
 public:
  explicit RamFileSystem(allocators::IAllocator& allocator, uint64_t limit_bytes = 0);
  ~RamFileSystem();
  RamFileSystem(const RamFileSystem&) = delete;
  RamFileSystem& operator=(const RamFileSystem&) = delete;

  FileSystemInfo info() const override;
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override;
  Status open_directory(StringView path, UniquePointer<IDirectory>* directory) override;
  Status stat(StringView path, DirectoryEntry* entry) override;
  Status create_directory(StringView path) override;
  Status remove(StringView path) override;
  Status rename(StringView from, StringView to) override;
  Status set_metadata(StringView path, const MetadataChange& change) override;
  Status create_hard_link(StringView existing, StringView link) override;
  Status create_symbolic_link(StringView link, StringView target, bool target_is_directory) override;
  Status read_symbolic_link(StringView path, String* target, bool* target_is_directory) override;
  Status flush() override { return Status::Ok; }

 private:
  struct Content;
  struct Node;
  class File;
  class Directory;

  // Resolves `path` from this filesystem's root, following symbolic links at every component including the last (so the Node returned, if any, is never itself a
  // Symlink). `hops` counts links followed so far across the whole call (TooManyLinks past a limit); callers from outside start it at 0.
  Status resolve(StringView path, Node** node, uint32_t* hops);
  Status resolve(StringView path, Node** node) { uint32_t hops = 0; return resolve(path, node, &hops); }
  // The same, but `path` is taken relative to `start` rather than to the root (a relative symbolic link's target is resolved this way, from the link's own directory).
  // `allow_dotdot` lets a ".." component climb back up from `start` instead of failing with InvalidPath, as a symbolic link's target may.
  Status resolve_from(Node* start, StringView path, Node** node, uint32_t* hops, bool allow_dotdot = false);
  // The directory a path's last component would be made in, and that component's name; does not look at (or follow) a node of that name.
  Status resolve_parent(StringView path, Node** parent, StringView* leaf);
  // The target of a symlink node, resolved against `from` (the link's own directory) if relative, from the root if it starts with '/'.
  Status follow_symlink(const Node& link, Node* from, Node** node, uint32_t* hops);
  Status add_child(Node& parent, StringView name, NodeType type, Node** created);
  Status set_size(Content& content, uint64_t new_size);
  static uint64_t find_child(const Node& directory, StringView name);
  static void describe(const Node& node, DirectoryEntry* entry);

  allocators::IAllocator& allocator_;
  const uint64_t limit_;
  uint64_t used_ = 0;                    // bytes in all files
  mutable kernel::RecursiveMutex mutex_;
  UniquePointer<Node> root_;
};

}  // namespace filesystem
