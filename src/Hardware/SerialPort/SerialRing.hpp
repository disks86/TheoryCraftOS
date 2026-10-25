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
#include "Allocators/IAllocator.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

// A first-in first-out queue of bytes in a block of memory of its own. Not thread safe: the owner locks.
class SerialRing {
 public:
  SerialRing() = default;
  SerialRing(const SerialRing&) = delete;
  SerialRing& operator=(const SerialRing&) = delete;

  // A queue of `capacity` bytes; false when out of memory.
  bool allocate(allocators::IAllocator& allocator, uint32_t capacity);
  void release(allocators::IAllocator& allocator);

  uint32_t capacity() const { return static_cast<uint32_t>(memory_.size()); }
  uint32_t size() const { return size_; }
  uint32_t space() const { return capacity() - size_; }
  void clear() { head_ = 0; size_ = 0; }

  // Appends as much of `data` as fits; returns the bytes taken.
  uint32_t push(ConstByteView data);
  bool push(uint8_t byte) { return push(ConstByteView(&byte, 1)) == 1; }
  // Removes up to out.size() bytes from the front; returns how many.
  uint32_t pop(ByteView out);
  // The next byte without removing it; false if empty.
  bool peek(uint8_t* out) const;

 private:
  ByteView memory_;
  uint32_t head_ = 0;   // where the oldest byte is
  uint32_t size_ = 0;
};

}  // namespace hardware
