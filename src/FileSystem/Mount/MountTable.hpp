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
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/SharedPointer.hpp"

namespace filesystem {

// A mounted filesystem together with whatever it stands on (a storage device, a partition window onto one). It is destroyed when the last
// user lets go of it, which is how a volume whose medium was pulled out stays valid for the handles still open on it.
class IVolume {
 public:
  virtual IFileSystem& file_system() = 0;
  // False once the medium is gone.
  virtual bool present() const { return true; }

 protected:
  constexpr IVolume() = default;
  ~IVolume() = default;
};

// The root of what the machine has mounted: a read-only folder with one subfolder per volume, each the root of that volume's filesystem
// ("/Backup/photos/a.jpg" is "/photos/a.jpg" on the volume mounted as "Backup"). Volumes come and go while the system runs (USB sticks):
//   - mount() adds one under a name made unique ("USB", "USB 2", ...);
//   - unmount() / unmount_owner() take one away. Anything already open on it keeps working as far as the data is cached by its
//     filesystem and otherwise fails with Status::NoMedia; the volume itself is freed when the last handle is closed;
//   - a volume whose present() turns false answers NoMedia before it is unmounted.
// The root and the volume folders cannot be created, removed or renamed through this filesystem. Safe from any thread.
class MountTable final : public IFileSystem {
 public:
  static constexpr uint32_t kMaxMounts = 32;
  static constexpr uint32_t kMaxName = 63;

  explicit MountTable(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator) {}
  ~MountTable();

  // `owner` is whatever the caller wants to unmount by later (a storage device's id). The name actually used goes to *assigned.
  bool mount(StringView name, SharedPointer<IVolume> volume, uint32_t owner, FixedString<kMaxName + 1>* assigned = nullptr);
  bool unmount(StringView name);
  // Unmounts everything mounted with `owner`; returns how many.
  uint32_t unmount_owner(uint32_t owner);

  uint32_t count() const;
  bool name_at(uint32_t index, FixedString<kMaxName + 1>* name) const;
  // Increases every time a volume is mounted or unmounted, for whoever shows the list and wants to know when to look again.
  uint32_t changes() const { return __atomic_load_n(&changes_, __ATOMIC_ACQUIRE); }

  FileSystemInfo info() const override;
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override;
  Status open_directory(StringView path, UniquePointer<IDirectory>* directory) override;
  Status stat(StringView path, DirectoryEntry* entry) override;
  Status create_directory(StringView path) override;
  Status remove(StringView path) override;
  Status rename(StringView from, StringView to) override;
  Status flush() override;

 private:
  struct Entry;
  class MountedFile;
  class MountedDirectory;
  class RootDirectory;

  // The volume a path starts in and the rest of the path within it ("/" when the path names the volume's folder). Null for the root.
  Status resolve(StringView path, SharedPointer<Entry>* entry, StringView* rest, bool* is_root) const;
  SharedPointer<Entry> find(StringView name) const;   // (locked)
  void changed() { __atomic_add_fetch(&changes_, 1, __ATOMIC_ACQ_REL); }

  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  List<SharedPointer<Entry>> entries_;
  uint32_t changes_ = 0;
};

}  // namespace filesystem
