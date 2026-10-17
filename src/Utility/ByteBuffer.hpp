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

// A growable run of bytes for building a file or packet in memory: append, put little-endian numbers, patch a
// number written earlier. Every operation that may need memory returns false on failure (and the buffer is then
// unchanged). The data pointer changes when it grows, so take views only when done.
class ByteBuffer {
 public:
  explicit ByteBuffer(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~ByteBuffer() { release_view(allocator_, storage_); }
  ByteBuffer(const ByteBuffer&) = delete;
  ByteBuffer& operator=(const ByteBuffer&) = delete;

  uint64_t size() const { return size_; }
  ConstByteView view() const { return ConstByteView(storage_).first(size_); }
  void clear() { size_ = 0; }
  // Drops everything after the first `size` bytes.
  void truncate(uint64_t size) { if (size < size_) size_ = size; }

  bool append(ConstByteView bytes) {
    if (!reserve(size_ + bytes.size())) return false;
    if (!bytes.empty()) storage_.subview(size_, bytes.size()).copy_from(bytes);
    size_ += bytes.size();
    return true;
  }
  bool put8(uint8_t v) { return append(ConstByteView(&v, 1)); }
  bool put16(uint32_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    return append(ConstByteView(b, 2));
  }
  bool put32(uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    return append(ConstByteView(b, 4));
  }
  // A four-character code ("RIFF", "movi", ...).
  bool put_tag(const char* tag) {
    const uint8_t b[4] = {static_cast<uint8_t>(tag[0]), static_cast<uint8_t>(tag[1]), static_cast<uint8_t>(tag[2]), static_cast<uint8_t>(tag[3])};
    return append(ConstByteView(b, 4));
  }
  bool put_zeros(uint64_t count) {
    if (!reserve(size_ + count)) return false;
    storage_.subview(size_, count).fill(0);
    size_ += count;
    return true;
  }
  // Overwrites four bytes already written.
  bool patch32(uint64_t offset, uint32_t v) {
    if (offset + 4 > size_) return false;
    for (int i = 0; i < 4; ++i) storage_[offset + i] = static_cast<uint8_t>(v >> (8 * i));
    return true;
  }

 private:
  bool reserve(uint64_t needed) {
    if (needed <= storage_.size()) return true;
    uint64_t capacity = storage_.empty() ? 1024 : storage_.size();
    while (capacity < needed) capacity *= 2;
    ByteView bigger = allocate_view<uint8_t>(allocator_, capacity);
    if (bigger.empty()) return false;
    if (size_) bigger.first(size_).copy_from(ConstByteView(storage_).first(size_));
    release_view(allocator_, storage_);
    storage_ = bigger;
    return true;
  }

  allocators::IAllocator& allocator_;
  ByteView storage_;
  uint64_t size_ = 0;
};
