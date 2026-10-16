#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"

namespace hardware {

// A split virtqueue (virtio 1.x, section 2.7) whose buffers are all written by
// the device (like the input event queue). Each buffer keeps its descriptor
// index, so the caller recycles a buffer by posting the same index again.
class Virtqueue {
 public:
  bool init(allocators::IAllocator& allocator, uint16_t size);
  void release(allocators::IAllocator& allocator);

  uint16_t size() const { return size_; }
  uint64_t descriptor_address() const { return reinterpret_cast<uint64_t>(descriptors_); }
  uint64_t available_address() const { return reinterpret_cast<uint64_t>(available_); }
  uint64_t used_address() const { return reinterpret_cast<uint64_t>(used_); }

  // Offers `length` bytes at `data` to the device as descriptor `index`.
  void post(uint16_t index, void* data, uint32_t length);
  // The next buffer the device finished: its descriptor index and bytes written.
  bool take_used(uint16_t* index, uint32_t* length);

 private:
  struct Descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
  };
  struct Available {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[1];  // really `size` entries
  };
  struct UsedElement {
    uint32_t id;
    uint32_t length;
  };
  struct Used {
    uint16_t flags;
    uint16_t index;
    UsedElement ring[1];  // really `size` entries
  };

  Descriptor* descriptors_ = nullptr;
  Available* available_ = nullptr;
  Used* used_ = nullptr;
  uint16_t size_ = 0;
  uint16_t last_used_ = 0;
};

}  // namespace hardware
