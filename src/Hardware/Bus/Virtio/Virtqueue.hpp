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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

// A split virtqueue (virtio 1.x, section 2.7) carrying single-descriptor
// buffers, either written by the device (input events, received frames) or read
// by it (frames to send). Each buffer keeps its descriptor index, so the caller
// recycles a buffer by posting the same index again.
class Virtqueue {
 public:
  bool init(allocators::IAllocator& allocator, uint16_t size);
  void release(allocators::IAllocator& allocator);

  uint16_t size() const { return size_; }
  uint64_t descriptor_address() const { return reinterpret_cast<uint64_t>(descriptors_.data()); }
  uint64_t available_address() const { return reinterpret_cast<uint64_t>(available_memory_.data()); }
  uint64_t used_address() const { return reinterpret_cast<uint64_t>(used_memory_.data()); }

  // Offers `length` bytes at `data` to the device as descriptor `index`: for the
  // device to fill in (the default) or, with device_writes false, to read.
  void post(uint16_t index, void* data, uint32_t length, bool device_writes = true);
  // Offers several buffers as one request: `count` chained descriptors, numbered
  // head .. head+count-1 (the caller keeps those free), in the order given.
  struct Segment {
    void* data;
    uint32_t length;
    bool device_writes;
  };
  void post_chain(uint16_t head, const Segment* segments, uint16_t count);
  // The next buffer the device finished: its descriptor index and bytes written.
  bool take_used(uint16_t* index, uint32_t* length);

 private:
  struct Descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
  };
  struct UsedElement {
    uint32_t id;
    uint32_t length;
  };

  // The three regions, one allocation each. Their headers (flags, index) are
  // reached through the views below, so every ring slot access is bounds-checked
  // - including the ones the device chooses (used-ring entries).
  ArrayView<Descriptor> descriptors_;
  ByteView available_memory_;  // flags, index, then `size` 16-bit slots, then used_event
  ByteView used_memory_;       // flags, index, then `size` UsedElements, then avail_event
  uint16_t size_ = 0;
  uint16_t last_used_ = 0;

  volatile uint16_t& available_index() const { return *reinterpret_cast<volatile uint16_t*>(&available_memory_[2]); }
  volatile uint16_t& available_slot(uint16_t i) const {
    return *reinterpret_cast<volatile uint16_t*>(&available_memory_[4 + 2 * uint64_t{i}]);
  }
  volatile uint16_t& used_index() const { return *reinterpret_cast<volatile uint16_t*>(&used_memory_[2]); }
  volatile UsedElement& used_slot(uint16_t i) const {
    return *reinterpret_cast<volatile UsedElement*>(&used_memory_[4 + 8 * uint64_t{i}]);
  }
};

}  // namespace hardware
