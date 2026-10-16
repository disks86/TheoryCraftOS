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

#include "Hardware/Bus/Virtio/Virtqueue.hpp"

namespace hardware {

namespace {
constexpr uint16_t kDescriptorNext = 1;   // chained to the descriptor in `next`
constexpr uint16_t kDescriptorWrite = 2;  // device writes this buffer
}  // namespace

bool Virtqueue::init(allocators::IAllocator& allocator, uint16_t size) {
  if (size == 0 || (size & (size - 1)) != 0) return false;  // must be a power of two
  size_ = size;
  last_used_ = 0;
  const uint64_t avail_bytes = 4 + uint64_t{size} * 2 + 2;
  const uint64_t used_bytes = 4 + uint64_t{size} * sizeof(UsedElement) + 2;
  descriptors_ = allocate_view<Descriptor>(allocator, size, 4096);
  available_memory_ = allocate_view<uint8_t>(allocator, avail_bytes, 4096);
  used_memory_ = allocate_view<uint8_t>(allocator, used_bytes, 4096);
  if (descriptors_.empty() || available_memory_.empty() || used_memory_.empty()) {
    release(allocator);
    return false;
  }
  __builtin_memset(descriptors_.data(), 0, descriptors_.size_bytes());
  available_memory_.fill(0);
  used_memory_.fill(0);
  return true;
}

void Virtqueue::release(allocators::IAllocator& allocator) {
  release_view(allocator, descriptors_);
  release_view(allocator, available_memory_);
  release_view(allocator, used_memory_);
  size_ = 0;
}

void Virtqueue::post(uint16_t index, void* data, uint32_t length, bool device_writes) {
  descriptors_[index] = {reinterpret_cast<uint64_t>(data), length,
                         static_cast<uint16_t>(device_writes ? kDescriptorWrite : 0), 0};
  available_slot(available_index() % size_) = index;
  __atomic_thread_fence(__ATOMIC_RELEASE);  // the entry must be visible before the index moves
  available_index() = available_index() + 1;
}

void Virtqueue::post_chain(uint16_t head, const Segment* segments, uint16_t count) {
  const ArrayView<Descriptor> chain = descriptors_.subview(head, count);  // the chain must fit the table
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t flags = segments[i].device_writes ? kDescriptorWrite : 0;
    if (i + 1 < count) flags |= kDescriptorNext;
    chain[i] = {reinterpret_cast<uint64_t>(segments[i].data), segments[i].length, flags,
                static_cast<uint16_t>(head + i + 1)};
  }
  available_slot(available_index() % size_) = head;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  available_index() = available_index() + 1;
}

bool Virtqueue::take_used(uint16_t* index, uint32_t* length) {
  while (used_index() != last_used_) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const volatile UsedElement& e = used_slot(last_used_ % size_);
    const uint32_t id = e.id;  // chosen by the device: do not trust it
    const uint32_t written = e.length;
    ++last_used_;
    if (id >= size_) continue;  // a bogus entry is skipped rather than used as an index
    *index = static_cast<uint16_t>(id);
    *length = written;
    return true;
  }
  return false;
}

}  // namespace hardware
