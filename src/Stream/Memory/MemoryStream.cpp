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

#include "Stream/Memory/MemoryStream.hpp"

namespace stream {

MemoryStream::MemoryStream(allocators::IAllocator& allocator, uint64_t initial_capacity) : allocator_(&allocator), writable_(true) {
  if (initial_capacity) reserve(initial_capacity);  // on failure it just starts empty
}

MemoryStream::MemoryStream(ByteView buffer, uint64_t length, bool writable)
    : allocator_(nullptr), data_(buffer.data()), length_(length > buffer.size() ? buffer.size() : length), capacity_(buffer.size()), writable_(writable) {}

MemoryStream::MemoryStream(ConstByteView data)
    : allocator_(nullptr), data_(const_cast<uint8_t*>(data.data())), length_(data.size()), capacity_(data.size()), writable_(false) {}

MemoryStream::~MemoryStream() {
  if (allocator_) allocator_->deallocate(data_);
}

StreamStatus MemoryStream::close() {
  if (allocator_) allocator_->deallocate(data_);
  data_ = nullptr;
  length_ = capacity_ = position_ = 0;
  allocator_ = nullptr;
  closed_ = true;
  return StreamStatus::Ok;
}

void MemoryStream::reset() {
  length_ = 0;
  position_ = 0;
}

StreamStatus MemoryStream::reserve(uint64_t capacity) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (capacity <= capacity_) return StreamStatus::Ok;
  if (!allocator_) return StreamStatus::NoSpace;
  uint8_t* fresh = static_cast<uint8_t*>(allocator_->allocate(capacity));
  if (!fresh) return StreamStatus::OutOfMemory;
  if (length_) __builtin_memcpy(fresh, data_, length_);
  allocator_->deallocate(data_);
  data_ = fresh;
  capacity_ = capacity;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::extend_to(uint64_t new_length) {
  if (new_length <= length_) return StreamStatus::Ok;
  if (new_length > capacity_) {
    if (!allocator_) return StreamStatus::NoSpace;
    uint64_t target = capacity_ ? capacity_ * 2 : 64;  // geometric growth
    if (target < capacity_ || target < new_length) target = new_length;
    const StreamStatus s = reserve(target);
    if (s != StreamStatus::Ok) return s;
  }
  __builtin_memset(data_ + length_, 0, new_length - length_);
  length_ = new_length;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::read(ByteView buffer, uint64_t* count) {
  *count = 0;
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (position_ >= length_ || buffer.empty()) return StreamStatus::Ok;
  uint64_t n = length_ - position_;
  if (n > buffer.size()) n = buffer.size();
  __builtin_memcpy(buffer.data(), data_ + position_, n);
  position_ += n;
  *count = n;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::write(ConstByteView data) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (!writable_) return StreamStatus::ReadOnly;
  if (data.empty()) return StreamStatus::Ok;
  if (data.size() > ~uint64_t{0} - position_) return StreamStatus::InvalidArgument;
  const uint64_t end = position_ + data.size();
  StreamStatus s = extend_to(end);  // zero-fills any gap before position_ too
  if (s != StreamStatus::Ok) return s;
  __builtin_memmove(data_ + position_, data.data(), data.size());
  position_ = end;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::length(uint64_t* length) const {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  *length = length_;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::position(uint64_t* position) const {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  *position = position_;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::seek(int64_t offset, SeekOrigin origin, uint64_t* new_position) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  uint64_t target = 0;
  const StreamStatus s = resolve_seek(offset, origin, position_, length_, &target);
  if (s != StreamStatus::Ok) return s;
  position_ = target;
  if (new_position) *new_position = target;
  return StreamStatus::Ok;
}

StreamStatus MemoryStream::set_length(uint64_t length) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (!writable_) return StreamStatus::ReadOnly;
  if (length <= length_) {
    length_ = length;
    return StreamStatus::Ok;
  }
  return extend_to(length);
}

}  // namespace stream
