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

#include "Hardware/SerialPort/SerialRing.hpp"

namespace hardware {

bool SerialRing::allocate(allocators::IAllocator& allocator, uint32_t capacity) {
  release(allocator);
  memory_ = allocate_view<uint8_t>(allocator, capacity);
  return memory_.size() != 0;
}

void SerialRing::release(allocators::IAllocator& allocator) {
  release_view(allocator, memory_);
  clear();
}

uint32_t SerialRing::push(ConstByteView data) {
  const uint32_t cap = capacity();
  uint32_t count = data.size() < space() ? static_cast<uint32_t>(data.size()) : space();
  const uint32_t tail = cap ? (head_ + size_) % cap : 0;   // (head_ and size_ are both below cap, so no wrap of the sum)
  const uint32_t first = count < cap - tail ? count : cap - tail;
  if (first) memory_.subview(tail, first).copy_from(data.first(first));
  if (count > first) memory_.first(count - first).copy_from(data.subview(first, count - first));
  size_ += count;
  return count;
}

uint32_t SerialRing::pop(ByteView out) {
  const uint32_t count = out.size() < size_ ? static_cast<uint32_t>(out.size()) : size_;
  const uint32_t cap = capacity();
  const uint32_t first = count < cap - head_ ? count : cap - head_;
  if (first) out.first(first).copy_from(ConstByteView(memory_.subview(head_, first)));
  if (count > first) out.subview(first, count - first).copy_from(ConstByteView(memory_.first(count - first)));
  head_ = cap ? (head_ + count) % cap : 0;
  size_ -= count;
  return count;
}

bool SerialRing::peek(uint8_t* out) const {
  if (size_ == 0) return false;
  *out = memory_[head_];
  return true;
}

}  // namespace hardware
