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
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// A filesystem that lives in memory from an IAllocator: nothing is persistent,
// and everything is freed with the filesystem. Useful as a scratch space and as a
// root to mount other things under. Names are case sensitive.
//
// `limit_bytes` caps the total size of all files (writes beyond it return
// NoSpace); 0 means only the allocator's memory limits it. Open files and
// directories must be destroyed before the filesystem.
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
  Status flush() override { return Status::Ok; }

 private:
  struct Node;
  class File;
  class Directory;

  Status resolve(StringView path, Node** node);
  Status resolve_parent(StringView path, Node** parent, StringView* leaf);
  Status add_child(Node& parent, StringView name, NodeType type, Node** created);
  Status set_size(Node& node, uint64_t new_size);
  static uint64_t find_child(const Node& directory, StringView name);
  static void describe(const Node& node, DirectoryEntry* entry);

  allocators::IAllocator& allocator_;
  const uint64_t limit_;
  uint64_t used_ = 0;                    // bytes in all files
  mutable kernel::RecursiveMutex mutex_;
  UniquePointer<Node> root_;
};

}  // namespace filesystem
