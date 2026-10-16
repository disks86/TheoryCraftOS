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
#include "Network/NetworkTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace network {

// A fixed-capacity circular byte buffer (the send and receive buffers of a TCP
// connection). The storage is one allocation made by reserve().
class ByteRing {
 public:
  explicit ByteRing(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~ByteRing() { allocator_.deallocate(storage_.data()); }
  ByteRing(const ByteRing&) = delete;
  ByteRing& operator=(const ByteRing&) = delete;

  bool reserve(uint32_t capacity) {
    if (storage_.data()) return capacity == storage_.size();
    uint8_t* memory = static_cast<uint8_t*>(allocator_.allocate(capacity));
    if (!memory) return false;
    storage_ = ByteView(memory, capacity);
    return true;
  }

  uint32_t capacity() const { return static_cast<uint32_t>(storage_.size()); }
  uint32_t size() const { return size_; }
  uint32_t free_space() const { return capacity() - size_; }

  // Appends as many of the bytes as fit; returns how many did.
  uint32_t write(ConstByteView bytes) {
    const uint32_t n = bytes.size() < free_space() ? static_cast<uint32_t>(bytes.size()) : free_space();
    for (uint32_t i = 0; i < n; ++i) storage_[(head_ + size_ + i) % storage_.size()] = bytes[i];
    size_ += n;
    return n;
  }
  // Copies up to out.size() bytes starting `offset` bytes from the front,
  // leaving them in place; returns how many were copied.
  uint32_t peek(uint32_t offset, ByteView out) const {
    if (offset >= size_) return 0;
    const uint32_t n = out.size() < size_ - offset ? static_cast<uint32_t>(out.size()) : size_ - offset;
    for (uint32_t i = 0; i < n; ++i) out[i] = storage_[(head_ + offset + i) % storage_.size()];
    return n;
  }
  // Drops up to `length` bytes from the front.
  void discard(uint32_t length) {
    const uint32_t n = length < size_ ? length : size_;
    head_ = storage_.size() ? static_cast<uint32_t>((head_ + n) % storage_.size()) : 0;
    size_ -= n;
  }
  // Removes and copies out up to out.size() bytes from the front.
  uint32_t read(ByteView out) {
    const uint32_t n = peek(0, out);
    discard(n);
    return n;
  }

 private:
  allocators::IAllocator& allocator_;
  ByteView storage_;  // the one allocation; every index is checked against it
  uint32_t head_ = 0;
  uint32_t size_ = 0;
};

}  // namespace network
