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

#include "Hardware/StorageDevice/UEFI/UefiBlockStorageDevice.hpp"
#include "Kernel/Firmware.hpp"

namespace hardware {

bool UefiBlockStorageDevice::initialize() {
  const efi::BlockIoMedia* m = io_ ? io_->media : nullptr;
  if (!m || !m->media_present || m->block_size == 0) return false;
  info_.block_size = m->block_size;
  info_.block_count = m->last_block + 1;
  info_.read_only = m->read_only;
  info_.removable = m->removable_media;
  __builtin_memcpy(info_.model, "firmware-driven block device", 29);
  return prepare_bounce();
}

bool UefiBlockStorageDevice::flush_cache() {
  efi::Status s = efi::kSuccess;
  kernel::firmware([&] { s = io_->flush_blocks(io_); });
  return s == efi::kSuccess;
}

bool UefiBlockStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) {
  const uint64_t bytes = uint64_t{blocks} * info_.block_size;
  efi::Status s = efi::kSuccess;
  kernel::firmware([&] {
    s = write ? io_->write_blocks(io_, io_->media->media_id, lba, bytes, dma)
              : io_->read_blocks(io_, io_->media->media_id, lba, bytes, dma);
  });
  return s == efi::kSuccess;
}

}  // namespace hardware
