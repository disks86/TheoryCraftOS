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

#include "Desktop/FileTransfer.hpp"
#include "Desktop/DirectoryListing.hpp"

namespace desktop {

using filesystem::DirectoryEntry;
using filesystem::IFile;
using filesystem::NodeType;
using filesystem::OpenMode;
using filesystem::Status;

namespace {
constexpr uint32_t kMaxDepth = 32;
constexpr uint64_t kChunk = 32 * 1024;

bool join(String* out, StringView folder, StringView name) {
  return out->assign(folder) && (folder.size() <= 1 || out->push_back('/')) && out->append(name);
}
}  // namespace

Status FileTransfer::measure(StringView path) {
  files_total_ = bytes_total_ = 0;
  return measure_at(path, 0);
}

Status FileTransfer::measure_at(StringView path, uint32_t depth) {
  if (cancelled()) return kCancelled;
  DirectoryEntry entry;
  Status status = fs_.stat(path, &entry);
  if (status != Status::Ok) return status;
  if (entry.type != NodeType::Directory) {
    ++files_total_;
    bytes_total_ += entry.size;
    return Status::Ok;
  }
  if (depth > kMaxDepth) return Status::InvalidPath;
  List<DirectoryEntry> children(allocator_);
  status = read_sorted_directory(fs_, path, &children);
  if (status != Status::Ok) return status;
  for (const DirectoryEntry& child : children) {
    String full(allocator_);
    if (!join(&full, path, child.name.view())) return Status::OutOfMemory;
    status = measure_at(full.view(), depth + 1);
    if (status != Status::Ok) return status;
  }
  return Status::Ok;
}

Status FileTransfer::copy(StringView from, StringView to) {
  files_done_ = bytes_done_ = 0;
  return copy_at(from, to, 0);
}

Status FileTransfer::copy_at(StringView from, StringView to, uint32_t depth) {
  if (cancelled()) return kCancelled;
  DirectoryEntry entry;
  Status status = fs_.stat(from, &entry);
  if (status != Status::Ok) return status;
  if (entry.type == NodeType::Directory) {
    if (depth > kMaxDepth) return Status::InvalidPath;
    status = fs_.create_directory(to);
    if (status != Status::Ok) return status;
    List<DirectoryEntry> children(allocator_);
    status = read_sorted_directory(fs_, from, &children);
    if (status != Status::Ok) return status;
    for (const DirectoryEntry& child : children) {
      String a(allocator_), b(allocator_);
      if (!join(&a, from, child.name.view()) || !join(&b, to, child.name.view())) return Status::OutOfMemory;
      status = copy_at(a.view(), b.view(), depth + 1);
      if (status != Status::Ok) return status;
    }
    return Status::Ok;
  }
  if (on_file_) on_file_(context_, from, files_done_, files_total_);
  UniquePointer<IFile> in, out;
  status = fs_.open_file(from, OpenMode::Read, &in);
  if (status != Status::Ok) return status;
  status = fs_.open_file(to, OpenMode::CreateTruncate, &out);
  if (status != Status::Ok) return status;
  void* memory = allocator_.allocate(kChunk);
  if (!memory) return Status::OutOfMemory;
  uint8_t* buffer = static_cast<uint8_t*>(memory);
  uint64_t offset = 0;
  for (;;) {
    if (cancelled()) {
      status = kCancelled;
      break;
    }
    uint64_t got = 0;
    status = in->read(offset, ByteView(buffer, kChunk), &got);
    if (status != Status::Ok || got == 0) break;
    uint64_t put = 0;
    status = out->write(offset, ConstByteView(buffer, got), &put);
    if (status != Status::Ok) break;
    offset += got;
    bytes_done_ += got;
    if (on_bytes_) on_bytes_(context_, bytes_done_, bytes_total_ > bytes_done_ ? bytes_total_ : bytes_done_);
  }
  allocator_.deallocate(memory);
  if (status == Status::Ok) status = out->flush();
  if (status == Status::Ok) ++files_done_;
  return status;
}

Status FileTransfer::remove_tree(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView path, uint32_t depth) {
  DirectoryEntry entry;
  Status status = fs.stat(path, &entry);
  if (status != Status::Ok) return status;
  if (entry.type == NodeType::Directory) {
    if (depth > kMaxDepth) return Status::InvalidPath;
    List<DirectoryEntry> children(allocator);
    status = read_sorted_directory(fs, path, &children);
    if (status != Status::Ok) return status;
    for (const DirectoryEntry& child : children) {
      String full(allocator);
      if (!join(&full, path, child.name.view())) return Status::OutOfMemory;
      status = remove_tree(allocator, fs, full.view(), depth + 1);
      if (status != Status::Ok) return status;
    }
  }
  return fs.remove(path);
}

}  // namespace desktop
