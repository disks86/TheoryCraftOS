#include "hardware/Bus/Virtio/Virtqueue.hpp"

namespace hardware {

namespace {
constexpr uint16_t kDescriptorNext = 1;   // chained to the descriptor in `next`
constexpr uint16_t kDescriptorWrite = 2;  // device writes this buffer
}

bool Virtqueue::init(allocators::IAllocator& allocator, uint16_t size) {
  if (size == 0 || (size & (size - 1)) != 0) return false;  // must be a power of two
  size_ = size;
  last_used_ = 0;
  const uint64_t desc_bytes = uint64_t{size} * sizeof(Descriptor);
  const uint64_t avail_bytes = 4 + uint64_t{size} * 2 + 2;
  const uint64_t used_bytes = 4 + uint64_t{size} * sizeof(UsedElement) + 2;
  descriptors_ = static_cast<Descriptor*>(allocator.allocate_aligned(desc_bytes, 4096));
  available_ = static_cast<Available*>(allocator.allocate_aligned(avail_bytes, 4096));
  used_ = static_cast<Used*>(allocator.allocate_aligned(used_bytes, 4096));
  if (!descriptors_ || !available_ || !used_) {
    release(allocator);
    return false;
  }
  __builtin_memset(descriptors_, 0, desc_bytes);
  __builtin_memset(available_, 0, avail_bytes);
  __builtin_memset(used_, 0, used_bytes);
  return true;
}

void Virtqueue::release(allocators::IAllocator& allocator) {
  allocator.deallocate(descriptors_);
  allocator.deallocate(available_);
  allocator.deallocate(used_);
  descriptors_ = nullptr;
  available_ = nullptr;
  used_ = nullptr;
  size_ = 0;
}

void Virtqueue::post(uint16_t index, void* data, uint32_t length, bool device_writes) {
  descriptors_[index] = {reinterpret_cast<uint64_t>(data), length,
                         static_cast<uint16_t>(device_writes ? kDescriptorWrite : 0), 0};
  volatile Available* avail = available_;
  avail->ring[avail->index % size_] = index;
  __atomic_thread_fence(__ATOMIC_RELEASE);  // the entry must be visible before the index moves
  avail->index = avail->index + 1;
}

void Virtqueue::post_chain(uint16_t head, const Segment* segments, uint16_t count) {
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t flags = segments[i].device_writes ? kDescriptorWrite : 0;
    if (i + 1 < count) flags |= kDescriptorNext;
    descriptors_[head + i] = {reinterpret_cast<uint64_t>(segments[i].data), segments[i].length, flags,
                              static_cast<uint16_t>(head + i + 1)};
  }
  volatile Available* avail = available_;
  avail->ring[avail->index % size_] = head;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  avail->index = avail->index + 1;
}

bool Virtqueue::take_used(uint16_t* index, uint32_t* length) {
  volatile Used* used = used_;
  if (used->index == last_used_) return false;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const volatile UsedElement& e = used->ring[last_used_ % size_];
  *index = static_cast<uint16_t>(e.id);
  *length = e.length;
  ++last_used_;
  return true;
}

}  // namespace hardware
