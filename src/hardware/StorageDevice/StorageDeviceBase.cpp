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

#include "hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

bool StorageDeviceBase::prepare_bounce() {
  release_bounce();
  if (info_.block_size == 0 || info_.block_size > kBounceBytes) return false;
  bounce_ = static_cast<uint8_t*>(allocator_.allocate_aligned(kBounceBytes, bounce_alignment_));
  if (!bounce_) return false;
  bounce_bytes_ = kBounceBytes;
  return true;
}

void StorageDeviceBase::release_bounce() {
  allocator_.deallocate(bounce_);
  bounce_ = nullptr;
  bounce_bytes_ = 0;
}

bool StorageDeviceBase::run(bool write, uint64_t lba, uint32_t count, uint8_t* buffer) {
  if (!bounce_ || !buffer || count == 0 || lba >= info_.block_count || count > info_.block_count - lba) {
    ++stats_.errors;
    return false;
  }
  while (count > 0) {
    const uint32_t n = count < max_blocks() ? count : max_blocks();
    const uint32_t bytes = n * info_.block_size;
    if (write) __builtin_memcpy(bounce_, buffer, bytes);
    if (!transfer(write, lba, n, bounce_)) {
      ++stats_.errors;
      return false;
    }
    if (!write) __builtin_memcpy(buffer, bounce_, bytes);
    (write ? stats_.blocks_written : stats_.blocks_read) += n;
    lba += n;
    count -= n;
    buffer += bytes;
  }
  ++(write ? stats_.writes : stats_.reads);
  return true;
}

bool StorageDeviceBase::read(uint64_t lba, uint32_t count, void* buffer) {
  return run(false, lba, count, static_cast<uint8_t*>(buffer));
}

bool StorageDeviceBase::write(uint64_t lba, uint32_t count, const void* buffer) {
  if (info_.read_only) {
    ++stats_.errors;
    return false;
  }
  return run(true, lba, count, const_cast<uint8_t*>(static_cast<const uint8_t*>(buffer)));
}

bool StorageDeviceBase::flush() {
  if (info_.read_only) return true;  // nothing can be dirty
  if (flush_cache()) return true;
  ++stats_.errors;
  return false;
}

}  // namespace hardware
