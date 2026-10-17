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

#include "Hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

bool StorageDeviceBase::prepare_bounce() {
  release_bounce();
  if (info_.block_size == 0 || info_.block_size > kBounceBytes) return false;
  bounce_ = allocate_dma_view<uint8_t>(allocator_, kBounceBytes, bounce_alignment_);
  return !bounce_.empty();
}

void StorageDeviceBase::release_bounce() { release_view(allocator_, bounce_); }

// Moves a whole request through the bounce buffer in chunks. The caller's memory
// is `into` (a read) or `from` (a write); every chunk is a checked slice of it.
bool StorageDeviceBase::run(bool write, uint64_t lba, ByteView into, ConstByteView from) {
  const uint64_t bytes = write ? from.size() : into.size();
  if (bounce_.empty() || bytes == 0 || bytes % info_.block_size != 0) {
    ++stats_.errors;
    return false;
  }
  uint64_t count = bytes / info_.block_size;
  if (lba >= info_.block_count || count > info_.block_count - lba) {
    ++stats_.errors;
    return false;
  }
  uint64_t done = 0;  // bytes already moved
  while (count > 0) {
    const uint32_t n = count < max_blocks() ? static_cast<uint32_t>(count) : max_blocks();
    const uint64_t chunk_bytes = uint64_t{n} * info_.block_size;
    const ByteView dma = bounce_.first(chunk_bytes);
    if (write) dma.copy_from(from.subview(done, chunk_bytes));
    if (!transfer(write, lba, n, dma)) {
      ++stats_.errors;
      return false;
    }
    if (!write) into.subview(done, chunk_bytes).copy_from(dma);
    (write ? stats_.blocks_written : stats_.blocks_read) += n;
    lba += n;
    count -= n;
    done += chunk_bytes;
  }
  ++(write ? stats_.writes : stats_.reads);
  return true;
}

bool StorageDeviceBase::read(uint64_t lba, ByteView buffer) {
  kernel::ScopedMutex guard(mutex_);
  return run(false, lba, buffer, ConstByteView());
}

bool StorageDeviceBase::write(uint64_t lba, ConstByteView buffer) {
  kernel::ScopedMutex guard(mutex_);
  if (info_.read_only) {
    ++stats_.errors;
    return false;
  }
  return run(true, lba, ByteView(), buffer);
}

bool StorageDeviceBase::flush() {
  kernel::ScopedMutex guard(mutex_);
  if (info_.read_only) return true;  // nothing can be dirty
  if (flush_cache()) return true;
  ++stats_.errors;
  return false;
}

}  // namespace hardware
