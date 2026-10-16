#include "hardware/StorageDevice/UEFI/UefiBlockStorageDevice.hpp"

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

bool UefiBlockStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) {
  const uint64_t bytes = uint64_t{blocks} * info_.block_size;
  const efi::Status s = write ? io_->write_blocks(io_, io_->media->media_id, lba, bytes, dma)
                              : io_->read_blocks(io_, io_->media->media_id, lba, bytes, dma);
  return s == efi::kSuccess;
}

}  // namespace hardware
