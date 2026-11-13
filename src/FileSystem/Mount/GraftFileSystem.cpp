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

// A folder of the graft, with the entries that are links reporting what they point to as the view resolves it (outside the graft too).
class GraftFileSystem::GraftDirectory final : public IDirectory {
 public:
  GraftDirectory(GraftFileSystem& owner, UniquePointer<IDirectory> inner, String path) : owner_(owner), inner_(util::move(inner)), path_(util::move(path)) {}
  Status next(DirectoryEntry* entry) override {
    const Status s = inner_->next(entry);
    if (s != Status::Ok || !entry->is_symlink) return s;
    String full(owner_.allocator_);
    DirectoryEntry target;
    if (!full.assign(path_.view()) || !full.push_back('/') || !full.append(entry->name.view())) return Status::OutOfMemory;
    if (owner_.stat(full.view(), &target) == Status::Ok) {
      entry->type = target.type;
      entry->size = target.size;
    } else {
      entry->type = NodeType::File;
      entry->size = 0;
    }
    return Status::Ok;
  }
  void rewind() override { inner_->rewind(); }

 private:
  GraftFileSystem& owner_;
  UniquePointer<IDirectory> inner_;
  String path_;
};

bool GraftFileSystem::set_mount_point(StringView at) {
  if (!at.starts_with('/') || at.size() < 2 || at.ends_with('/') || !at_.assign(at)) return false;
  const uint64_t slash = at_.view().rfind('/');
  parent_ = slash == 0 ? StringView("/") : at_.view().first(slash);
  name_ = at_.view().substr(slash + 1);
  return !name_.empty() && !name_.equals(".") && !name_.equals("..");
}

bool GraftFileSystem::set_link_root(StringView root) {
  uint64_t after = 0;
  if (at_.empty() || !root.starts_with('/') || !starts_with_parts(at_.view(), root, &after)) return false;
  if (!link_root_.assign(root.equals("/") ? StringView() : root)) return false;
  const StringView inside = at_.view().substr(after);
  return mount_world_.assign(inside.empty() ? StringView("/") : inside);
}

namespace {

// Removes ".", ".." (never above "/") and repeated slashes from an absolute path, without looking at links.
bool clean_path(StringView path, String* out) {
  if (!out->assign("")) return false;
  for (uint64_t pos = 0;;) {
    const StringView part = next_part(path, &pos);
    if (part.empty()) break;
    if (part.equals("..")) {
      const uint64_t slash = out->view().rfind('/');
      out->truncate(slash == StringView::npos ? 0 : slash);
      continue;
    }
    if (!out->push_back('/') || !out->append(part)) return false;
  }
  return out->empty() ? out->assign("/") : true;
}

}  // namespace

Status GraftFileSystem::route(StringView path, bool follow_last, String* storage, StringView* out) const {
  *out = path;
  if (mount_world_.empty()) return Status::Ok;
  for (uint32_t hop = 0; hop < kMaxHops; ++hop) {
    StringView rest;
    if (where(*out, &rest) != Where::Graft) return Status::Ok;
    String exit(allocator_);
    if (graft_.resolve_exit(rest, follow_last, &exit) != Status::Ok || exit.empty()) return Status::Ok;      // (the operation itself says what is wrong)
    // Where it goes on, in the process's world.
    String world(allocator_), cleaned(allocator_), next(allocator_);
    if (exit.view().starts_with('/')) {
      if (!world.assign(exit.view())) return Status::OutOfMemory;
    } else {
      if (!world.assign(mount_world_.view()) || !world.push_back('/') || !world.append(exit.view())) return Status::OutOfMemory;      // ("../x" from the mount point)
    }
    if (!clean_path(world.view(), &cleaned) || !next.assign(link_root_.view())) return Status::OutOfMemory;
    if ((!cleaned.view().equals("/") || next.empty()) && !next.append(cleaned.view())) return Status::OutOfMemory;
    *storage = util::move(next);
    *out = storage->view();
  }
  return Status::TooManyLinks;
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

#define TCOS_GRAFT_ROUTE(path, follow)                                    \
  String routed_storage_(allocator_);                                    \
  StringView routed_;                                                    \
  if (const Status r_ = route(path, follow, &routed_storage_, &routed_); r_ != Status::Ok) return r_; \
  StringView rest;

Status GraftFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  TCOS_GRAFT_ROUTE(path, true)
  switch (where(routed_, &rest)) {
    case Where::Base: return base_.open_file(routed_, mode, file);
    case Where::MountPoint: return Status::IsADirectory;
    case Where::Graft: return graft_.open_file(rest, mode, file);
  }
  return Status::NotFound;
}

Status GraftFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  TCOS_GRAFT_ROUTE(path, true)
  const Where side = where(routed_, &rest);
  if (side != Where::Base) {
    if (mount_world_.empty()) return graft_.open_directory(rest, directory);
    UniquePointer<IDirectory> inner;
    if (const Status s = graft_.open_directory(rest, &inner); s != Status::Ok) return s;
    String copy(allocator_);
    if (!copy.assign(routed_)) return Status::OutOfMemory;
    UniquePointer<GraftDirectory> listing = UniquePointer<GraftDirectory>::make(allocator_, *this, util::move(inner), util::move(copy));
    if (!listing) return Status::OutOfMemory;
    *directory = util::move(listing);
    return Status::Ok;
  }
  if (!is_parent(routed_)) return base_.open_directory(routed_, directory);
  UniquePointer<IDirectory> base;
  if (const Status s = base_.open_directory(routed_, &base); s != Status::Ok) return s;
  UniquePointer<ParentDirectory> listing = UniquePointer<ParentDirectory>::make(allocator_, *this, util::move(base));
  if (!listing) return Status::OutOfMemory;
  *directory = util::move(listing);
  return Status::Ok;
}

Status GraftFileSystem::stat(StringView path, DirectoryEntry* entry) {
  TCOS_GRAFT_ROUTE(path, true)
  Status s = Status::NotFound;
  switch (where(routed_, &rest)) {
    case Where::Base: s = base_.stat(routed_, entry); break;
    case Where::Graft: s = graft_.stat(rest, entry); break;
    case Where::MountPoint:
      s = graft_.stat("/", entry);
      if (s == Status::Ok) {
        entry->name.assign(name_);
        entry->type = NodeType::Directory;
      }
      break;
  }
  if (s == Status::Ok && routed_.data() != path.data()) {
    // It went out of the graft: the entry is the name asked for, a link if the last name was one.
    uint64_t pos = 0;
    StringView last;
    for (StringView part; !(part = next_part(path, &pos)).empty();) last = part;
    entry->name.assign(last);
    // Without following the last name, does it stay in the graft? Then that name was the link that led out.
    String plain_storage(allocator_);
    StringView plain;
    const bool stays = route(path, false, &plain_storage, &plain) == Status::Ok && plain.data() == path.data();
    if (stays) entry->is_symlink = true;
  }
  return s;
}

Status GraftFileSystem::create_directory(StringView path) {
  TCOS_GRAFT_ROUTE(path, false)
  switch (where(routed_, &rest)) {
    case Where::Base: return base_.create_directory(routed_);
    case Where::MountPoint: return Status::AlreadyExists;
    case Where::Graft: return graft_.create_directory(rest);
  }
  return Status::NotFound;
}

Status GraftFileSystem::remove(StringView path) {
  TCOS_GRAFT_ROUTE(path, false)
  switch (where(routed_, &rest)) {
    case Where::Base: return base_.remove(routed_);
    case Where::MountPoint: return Status::Busy;
    case Where::Graft: return graft_.remove(rest);
  }
  return Status::NotFound;
}

Status GraftFileSystem::rename(StringView from, StringView to) {
  String from_storage(allocator_), to_storage(allocator_);
  StringView from_path, to_path;
  if (const Status s = route(from, false, &from_storage, &from_path); s != Status::Ok) return s;
  if (const Status s = route(to, false, &to_storage, &to_path); s != Status::Ok) return s;
  StringView from_rest, to_rest;
  const Where a = where(from_path, &from_rest), b = where(to_path, &to_rest);
  if (a == Where::Base && b == Where::Base) return base_.rename(from_path, to_path);
  if (a == Where::Graft && b == Where::Graft) return graft_.rename(from_rest, to_rest);
  return a == Where::MountPoint || b == Where::MountPoint ? Status::Busy : Status::Unsupported;
}

Status GraftFileSystem::trim(StringView path, uint64_t* discarded_bytes) {
  TCOS_GRAFT_ROUTE(path, true)
  if (where(routed_, &rest) == Where::Base) return base_.trim(routed_, discarded_bytes);
  return graft_.trim(rest, discarded_bytes);
}

Status GraftFileSystem::set_metadata(StringView path, const MetadataChange& change) {
  TCOS_GRAFT_ROUTE(path, true)
  if (where(routed_, &rest) == Where::Base) return base_.set_metadata(routed_, change);
  return graft_.set_metadata(rest, change);
}

Status GraftFileSystem::create_special(StringView path, SpecialNode kind) {
  TCOS_GRAFT_ROUTE(path, false)
  switch (where(routed_, &rest)) {
    case Where::Base: return base_.create_special(routed_, kind);
    case Where::MountPoint: return Status::AlreadyExists;
    case Where::Graft: return graft_.create_special(rest, kind);
  }
  return Status::NotFound;
}

Status GraftFileSystem::create_hard_link(StringView existing, StringView link) {
  String a_storage(allocator_), b_storage(allocator_);
  StringView a_path, b_path;
  if (const Status s = route(existing, true, &a_storage, &a_path); s != Status::Ok) return s;
  if (const Status s = route(link, false, &b_storage, &b_path); s != Status::Ok) return s;
  StringView a_rest, b_rest;
  const Where a = where(a_path, &a_rest), b = where(b_path, &b_rest);
  if (a == Where::Base && b == Where::Base) return base_.create_hard_link(a_path, b_path);
  if (a == Where::Graft && b == Where::Graft) return graft_.create_hard_link(a_rest, b_rest);
  return Status::Unsupported;
}

Status GraftFileSystem::create_symbolic_link(StringView link, StringView target, bool target_is_directory) {
  TCOS_GRAFT_ROUTE(link, false)
  switch (where(routed_, &rest)) {
    case Where::Base: return base_.create_symbolic_link(routed_, target, target_is_directory);
    case Where::MountPoint: return Status::AlreadyExists;
    case Where::Graft: return graft_.create_symbolic_link(rest, target, target_is_directory);
  }
  return Status::NotFound;
}

Status GraftFileSystem::read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
  TCOS_GRAFT_ROUTE(path, false)
  Status s = Status::NotFound;
  switch (where(routed_, &rest)) {
    case Where::Base: return base_.read_symbolic_link(routed_, target, target_is_directory);
    case Where::MountPoint: return Status::InvalidArgument;
    case Where::Graft: s = graft_.read_symbolic_link(rest, target, target_is_directory); break;
  }
  if (s == Status::Ok && !mount_world_.empty()) {      // whether it points at a folder, as the view resolves it
    DirectoryEntry pointed;
    *target_is_directory = stat(path, &pointed) == Status::Ok && pointed.type == NodeType::Directory;
  }
  return s;
}

#undef TCOS_GRAFT_ROUTE

}  // namespace filesystem
