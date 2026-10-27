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
#include "Process/ProcessFiles.hpp"
#include "Process/Objects.hpp"

namespace process {

using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::OpenMode;
using filesystem::Status;

namespace {

constexpr uint64_t kMaxName = filesystem::kMaxNameLength;

bool windows_forbidden(char c) { return static_cast<uint8_t>(c) < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*'; }

bool equals_ignore_case(StringView text, const char* literal) {
  uint64_t i = 0;
  for (; literal[i]; ++i) {
    if (i >= text.size()) return false;
    char c = text[i], l = literal[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (l >= 'A' && l <= 'Z') l = static_cast<char>(l + 32);
    if (c != l) return false;
  }
  return i == text.size();
}

bool starts_with_ignore_case(StringView text, const char* literal, uint64_t* length) {
  uint64_t n = 0;
  while (literal[n]) ++n;
  if (text.size() < n || !equals_ignore_case(text.first(n), literal)) return false;
  *length = n;
  return true;
}

// The cleaned path as a list of components, kept in `out` as "/a/b".
class Cleaner {
 public:
  explicit Cleaner(String* out) : out_(out) { out_->clear(); }
  bool failed() const { return failed_; }
  PathResult result() const { return result_; }
  // Adds the components of a path in which `separators` split names.
  void add(StringView path, bool windows) {
    uint64_t i = 0;
    while (i <= path.size() && !failed_) {
      uint64_t end = i;
      while (end < path.size() && !(path[end] == '/' || (windows && path[end] == '\\'))) ++end;
      component(path.substr(i, end - i), windows);
      i = end + 1;
    }
  }
  void reset_to_root() { out_->clear(); }
  void finish() {
    if (!failed_ && out_->empty()) out_->assign("/");
  }

 private:
  void fail(PathResult r) { failed_ = true; result_ = r; }
  void component(StringView name, bool windows) {
    if (name.empty() || (name.size() == 1 && name[0] == '.')) return;
    if (name.size() == 2 && name[0] == '.' && name[1] == '.') {
      const uint64_t cut = out_->view().rfind('/');
      if (cut != StringView::npos) out_->erase(cut, out_->size() - cut);      // never above the root: with nothing left there is nothing to remove
      return;
    }
    if (windows) {
      // Windows drops trailing dots and spaces from a name, and refuses characters it reserves.
      while (!name.empty() && (name[name.size() - 1] == '.' || name[name.size() - 1] == ' ')) name = name.first(name.size() - 1);
      if (name.empty()) return;
      for (char c : name) if (windows_forbidden(c)) { fail(PathResult::Invalid); return; }
    } else {
      for (char c : name) if (c == 0) { fail(PathResult::Invalid); return; }
    }
    if (name.size() > kMaxName) { fail(PathResult::TooLong); return; }
    if (out_->size() + 1 + name.size() > kMaxPath) { fail(PathResult::TooLong); return; }
    if (!out_->push_back('/') || !out_->append(name)) fail(PathResult::TooLong);
  }
  String* out_;
  bool failed_ = false;
  PathResult result_ = PathResult::Ok;
};

}  // namespace

PathResult resolve_path(PathStyle style, StringView cwd, StringView path, String* out) {
  if (path.empty()) return PathResult::Empty;
  if (path.size() > kMaxPath) return PathResult::TooLong;
  const bool windows = style == PathStyle::Windows;
  Cleaner cleaner(out);
  bool absolute = false;
  if (windows) {
    uint64_t skip = 0;
    // NT and Win32 "device" prefixes, then the drive.
    bool prefixed = starts_with_ignore_case(path, "\\??\\", &skip) || starts_with_ignore_case(path, "\\\\?\\", &skip) || starts_with_ignore_case(path, "\\\\.\\", &skip);
    if (prefixed) path = path.without_prefix(skip);
    const bool drive = path.size() >= 2 && path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
    if (drive) {
      if (path[0] != 'C' && path[0] != 'c') return PathResult::Invalid;      // there is only drive C
      path = path.without_prefix(2);
      absolute = !path.empty() && (path[0] == '\\' || path[0] == '/');
      if (path.empty()) return out->assign(cwd) ? PathResult::Ok : PathResult::TooLong;      // "C:" alone is the current directory
    } else if (prefixed) {
      return PathResult::Invalid;      // "\\??\\" must be followed by a drive
    } else {
      absolute = path[0] == '\\' || path[0] == '/';
    }
  } else {
    absolute = path[0] == '/';
  }
  if (!absolute) cleaner.add(cwd, windows);
  cleaner.add(path, windows);
  if (cleaner.failed()) return cleaner.result();
  cleaner.finish();
  return PathResult::Ok;
}

ProcessFiles::ProcessFiles(PathStyle style) : style_(style) {
  slots_[0].kind = DescriptorKind::ConsoleInput;
  slots_[1].kind = DescriptorKind::ConsoleOutput;
  slots_[2].kind = DescriptorKind::ConsoleError;
}

void ProcessFiles::attach(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView root_prefix) {
  allocator_ = &allocator;
  fs_ = &fs;
  root_prefix_ = root_prefix;
}

bool ProcessFiles::host_path(StringView virtual_path, String* out) const {
  if (!fs_ || !allocator_) return false;
  out->clear();
  if (!out->assign(root_prefix_)) return false;
  return virtual_path == StringView("/") || out->append(virtual_path);
}

int64_t ProcessFiles::free_slot(uint64_t minimum) const {
  for (uint64_t i = minimum; i < kMaxDescriptors; ++i) if (slots_[i].kind == DescriptorKind::Closed) return static_cast<int64_t>(i);
  return -1;
}

bool ProcessFiles::close(uint64_t slot) {
  if (slot >= kMaxDescriptors || slots_[slot].kind == DescriptorKind::Closed) return false;
  Slot closing;
  closing.kind = slots_[slot].kind;
  closing.object = static_cast<SharedPointer<OpenObject>&&>(slots_[slot].object);
  slots_[slot] = Slot();
  if (closing.object && closing.object.use_count() == 1) if (ObjectManager* objects = system_objects()) { objects->unlock_all(closing.object.get()); objects->share_release(closing.object.get()); }      // (its byte-range locks and its share of the file go with it)
  if (closing.object && closing.object.use_count() == 1 && closing.object->change_watch != 0)
    if (ObjectManager* objects = system_objects()) objects->release(closing.object->change_watch);      // ReadDirectoryChangesW's own watch goes with the handle
  if (closing.object && closing.object.use_count() == 1 && closing.object->delete_on_close) {
    String path(*allocator_), virtual_path(*allocator_);
    virtual_path.assign(closing.object->path.view());
    closing.object = SharedPointer<OpenObject>();      // the file must be closed before it can be removed
    if (host_path(virtual_path.view(), &path)) fs_->remove(path.view());
  }
  return true;
}

int64_t ProcessFiles::duplicate(uint64_t slot, uint64_t minimum) {
  if (kind(slot) == DescriptorKind::Closed) return -1;
  const int64_t target = free_slot(minimum);
  if (target < 0) return -1;
  slots_[target].kind = slots_[slot].kind;
  slots_[target].object = slots_[slot].object;
  slots_[target].token = slots_[slot].token;
  slots_[target].close_on_exec = false;
  slots_[target].inherit = slots_[target].protect = false;
  return target;
}

bool ProcessFiles::duplicate_to(uint64_t slot, uint64_t target) {
  if (kind(slot) == DescriptorKind::Closed || target >= kMaxDescriptors) return false;
  if (slot == target) return true;
  close(target);
  slots_[target].kind = slots_[slot].kind;
  slots_[target].object = slots_[slot].object;
  slots_[target].token = slots_[slot].token;
  slots_[target].close_on_exec = false;
  slots_[target].inherit = slots_[target].protect = false;
  return true;
}

uint32_t ProcessFiles::inherit_from(const ProcessFiles& parent, void (*copy_token)(DescriptorKind kind, uint64_t token)) {
  uint32_t copied = 0;
  for (uint64_t slot = 0; slot < kMaxDescriptors; ++slot) {
    const Slot& s = parent.slots_[slot];
    if (s.kind == DescriptorKind::Closed || !s.inherit || s.kind == DescriptorKind::Thread) continue;      // (a thread of the parent means nothing to the child)
    close(slot);
    slots_[slot].kind = s.kind;
    slots_[slot].object = s.object;
    slots_[slot].token = s.token;
    slots_[slot].inherit = true;
    if (copy_token && s.token != 0) copy_token(s.kind, s.token);
    ++copied;
  }
  return copied;
}

int64_t ProcessFiles::open_token(DescriptorKind kind, uint64_t token) {
  const int64_t target = free_slot(0);
  if (target < 0) return -1;
  slots_[target].kind = kind;
  slots_[target].token = token;
  slots_[target].close_on_exec = false;
  slots_[target].inherit = slots_[target].protect = false;
  return target;
}

int64_t ProcessFiles::open_device(DescriptorKind device) {
  const int64_t target = free_slot(0);
  if (target < 0) return -1;
  slots_[target].kind = device;
  slots_[target].close_on_exec = false;
  slots_[target].inherit = slots_[target].protect = false;
  return target;
}

DescriptorKind ProcessFiles::device_at(StringView virtual_path) const {
  if (style_ != PathStyle::Posix) return DescriptorKind::Closed;
  if (virtual_path == StringView("/dev/null")) return DescriptorKind::Null;
  if (virtual_path == StringView("/dev/zero")) return DescriptorKind::Zero;
  if (virtual_path == StringView("/dev/urandom") || virtual_path == StringView("/dev/random")) return DescriptorKind::Random;
  if (virtual_path == StringView("/dev/tty")) return DescriptorKind::ConsoleOutput;
  return DescriptorKind::Closed;
}

Status ProcessFiles::stat(StringView virtual_path, DirectoryEntry* entry) {
  if (!fs_) return Status::NotFound;
  String path(*allocator_);
  if (!host_path(virtual_path, &path)) return Status::NotFound;
  return fs_->stat(path.view(), entry);
}

Status ProcessFiles::open(StringView virtual_path, const OpenFlags& flags, uint32_t* slot, bool* created) {
  if (created) *created = false;
  if (!fs_) return Status::NotFound;
  String path(*allocator_);
  if (!host_path(virtual_path, &path)) return Status::OutOfMemory;
  const int64_t target = free_slot(0);
  if (target < 0) return Status::OutOfMemory;      // the caller reports "too many open files"
  SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(*allocator_, *allocator_);
  if (!object || !object->path.assign(virtual_path)) return Status::OutOfMemory;

  DirectoryEntry info;
  Status status = fs_->stat(path.view(), &info);
  const bool exists = status == Status::Ok;
  if (!exists && status != Status::NotFound) return status;
  if (exists && flags.create && flags.exclusive) return Status::AlreadyExists;
  if (exists && info.type == NodeType::Directory) {
    if (flags.write || flags.truncate || flags.must_not_be_directory) return Status::IsADirectory;
    status = fs_->open_directory(path.view(), &object->directory);
    if (status != Status::Ok) return status;
    object->readable = true;
    slots_[target].kind = DescriptorKind::Directory;
  } else {
    if (exists && flags.must_be_directory) return Status::NotADirectory;
    if (!exists && !flags.create) return Status::NotFound;
    if (!exists && flags.must_be_directory) {
      status = fs_->create_directory(path.view());      // a directory is created by asking for one
      if (status != Status::Ok) return status;
      if (created) *created = true;
      status = fs_->open_directory(path.view(), &object->directory);
      if (status != Status::Ok) return status;
      object->readable = true;
      slots_[target].kind = DescriptorKind::Directory;
    } else {
      OpenMode mode = OpenMode::Read;
      if (flags.write) mode = OpenMode::ReadWrite;
      if (!exists) mode = OpenMode::CreateOrOpen;
      else if (flags.create && flags.truncate) mode = OpenMode::CreateTruncate;
      status = fs_->open_file(path.view(), mode, &object->file);
      if (status != Status::Ok) return status;
      if (!exists && created) *created = true;
      if (exists && flags.truncate && mode != OpenMode::CreateTruncate && object->file->resize(0) != Status::Ok) return Status::IoError;
      object->readable = flags.read;
      object->writable = flags.write;
      object->append = flags.append;
      slots_[target].kind = DescriptorKind::File;
    }
  }
  slots_[target].object = object;
  slots_[target].close_on_exec = false;
  slots_[target].inherit = slots_[target].protect = false;
  *slot = static_cast<uint32_t>(target);
  return Status::Ok;
}

Status ProcessFiles::make_directory(StringView virtual_path) {
  if (!fs_) return Status::NotFound;
  String path(*allocator_);
  if (!host_path(virtual_path, &path)) return Status::NotFound;
  return fs_->create_directory(path.view());
}

Status ProcessFiles::set_metadata(StringView virtual_path, const filesystem::MetadataChange& change) {
  if (!fs_) return Status::NotFound;
  String path(*allocator_);
  if (!host_path(virtual_path, &path)) return Status::NotFound;
  return fs_->set_metadata(path.view(), change);
}

Status ProcessFiles::remove(StringView virtual_path) {
  if (!fs_) return Status::NotFound;
  String path(*allocator_);
  if (!host_path(virtual_path, &path)) return Status::NotFound;
  if (virtual_path == StringView("/")) return Status::Busy;      // the root of the program's tree stays
  return fs_->remove(path.view());
}

Status ProcessFiles::rename(StringView from, StringView to) {
  if (!fs_) return Status::NotFound;
  String a(*allocator_), b(*allocator_);
  if (!host_path(from, &a) || !host_path(to, &b)) return Status::NotFound;
  if (from == StringView("/")) return Status::Busy;
  return fs_->rename(a.view(), b.view());
}

Status ProcessFiles::create_hard_link(StringView existing, StringView link) {
  if (!fs_) return Status::NotFound;
  String a(*allocator_), b(*allocator_);
  if (!host_path(existing, &a) || !host_path(link, &b)) return Status::NotFound;
  return fs_->create_hard_link(a.view(), b.view());
}

Status ProcessFiles::create_symbolic_link(StringView link, StringView target, bool target_is_directory) {
  if (!fs_) return Status::NotFound;
  String a(*allocator_), b(*allocator_);
  if (!host_path(link, &a) || !host_path(target, &b)) return Status::NotFound;
  return fs_->create_symbolic_link(a.view(), b.view(), target_is_directory);
}

Status ProcessFiles::change_directory(StringView virtual_path) {
  DirectoryEntry entry;
  const Status status = stat(virtual_path, &entry);
  if (status != Status::Ok) return status;
  if (entry.type != NodeType::Directory) return Status::NotADirectory;
  if (virtual_path.size() > kMaxPath) return Status::InvalidPath;
  for (uint64_t i = 0; i < virtual_path.size(); ++i) cwd_[i] = virtual_path[i];
  cwd_size_ = virtual_path.size();
  return Status::Ok;
}

Status ProcessFiles::read(OpenObject& object, uint64_t offset, ByteView out, uint64_t* got) {
  *got = 0;
  if (!object.file) return Status::IsADirectory;
  if (!object.readable) return Status::AccessDenied;
  return object.file->read(offset, out, got);
}

Status ProcessFiles::write(OpenObject& object, uint64_t offset, ConstByteView data, uint64_t* done) {
  *done = 0;
  if (!object.file) return Status::IsADirectory;
  if (!object.writable) return Status::AccessDenied;
  return object.file->write(object.append ? object.file->size() : offset, data, done);
}

Status ProcessFiles::resize(OpenObject& object, uint64_t size) {
  if (!object.file) return Status::IsADirectory;
  if (!object.writable) return Status::AccessDenied;
  return object.file->resize(size);
}

Status ProcessFiles::flush(OpenObject& object) { return object.file ? object.file->flush() : Status::Ok; }

Status ProcessFiles::stat_object(const OpenObject& object, DirectoryEntry* entry) {
  const Status status = stat(object.path.view(), entry);
  if (status == Status::Ok && object.file) entry->size = object.file->size();      // reflects writes the file system has not been asked about yet
  return status;
}

void ProcessFiles::rewind(OpenObject& object) {
  if (object.directory) object.directory->rewind();
  object.directory_phase = 0;
  object.directory_index = 0;
  object.has_pending = false;
}

Status ProcessFiles::next_entry(OpenObject& object, DirectoryEntry* entry) {
  if (!object.directory) return Status::NotADirectory;
  if (object.has_pending) {
    *entry = object.pending;
    object.has_pending = false;
    return Status::Ok;
  }
  if (object.directory_phase < 2) {
    *entry = DirectoryEntry();
    entry->type = NodeType::Directory;
    entry->name.assign(object.directory_phase == 0 ? "." : "..");
    ++object.directory_phase;
    return Status::Ok;
  }
  return object.directory->next(entry);
}

bool prepare_program_folders(filesystem::IFileSystem& fs) {
  static const char* const kFolders[] = {"/Linux", "/Linux/bin", "/Linux/etc", "/Linux/home", "/Linux/root", "/Linux/tmp", "/Linux/usr", "/Linux/var",
                                          "/Windows", "/Windows/Windows", "/Windows/Windows/System32", "/Windows/Users", "/Windows/Program Files", "/Windows/Temp"};
  bool ok = true;
  for (const char* folder : kFolders) {
    DirectoryEntry entry;
    if (fs.stat(folder, &entry) == Status::Ok) {
      ok = ok && entry.type == NodeType::Directory;
      continue;
    }
    const Status status = fs.create_directory(folder);
    ok = ok && (status == Status::Ok || status == Status::AlreadyExists);
  }
  return ok;
}

}  // namespace process
