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

#include "Desktop/AppMan/InstallFiles.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

using filesystem::Status;

struct Writer {
  filesystem::IFile* file;
  Progress* progress;
  crypto::Hasher* hasher;
  const uint64_t* total = nullptr;      // (filled in by the network before the first bytes)
  uint64_t offset = 0, reported = 0;
  bool failed = false;
  Status status = Status::Ok;      // (why writing failed)
};

bool write_chunk(void* context, ConstByteView data) {
  Writer& w = *static_cast<Writer*>(context);
  if (w.progress && __atomic_load_n(&w.progress->cancel, __ATOMIC_RELAXED)) return false;
  uint64_t done = 0;
  const Status s = w.file->write(w.offset, data, &done);
  if (s != Status::Ok || done != data.size()) {
    w.failed = true;
    w.status = s == Status::Ok ? Status::NoSpace : s;      // (a short write: the volume filled up)
    return false;
  }
  if (w.hasher) w.hasher->update(data);
  w.offset += done;
  if (w.progress) {
    __atomic_store_n(&w.progress->done, w.offset, __ATOMIC_RELAXED);
    if (w.total && *w.total) __atomic_store_n(&w.progress->total, *w.total, __ATOMIC_RELAXED);
    if (w.progress->changed && w.offset - w.reported >= (256u << 10)) {
      w.reported = w.offset;
      w.progress->changed(w.progress->context, *w.progress);
    }
  }
  return true;
}

}  // namespace

bool join(String* out, StringView a, StringView b, StringView c, StringView d) {
  return out->assign(a) && out->append(b) && out->append(c) && out->append(d);
}

bool make_folders(filesystem::IFileSystem& files, StringView path) {
  for (uint64_t at = 1; at <= path.size(); ++at) {
    if (at < path.size() && path[at] != '/') continue;
    const Status s = files.create_directory(path.first(at));
    if (s != Status::Ok && s != Status::AlreadyExists) return false;
  }
  return true;
}

bool read_text_file(filesystem::IFileSystem& files, allocators::IAllocator& allocator, StringView path, uint64_t limit, String* text) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::Read, &file) != Status::Ok) return false;
  const uint64_t size = file->size() < limit ? file->size() : limit;
  ByteBuffer bytes(allocator);
  if (!bytes.put_zeros(size)) return false;
  uint64_t done = 0;
  while (done < size) {
    uint64_t got = 0;
    if (file->read(done, bytes.writable_view().subview(done), &got) != Status::Ok || got == 0) return false;
    done += got;
  }
  return text->assign(StringView(bytes.view()));
}

bool write_text_file(filesystem::IFileSystem& files, StringView path, StringView text) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != Status::Ok) return false;
  uint64_t done = 0;
  return file->write(0, text.bytes(), &done) == Status::Ok && done == text.size() && file->flush() == Status::Ok;
}

bool remove_tree(filesystem::IFileSystem& files, allocators::IAllocator& allocator, StringView path, uint32_t depth) {
  if (depth > 32) return false;
  filesystem::DirectoryEntry entry;
  const Status s = files.stat(path, &entry);
  if (s == Status::NotFound) return true;
  if (s == Status::Ok && entry.type == filesystem::NodeType::Directory && !entry.is_symlink) {
    List<String> names(allocator);
    {
      UniquePointer<filesystem::IDirectory> listing;
      if (files.open_directory(path, &listing) != Status::Ok) return false;
      filesystem::DirectoryEntry child;
      for (uint32_t guard = 0; guard < 100000 && listing->next(&child) == Status::Ok; ++guard) {
        String full(allocator);
        if (!join(&full, path, "/", child.name.view()) || !names.push_back(util::move(full))) return false;
      }
    }
    for (const String& child : names.view())
      if (!remove_tree(files, allocator, child.view(), depth + 1)) return false;
  }
  const Status r = files.remove(path);
  return r == Status::Ok || r == Status::NotFound;
}

Result download_to_file(filesystem::IFileSystem& files, INetwork& network, StringView url, StringView path, Progress* progress, crypto::Hasher* hasher,
                        String* detail) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != Status::Ok) {
    detail->assign(path);
    return Result::FileError;
  }
  uint64_t total = 0;
  Writer writer{file.get(), progress, hasher, &total};
  if (progress) {
    __atomic_store_n(&progress->done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&progress->total, 0, __ATOMIC_RELAXED);
  }
  const bool ok = network.download(url, &write_chunk, &writer, &total);
  if (progress && total) __atomic_store_n(&progress->total, total, __ATOMIC_RELAXED);
  if (progress && progress->changed) progress->changed(progress->context, *progress);
  const Status flush = file->flush();
  const bool flushed = flush == Status::Ok;
  file = UniquePointer<filesystem::IFile>();
  if (ok && flushed && writer.offset != 0) return Result::Ok;
  (void)files.remove(path);
  if (progress && __atomic_load_n(&progress->cancel, __ATOMIC_RELAXED)) return Result::Cancelled;
  if (writer.failed || !flushed) {
    const bool full = (writer.failed ? writer.status : flush) == Status::NoSpace;
    (void)(full ? detail->assign("downloading ") : detail->assign("writing "));
    (void)detail->append(path);
    if (full && total) (void)(detail->append(", which needs ") && detail->append_uint(total / 1000000 + 1) && detail->append(" MB"));
    return full ? Result::DiskFull : Result::FileError;
  }
  detail->assign(url);
  return Result::DownloadFailed;
}

}  // namespace desktop::appman
