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

#include "Hardware/StorageDevice/Virtio/VirtioBlockDevice.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"

namespace hardware {

namespace {
constexpr uint32_t kFeatureReadOnly = 1u << 5, kFeatureBlockSize = 1u << 6, kFeatureFlush = 1u << 9;
constexpr uint32_t kConfigCapacity = 0, kConfigBlockSize = 20;
constexpr uint32_t kTypeRead = 0, kTypeWrite = 1, kTypeFlush = 4;
constexpr uint8_t kStatusOk = 0, kStatusPending = 0xFF;
constexpr uint32_t kSectorBytes = 512;
}  // namespace

uint64_t VirtioBlockDevice::read_config64(uint32_t offset) const {
  uint64_t v = 0;
  for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{transport_.read_config8(offset + i)} << (8 * i);
  return v;
}

bool VirtioBlockDevice::initialize() {
  transport_.request_features(kFeatureReadOnly | kFeatureBlockSize | kFeatureFlush);
  if (!transport_.initialize()) {
    transport_.shutdown();
    return false;
  }
  const uint32_t accepted = transport_.accepted_features();
  info_.read_only = accepted & kFeatureReadOnly;
  has_flush_ = accepted & kFeatureFlush;
  info_.block_size = kSectorBytes;
  if (accepted & kFeatureBlockSize) {
    uint32_t size = 0;
    for (uint32_t i = 0; i < 4; ++i) size |= uint32_t{transport_.read_config8(kConfigBlockSize + i)} << (8 * i);
    if (size >= kSectorBytes && (size & (size - 1)) == 0 && size <= 4096) info_.block_size = size;
  }
  info_.block_count = read_config64(kConfigCapacity) / (info_.block_size / kSectorBytes);

  request_ = static_cast<Request*>(allocator_.allocate_aligned(4096, 4096));
  if (!request_ || !transport_.setup_queue(0, &queue_) || queue_.size() < 3 || !prepare_bounce()) {
    shutdown();
    return false;
  }
  status_ = reinterpret_cast<uint8_t*>(request_) + 64;
  if (!transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  __builtin_memcpy(info_.model, "virtio block device", 20);
  return info_.block_count > 0;
}

void VirtioBlockDevice::shutdown() {
  started_ = false;
  transport_.shutdown();  // reset first: the device must stop using our memory
  queue_.release(allocator_);
  allocator_.deallocate(request_);
  request_ = nullptr;
  release_bounce();
}

bool VirtioBlockDevice::submit(uint32_t type, uint64_t sector, void* data, uint32_t length) {
  if (!started_) return false;
  request_->type = type;
  request_->reserved = 0;
  request_->sector = sector;
  *status_ = kStatusPending;
  Virtqueue::Segment chain[3];
  uint16_t n = 0;
  chain[n++] = {request_, sizeof(Request), false};
  if (length) chain[n++] = {data, length, type == kTypeRead};
  chain[n++] = {status_, 1, true};
  queue_.post_chain(0, chain, n);
  transport_.notify(0);

  uint16_t id;
  uint32_t written;
  if (!wait_for(st_, 10000, [&] { return queue_.take_used(&id, &written); })) return false;
  return *reinterpret_cast<volatile uint8_t*>(status_) == kStatusOk;
}

bool VirtioBlockDevice::transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) {
  const uint32_t scale = info_.block_size / kSectorBytes;
  return submit(write ? kTypeWrite : kTypeRead, lba * scale, dma, blocks * info_.block_size);
}

bool VirtioBlockDevice::flush_cache() {
  return !has_flush_ || submit(kTypeFlush, 0, nullptr, 0);
}

}  // namespace hardware
