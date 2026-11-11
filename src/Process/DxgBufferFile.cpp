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

#include "Process/DxgBufferFile.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace process {

namespace {
DxgBufferCopier g_copier = nullptr;      // set once at boot (install_dxg), before any process runs
}

void set_dxg_buffer_copier(DxgBufferCopier copier) { g_copier = copier; }

DxgBufferFile::DxgBufferFile(allocators::IAllocator& allocator, SharedPointer<dxgk::ExportedAllocation> exported, uint64_t bytes)
    : allocator_(allocator), exported_(util::move(exported)), bytes_(bytes) {}

DxgBufferFile::~DxgBufferFile() = default;

namespace {
// A checksum of the copy (whether the CPU wrote it since): eight bytes at a time, mixed.
uint64_t checksum(ConstByteView bytes) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  ByteReader in(bytes);
  while (in.remaining() >= 8) h = (h ^ in.le64()) * 0xFF51AFD7ED558CCDull, h ^= h >> 32;
  while (in.remaining() > 0) h = (h ^ in.u8()) * 0x100000001B3ull;
  return h;
}
}  // namespace

bool DxgBufferFile::copy_locked(bool to_image) {
  if (!g_copier || !copy_ || !exported_) return false;
  const ByteView view = copy_.get()->bytes().first(bytes_ < copy_.get()->capacity() ? bytes_ : copy_.get()->capacity());
  if (!g_copier(*exported_, view, to_image)) return false;
  moved_sum_ = checksum(ConstByteView(view));
  return true;
}

BlockRef DxgBufferFile::map_block(bool writable_shared) {
  kernel::ScopedMutex hold(lock_);
  if (!g_copier || !exported_ || bytes_ == 0 || bytes_ > MemoryFile::kMaxSize) return BlockRef();
  if (!copy_) {
    const uint64_t pages = (bytes_ + MemoryBlock::kPage - 1) / MemoryBlock::kPage;      // (bytes_ <= kMaxSize: no wrap)
    copy_ = BlockRef(MemoryBlock::make(allocator_, pages * MemoryBlock::kPage));
    if (!copy_) return BlockRef();
    copy_locked(false);      // (the image's pixels as they are now; if the GPU still works on it, zeros until DMA_BUF_IOCTL_SYNC brings them)
  }
  if (writable_shared) written_shared_ = true;
  copy_.get()->retain();
  return BlockRef(copy_.get());
}

DxgBufferFile::SyncResult DxgBufferFile::sync(uint64_t flags) {
  if (flags & ~(kSyncRead | kSyncWrite | kSyncEnd) || !(flags & (kSyncRead | kSyncWrite))) return SyncResult::Invalid;
  kernel::ScopedMutex hold(lock_);
  if (!g_copier) return SyncResult::NoCopier;
  if (!copy_) return SyncResult::Ok;      // (nothing mapped: nothing to move)
  const bool end = (flags & kSyncEnd) != 0;
  if (!end && (flags & kSyncRead)) return copy_locked(false) ? SyncResult::Ok : SyncResult::Busy;      // the CPU is about to read: the image's pixels
  if (end && (flags & kSyncWrite)) return copy_locked(true) ? SyncResult::Ok : SyncResult::Busy;       // the CPU has written: to the image
  return SyncResult::Ok;
}

bool DxgBufferFile::flush_cpu_writes() {
  kernel::ScopedMutex hold(lock_);
  if (!written_shared_ || !copy_) return true;
  if (copy_.get()->uses() <= 1) {      // (only this file holds it: every mapping is gone)
    written_shared_ = false;
    return true;
  }
  const ByteView view = copy_.get()->bytes().first(bytes_ < copy_.get()->capacity() ? bytes_ : copy_.get()->capacity());
  if (checksum(ConstByteView(view)) == moved_sum_) return true;      // (the CPU wrote nothing since: the image may have newer GPU frames)
  return copy_locked(true);
}

DxgSyncFile::DxgSyncFile(SharedPointer<dxgk::ExportedSync> exported) : exported_(util::move(exported)) {}
DxgSyncFile::~DxgSyncFile() = default;

}  // namespace process
