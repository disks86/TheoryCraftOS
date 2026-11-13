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

#include "FileSystem/Mount/GraftFileSystem.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

namespace {

// The next component of `path` at or after *pos ("" at the end), skipping "." and repeated slashes.
StringView next_part(StringView path, uint64_t* pos) {
  for (;;) {
    while (*pos < path.size() && path[*pos] == '/') ++*pos;
    const uint64_t start = *pos;
    while (*pos < path.size() && path[*pos] != '/') ++*pos;
    const StringView part = path.substr(start, *pos - start);
    if (!part.equals(".")) return part;
  }
}

// Does `path` start with the components of `prefix`? *after is where in `path` the rest begins.
bool starts_with_parts(StringView path, StringView prefix, uint64_t* after) {
  uint64_t p = 0, q = 0;
  for (;;) {
    const StringView want = next_part(prefix, &q);
    if (want.empty()) {
      *after = p;
      return true;
    }
    if (!next_part(path, &p).equals(want)) return false;
  }
}

}  // namespace

class GraftFileSystem::ParentDirectory final : public IDirectory {
 public:
  ParentDirectory(GraftFileSystem& owner, UniquePointer<IDirectory> base) : owner_(owner), base_(util::move(base)) {}
  Status next(DirectoryEntry* entry) override {
    while (!base_done_) {
      const Status s = base_->next(entry);
      if (s == Status::EndOfDirectory) { base_done_ = true; break; }
      if (s != Status::Ok) return s;
      if (!entry->name.view().equals(owner_.name_)) return Status::Ok;
    }
    if (graft_listed_) return Status::EndOfDirectory;
    graft_listed_ = true;
    const Status s = owner_.graft_.stat("/", entry);
    if (s != Status::Ok) return s;
    entry->name.assign(owner_.name_);
    entry->type = NodeType::Directory;
    return Status::Ok;
  }
  void rewind() override {
    base_->rewind();
    base_done_ = graft_listed_ = false;
  }

 private:
  GraftFileSystem& owner_;
  UniquePointer<IDirectory> base_;
  bool base_done_ = false, graft_listed_ = false;
};

bool GraftFileSystem::set_mount_point(StringView at) {
  if (!at.starts_with('/') || at.size() < 2 || at.ends_with('/') || !at_.assign(at)) return false;
  const uint64_t slash = at_.view().rfind('/');
  parent_ = slash == 0 ? StringView("/") : at_.view().first(slash);
  name_ = at_.view().substr(slash + 1);
  return !name_.empty() && !name_.equals(".") && !name_.equals("..");
}

GraftFileSystem::Where GraftFileSystem::where(StringView path, StringView* rest) const {
  uint64_t after = 0;
  if (at_.empty() || !starts_with_parts(path, at_.view(), &after)) return Where::Base;
  uint64_t probe = after;
  if (next_part(path, &probe).empty()) {
    *rest = "/";
    return Where::MountPoint;
  }
  *rest = path.substr(after);
  return Where::Graft;
}

bool GraftFileSystem::is_parent(StringView path) const {
  uint64_t after = 0;
  if (!starts_with_parts(path, parent_, &after)) return false;
  return next_part(path, &after).empty();
}

Status GraftFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  StringView rest;
  switch (where(path, &rest)) {
    case Where::Base: return base_.open_file(path, mode, file);
    case Where::MountPoint: return Status::IsADirectory;
    case Where::Graft: return graft_.open_file(rest, mode, file);
  }
  return Status::NotFound;
}

Status GraftFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  StringView rest;
  const Where side = where(path, &rest);
  if (side != Where::Base) return graft_.open_directory(rest, directory);
  if (!is_parent(path)) return base_.open_directory(path, directory);
  UniquePointer<IDirectory> base;
  if (const Status s = base_.open_directory(path, &base); s != Status::Ok) return s;
  UniquePointer<ParentDirectory> listing = UniquePointer<ParentDirectory>::make(allocator_, *this, util::move(base));
  if (!listing) return Status::OutOfMemory;
  *directory = util::move(listing);
  return Status::Ok;
}

Status GraftFileSystem::stat(StringView path, DirectoryEntry* entry) {
  StringView rest;
  switch (where(path, &rest)) {
    case Where::Base: return base_.stat(path, entry);
    case Where::Graft: return graft_.stat(rest, entry);
    case Where::MountPoint: {
      const Status s = graft_.stat("/", entry);
      if (s == Status::Ok) {
        entry->name.assign(name_);
        entry->type = NodeType::Directory;
      }
      return s;
    }
  }
  return Status::NotFound;
}

Status GraftFileSystem::create_directory(StringView path) {
  StringView rest;
  switch (where(path, &rest)) {
    case Where::Base: return base_.create_directory(path);
    case Where::MountPoint: return Status::AlreadyExists;
    case Where::Graft: return graft_.create_directory(rest);
  }
  return Status::NotFound;
}

Status GraftFileSystem::remove(StringView path) {
  StringView rest;
  switch (where(path, &rest)) {
    case Where::Base: return base_.remove(path);
    case Where::MountPoint: return Status::Busy;
    case Where::Graft: return graft_.remove(rest);
  }
  return Status::NotFound;
}

Status GraftFileSystem::rename(StringView from, StringView to) {
  StringView from_rest, to_rest;
  const Where a = where(from, &from_rest), b = where(to, &to_rest);
  if (a == Where::Base && b == Where::Base) return base_.rename(from, to);
  if (a == Where::Graft && b == Where::Graft) return graft_.rename(from_rest, to_rest);
  return a == Where::MountPoint || b == Where::MountPoint ? Status::Busy : Status::Unsupported;
}

Status GraftFileSystem::trim(StringView path, uint64_t* discarded_bytes) {
  StringView rest;
  if (where(path, &rest) == Where::Base) return base_.trim(path, discarded_bytes);
  return graft_.trim(rest, discarded_bytes);
}

Status GraftFileSystem::set_metadata(StringView path, const MetadataChange& change) {
  StringView rest;
  if (where(path, &rest) == Where::Base) return base_.set_metadata(path, change);
  return graft_.set_metadata(rest, change);
}

Status GraftFileSystem::create_hard_link(StringView existing, StringView link) {
  StringView a_rest, b_rest;
  const Where a = where(existing, &a_rest), b = where(link, &b_rest);
  if (a == Where::Base && b == Where::Base) return base_.create_hard_link(existing, link);
  if (a == Where::Graft && b == Where::Graft) return graft_.create_hard_link(a_rest, b_rest);
  return Status::Unsupported;
}

Status GraftFileSystem::create_symbolic_link(StringView link, StringView target, bool target_is_directory) {
  StringView rest;
  switch (where(link, &rest)) {
    case Where::Base: return base_.create_symbolic_link(link, target, target_is_directory);
    case Where::MountPoint: return Status::AlreadyExists;
    case Where::Graft: return graft_.create_symbolic_link(rest, target, target_is_directory);
  }
  return Status::NotFound;
}

Status GraftFileSystem::read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
  StringView rest;
  switch (where(path, &rest)) {
    case Where::Base: return base_.read_symbolic_link(path, target, target_is_directory);
    case Where::MountPoint: return Status::InvalidArgument;
    case Where::Graft: return graft_.read_symbolic_link(rest, target, target_is_directory);
  }
  return Status::NotFound;
}

}  // namespace filesystem
