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

#include "FileSystem/Mount/MountTable.hpp"
#include "FileSystem/Path.hpp"

namespace filesystem {

struct MountTable::Entry {
  FixedString<kMaxName + 1> name;
  uint32_t owner = 0;
  SharedPointer<IVolume> volume;
  bool gone = false;   // unmounted (set once, read from any thread)

  bool usable() const { return !__atomic_load_n(&gone, __ATOMIC_ACQUIRE) && volume->present(); }
};

// An open file on a volume: keeps the mount entry (and so the volume) alive, and answers NoMedia once the volume is gone.
class MountTable::MountedFile final : public IFile {
 public:
  MountedFile(SharedPointer<Entry> entry, UniquePointer<IFile> inner) : entry_(util::move(entry)), inner_(util::move(inner)) {}
  uint64_t size() const override { return inner_->size(); }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    if (!entry_->usable()) { *transferred = 0; return Status::NoMedia; }
    return inner_->read(offset, buffer, transferred);
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    if (!entry_->usable()) { *transferred = 0; return Status::NoMedia; }
    return inner_->write(offset, data, transferred);
  }
  Status resize(uint64_t new_size) override { return entry_->usable() ? inner_->resize(new_size) : Status::NoMedia; }
  Status flush() override { return entry_->usable() ? inner_->flush() : Status::NoMedia; }

 private:
  SharedPointer<Entry> entry_;   // (declared first: destroyed last, after the handle that uses the volume)
  UniquePointer<IFile> inner_;
};

class MountTable::MountedDirectory final : public IDirectory {
 public:
  MountedDirectory(SharedPointer<Entry> entry, UniquePointer<IDirectory> inner) : entry_(util::move(entry)), inner_(util::move(inner)) {}
  Status next(DirectoryEntry* entry) override { return entry_->usable() ? inner_->next(entry) : Status::NoMedia; }
  void rewind() override { inner_->rewind(); }

 private:
  SharedPointer<Entry> entry_;
  UniquePointer<IDirectory> inner_;
};

// The list of volumes, as it was when the folder was opened.
class MountTable::RootDirectory final : public IDirectory {
 public:
  Status next(DirectoryEntry* entry) override {
    if (index_ >= count_) return Status::EndOfDirectory;
    *entry = DirectoryEntry();
    entry->name.assign(names_[index_++].view());
    entry->type = NodeType::Directory;
    return Status::Ok;
  }
  void rewind() override { index_ = 0; }
  bool add(StringView name) { return count_ < kMaxMounts && names_[count_++].assign(name); }

 private:
  FixedString<kMaxName + 1> names_[kMaxMounts];
  uint32_t count_ = 0, index_ = 0;
};

MountTable::~MountTable() {
  kernel::ScopedMutex guard(mutex_);
  for (SharedPointer<Entry>& e : entries_.view()) __atomic_store_n(&e->gone, true, __ATOMIC_RELEASE);
  entries_.clear();
}

SharedPointer<MountTable::Entry> MountTable::find(StringView name) const {
  for (const SharedPointer<Entry>& e : entries_.view())
    if (e->name.view().equals_ignore_case(name)) return e;
  return SharedPointer<Entry>();
}

bool MountTable::mount(StringView name, SharedPointer<IVolume> volume, uint32_t owner, FixedString<kMaxName + 1>* assigned) {
  if (!volume) return false;
  // A name that is one path component: no slashes or control characters, no blanks around it, room left for a number.
  FixedString<kMaxName + 1> base;
  for (char c : name) {
    if (base.size() >= kMaxName - 8) break;
    base.push_back((c == '/' || c == '\\' || static_cast<unsigned char>(c) < 32 || c == 127) ? '_' : c);
  }
  const StringView trimmed = base.view().trimmed();
  FixedString<kMaxName + 1> clean;
  clean.assign(trimmed.empty() || trimmed.equals(".") || trimmed.equals("..") ? StringView("Volume") : trimmed);

  SharedPointer<Entry> entry = SharedPointer<Entry>::make(allocator_);
  if (!entry) return false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (entries_.count() >= kMaxMounts) return false;
    FixedString<kMaxName + 1> unique = clean;
    for (uint32_t n = 2; find(unique.view()); ++n) {
      unique = clean;
      unique.push_back(' ');
      unique.append_uint(n);
    }
    entry->name = unique;
    entry->owner = owner;
    entry->volume = util::move(volume);
    if (!entries_.push_back(entry)) return false;
    if (assigned) *assigned = unique;
  }
  changed();
  return true;
}

bool MountTable::unmount(StringView name) {
  SharedPointer<Entry> removed;
  {
    kernel::ScopedMutex guard(mutex_);
    for (uint64_t i = 0; i < entries_.count(); ++i)
      if (entries_[i]->name.view().equals_ignore_case(name)) {
        removed = entries_[i];
        entries_.remove_at(i);
        break;
      }
  }
  if (!removed) return false;
  __atomic_store_n(&removed->gone, true, __ATOMIC_RELEASE);
  changed();
  return true;
}

uint32_t MountTable::unmount_owner(uint32_t owner) {
  uint32_t n = 0;
  for (;;) {
    SharedPointer<Entry> removed;
    {
      kernel::ScopedMutex guard(mutex_);
      for (uint64_t i = 0; i < entries_.count(); ++i)
        if (entries_[i]->owner == owner) {
          removed = entries_[i];
          entries_.remove_at(i);
          break;
        }
    }
    if (!removed) return n;
    __atomic_store_n(&removed->gone, true, __ATOMIC_RELEASE);
    changed();
    ++n;
  }
}

uint32_t MountTable::count() const {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(entries_.count());
}

bool MountTable::name_at(uint32_t index, FixedString<kMaxName + 1>* name) const {
  kernel::ScopedMutex guard(mutex_);
  if (index >= entries_.count()) return false;
  *name = entries_[index]->name;
  return true;
}

Status MountTable::resolve(StringView path, SharedPointer<Entry>* entry, StringView* rest, bool* is_root) const {
  StringView remaining = path, component;
  bool found = false;
  while (next_component(&remaining, &component)) {
    if (is_dot(component)) continue;
    found = true;
    break;
  }
  *is_root = !found;
  if (!found) return Status::Ok;
  if (component.equals("..")) return Status::InvalidPath;
  {
    kernel::ScopedMutex guard(mutex_);
    *entry = find(component);
  }
  if (!*entry) return Status::NotFound;
  *rest = remaining.empty() ? StringView("/") : remaining;
  return Status::Ok;
}

FileSystemInfo MountTable::info() const {
  FileSystemInfo info;
  info.type_name.assign("mounts");
  info.read_only = true;
  return info;
}

Status MountTable::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  SharedPointer<Entry> entry;
  StringView rest;
  bool root;
  const Status resolved = resolve(path, &entry, &rest, &root);
  if (resolved != Status::Ok) return resolved;
  if (root) return Status::IsADirectory;
  if (!entry->usable()) return Status::NoMedia;
  UniquePointer<IFile> inner;
  const Status status = entry->volume->file_system().open_file(rest, mode, &inner);
  if (status != Status::Ok) return status;
  UniquePointer<IFile> wrapped = UniquePointer<IFile>::make_as<MountedFile>(allocator_, util::move(entry), util::move(inner));
  if (!wrapped) return Status::OutOfMemory;
  *file = util::move(wrapped);
  return Status::Ok;
}

Status MountTable::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  SharedPointer<Entry> entry;
  StringView rest;
  bool root;
  const Status resolved = resolve(path, &entry, &rest, &root);
  if (resolved != Status::Ok) return resolved;
  if (root) {
    UniquePointer<RootDirectory> listing = UniquePointer<RootDirectory>::make(allocator_);
    if (!listing) return Status::OutOfMemory;
    {
      kernel::ScopedMutex guard(mutex_);
      for (const SharedPointer<Entry>& e : entries_.view()) listing->add(e->name.view());
    }
    *directory = UniquePointer<IDirectory>(util::move(listing));
    return Status::Ok;
  }
  if (!entry->usable()) return Status::NoMedia;
  UniquePointer<IDirectory> inner;
  const Status status = entry->volume->file_system().open_directory(rest, &inner);
  if (status != Status::Ok) return status;
  UniquePointer<IDirectory> wrapped = UniquePointer<IDirectory>::make_as<MountedDirectory>(allocator_, util::move(entry), util::move(inner));
  if (!wrapped) return Status::OutOfMemory;
  *directory = util::move(wrapped);
  return Status::Ok;
}

Status MountTable::stat(StringView path, DirectoryEntry* out) {
  SharedPointer<Entry> entry;
  StringView rest;
  bool root;
  const Status resolved = resolve(path, &entry, &rest, &root);
  if (resolved != Status::Ok) return resolved;
  if (root) {
    *out = DirectoryEntry();
    out->type = NodeType::Directory;
    return Status::Ok;
  }
  if (!entry->usable()) return Status::NoMedia;
  const Status status = entry->volume->file_system().stat(rest, out);
  if (status == Status::Ok && rest.equals("/")) out->name.assign(entry->name.view());   // the volume's folder is called by the name it is mounted under
  return status;
}

Status MountTable::create_directory(StringView path) {
  SharedPointer<Entry> entry;
  StringView rest;
  bool root;
  const Status resolved = resolve(path, &entry, &rest, &root);
  if (resolved != Status::Ok) return resolved == Status::NotFound ? Status::Unsupported : resolved;   // (no new volumes by creating folders)
  if (root) return Status::AlreadyExists;
  if (!entry->usable()) return Status::NoMedia;
  if (rest.equals("/")) return Status::AlreadyExists;
  return entry->volume->file_system().create_directory(rest);
}

Status MountTable::remove(StringView path) {
  SharedPointer<Entry> entry;
  StringView rest;
  bool root;
  const Status resolved = resolve(path, &entry, &rest, &root);
  if (resolved != Status::Ok) return resolved;
  if (root || rest.equals("/")) return Status::Unsupported;   // volumes are not removed, they are ejected
  if (!entry->usable()) return Status::NoMedia;
  return entry->volume->file_system().remove(rest);
}

Status MountTable::rename(StringView from, StringView to) {
  SharedPointer<Entry> a, b;
  StringView rest_a, rest_b;
  bool root_a, root_b;
  Status status = resolve(from, &a, &rest_a, &root_a);
  if (status != Status::Ok) return status;
  status = resolve(to, &b, &rest_b, &root_b);
  if (status != Status::Ok && status != Status::NotFound) return status;
  if (root_a || root_b || !b || rest_a.equals("/") || rest_b.equals("/")) return Status::Unsupported;
  if (a.get() != b.get()) return Status::Unsupported;   // between volumes is a copy, not a rename
  if (!a->usable()) return Status::NoMedia;
  return a->volume->file_system().rename(rest_a, rest_b);
}

Status MountTable::flush() {
  List<SharedPointer<Entry>> snapshot(allocator_);
  {
    kernel::ScopedMutex guard(mutex_);
    for (const SharedPointer<Entry>& e : entries_.view()) snapshot.push_back(e);
  }
  Status result = Status::Ok;
  for (const SharedPointer<Entry>& e : snapshot.view()) {
    if (!e->usable()) continue;
    const Status status = e->volume->file_system().flush();
    if (status != Status::Ok && result == Status::Ok) result = status;
  }
  return result;
}

}  // namespace filesystem
