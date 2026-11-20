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
#include "Process/ImageFileCache.hpp"

namespace process {

namespace {
ImageFileCache* g_file_cache = nullptr;
}  // namespace

ImageFileCache* image_file_cache() { return g_file_cache; }
void set_image_file_cache(ImageFileCache* cache) { g_file_cache = cache; }

SharedPointer<ByteBuffer> ImageFileCache::find(StringView path, const Key& key) {
  kernel::ScopedMutex hold(lock_);
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    Entry& e = *entries_[i];
    if (!e.path.view().equals(path)) continue;
    if (e.key == key) {
      e.last_used = ++clock_;
      return e.bytes;
    }
    entries_.remove_at(i);      // (the file changed: what was kept of it is of no use; a process still holding it keeps its copy)
    break;
  }
  return SharedPointer<ByteBuffer>();
}

SharedPointer<ByteBuffer> ImageFileCache::put(StringView path, const Key& key, const SharedPointer<ByteBuffer>& bytes) {
  kernel::ScopedMutex hold(lock_);
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    Entry& e = *entries_[i];
    if (!e.path.view().equals(path)) continue;
    if (e.key == key) {      // another process read it at the same time and got here first: one copy is enough
      e.last_used = ++clock_;
      return e.bytes;
    }
    entries_.remove_at(i);
    break;
  }
  UniquePointer<Entry> entry = UniquePointer<Entry>::make(allocator_, allocator_);
  if (!entry || !entry->path.assign(path)) return bytes;      // (not kept: the caller still has its bytes)
  entry->key = key;
  entry->bytes = bytes;
  entry->last_used = ++clock_;
  const Entry* fresh = entry.get();
  if (!entries_.push_back(static_cast<UniquePointer<Entry>&&>(entry))) return bytes;
  trim_locked(fresh);
  return bytes;
}

// Lets go of the least recently used files nobody else holds until the unused ones fit in the budget.
void ImageFileCache::trim_locked(const Entry* fresh) {
  for (;;) {
    uint64_t unused = 0;
    int64_t oldest = -1;
    for (uint64_t i = 0; i < entries_.count(); ++i) {
      const Entry& e = *entries_[i];
      if (e.bytes.use_count() > (&e == fresh ? 2u : 1u)) continue;      // (a load is using it)
      unused += e.bytes ? e.bytes->size() : 0;
      if (&e == fresh) continue;      // (what was just read is kept: the others make room)
      if (oldest < 0 || e.last_used < entries_[uint64_t(oldest)]->last_used) oldest = int64_t(i);
    }
    if (unused <= budget_ || oldest < 0) return;
    entries_.remove_at(uint64_t(oldest));
  }
}

uint32_t ImageFileCache::count() {
  kernel::ScopedMutex hold(lock_);
  return uint32_t(entries_.count());
}

uint64_t ImageFileCache::bytes() {
  kernel::ScopedMutex hold(lock_);
  uint64_t n = 0;
  for (uint64_t i = 0; i < entries_.count(); ++i) n += entries_[i]->bytes ? entries_[i]->bytes->size() : 0;
  return n;
}

}  // namespace process
