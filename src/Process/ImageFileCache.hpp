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
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace process {

// The bytes of DLL files that NtLoadModule reads, kept for the next process that loads the same file. A program loads its big DLL at run time (Chrome's 313 MB
// chrome.dll, in every one of its processes): reading it again for each process took a fresh block of that size each time, which a heap whose free memory is in
// pieces cannot give however much is free, and the load failed with STATUS_NO_MEMORY. Here the block is taken once and shared (ImageCache then shares the
// laid-out image made from it).
//
// A file is known by its path and what its directory entry says of it (size, times, identity): a file written again is read again. Files nobody holds stay while
// they fit in the budget; the oldest unused ones go first. Safe to use from several cores at once.
class ImageFileCache {
 public:
  struct Key {
    uint64_t size = 0, modified = 0, exact_modified = 0, file_id = 0;
    bool operator==(const Key& o) const { return size == o.size && modified == o.modified && exact_modified == o.exact_modified && file_id == o.file_id; }
  };
  // `budget`: the bytes of unused files that may stay (files in use are kept whatever their size).
  ImageFileCache(allocators::IAllocator& allocator, uint64_t budget) : allocator_(allocator), entries_(allocator), budget_(budget) {}
  ImageFileCache(const ImageFileCache&) = delete;
  ImageFileCache& operator=(const ImageFileCache&) = delete;

  // The bytes kept for `path` with this key; empty if there are none (the caller reads the file and offers them with put()).
  SharedPointer<ByteBuffer> find(StringView path, const Key& key);
  // Keeps `bytes` for `path` (another copy of the same file that got there first wins and is returned instead). Unused files over the budget are let go of.
  SharedPointer<ByteBuffer> put(StringView path, const Key& key, const SharedPointer<ByteBuffer>& bytes);

  uint32_t count();
  uint64_t bytes();

 private:
  struct Entry {
    explicit Entry(allocators::IAllocator& a) : path(a) {}
    String path;
    Key key;
    SharedPointer<ByteBuffer> bytes;
    uint64_t last_used = 0;
  };
  void trim_locked(const Entry* fresh);      // (`fresh`: just put, its caller's reference does not make it in use)

  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;
  List<UniquePointer<Entry>> entries_;
  uint64_t budget_ = 0, clock_ = 0;
};

// The cache NtLoadModule uses (nullptr: every load reads the file, as in the host tests that do not set one).
ImageFileCache* image_file_cache();
void set_image_file_cache(ImageFileCache* cache);

}  // namespace process
