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
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/String.hpp"

namespace filesystem {

// A view of one file system with another one grafted onto it at a folder: paths at or below `at` go to the graft (with `at` taken off), everything else
// to the base. It is how a process gets a private mount (an AppImage's image at /tmp/.mount_<name> of its world) without the mount being seen by
// anyone else: whoever runs the process hands it this view instead of the system's file system.
//
// The graft folder is listed in its parent folder (after the base's own entries; a base entry of the same name is hidden). It cannot be removed or renamed,
// nor can anything be renamed between the graft and the base (Unsupported). Both file systems must outlive the view; it is as thread safe as they are.
class GraftFileSystem final : public IFileSystem {
 public:
  // `at` is an absolute, cleaned path ("/Linux/tmp/.mount_App1"), not the root.
  GraftFileSystem(allocators::IAllocator& allocator, IFileSystem& base, IFileSystem& graft) : allocator_(allocator), base_(base), graft_(graft), at_(allocator) {}
  [[nodiscard]] bool set_mount_point(StringView at);
  StringView mount_point() const { return at_.view(); }

  FileSystemInfo info() const override { return base_.info(); }
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override;
  Status open_directory(StringView path, UniquePointer<IDirectory>* directory) override;
  Status stat(StringView path, DirectoryEntry* entry) override;
  Status create_directory(StringView path) override;
  Status remove(StringView path) override;
  Status rename(StringView from, StringView to) override;
  Status flush() override { return base_.flush(); }
  Status send_discards() override { return base_.send_discards(); }
  Status trim(StringView path, uint64_t* discarded_bytes) override;
  Status set_metadata(StringView path, const MetadataChange& change) override;
  Status create_hard_link(StringView existing, StringView link) override;
  Status create_symbolic_link(StringView link, StringView target, bool target_is_directory) override;
  Status read_symbolic_link(StringView path, String* target, bool* target_is_directory) override;

 private:
  class ParentDirectory;
  enum class Where : uint8_t { Base, Graft, MountPoint };
  // Which side `path` is on; for the graft, *rest is the path inside it ("/" for the mount point itself).
  Where where(StringView path, StringView* rest) const;
  // Is `path` the folder that holds the mount point?
  bool is_parent(StringView path) const;

  allocators::IAllocator& allocator_;
  IFileSystem& base_;
  IFileSystem& graft_;
  String at_;
  StringView parent_, name_;      // (views into at_)
};

}  // namespace filesystem
