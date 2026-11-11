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

#include "Process/MemoryFile.hpp"
#include <new>

namespace process {

using filesystem::Status;

MemoryBlock* MemoryBlock::make(allocators::IAllocator& allocator, uint64_t capacity) {
  if (capacity == 0 || capacity % kPage != 0) return nullptr;
  uint8_t* storage = static_cast<uint8_t*>(allocator.allocate_aligned(capacity, kPage));
  if (!storage) return nullptr;
  ByteView(storage, capacity).fill(0);
  void* memory = allocator.allocate(sizeof(MemoryBlock));
  if (!memory) {
    allocator.deallocate(storage);
    return nullptr;
  }
  return new (memory) MemoryBlock(allocator, storage, capacity);      // (freed by the last release(), below)
}

void MemoryBlock::release() {
  if (__atomic_sub_fetch(&references_, 1, __ATOMIC_ACQ_REL) != 0) return;
  allocators::IAllocator& allocator = allocator_;
  allocator.deallocate(storage_);
  this->~MemoryBlock();
  allocator.deallocate(this);
}

MemoryFile::MemoryFile(allocators::IAllocator& allocator, bool sealing) : allocator_(allocator), seals_(sealing ? 0 : kSealSeal) {}

MemoryFile::~MemoryFile() {
  if (block_) block_->release();
}

uint64_t MemoryFile::size() const {
  kernel::ScopedMutex hold(lock_);
  return size_;
}

Status MemoryFile::read(uint64_t offset, ByteView buffer, uint64_t* transferred) {
  kernel::ScopedMutex hold(lock_);
  *transferred = 0;
  if (offset >= size_ || buffer.size() == 0) return Status::Ok;
  const uint64_t n = buffer.size() < size_ - offset ? buffer.size() : size_ - offset;
  if (!buffer.first(n).copy_from(block_->bytes().subview(offset, n))) return Status::IoError;
  *transferred = n;
  return Status::Ok;
}

Status MemoryFile::write(uint64_t offset, ConstByteView data, uint64_t* transferred) {
  kernel::ScopedMutex hold(lock_);
  *transferred = 0;
  if (data.size() == 0) return Status::Ok;
  if (seals_ & (kSealWrite | kSealFutureWrite)) return Status::AccessDenied;
  if (offset > kMaxSize || data.size() > kMaxSize - offset) return Status::NoSpace;
  const uint64_t end = offset + data.size();
  if (end > size_) {
    if (seals_ & kSealGrow) return Status::AccessDenied;
    const Status grown = grow_locked(end);
    if (grown != Status::Ok) return grown;
  }
  if (!block_->bytes().subview(offset, data.size()).copy_from(data)) return Status::IoError;
  *transferred = data.size();
  return Status::Ok;
}

// Makes the file new_size (> size_) bytes: in the block when it has room, else in a bigger one the contents move to.
Status MemoryFile::grow_locked(uint64_t new_size) {
  if (new_size > kMaxSize) return Status::NoSpace;
  if (!block_ || new_size > block_->capacity()) {
    uint64_t capacity = (new_size + MemoryBlock::kPage - 1) & ~(MemoryBlock::kPage - 1);
    MemoryBlock* bigger = MemoryBlock::make(allocator_, capacity);
    if (!bigger) return Status::OutOfMemory;
    if (block_) {
      bigger->bytes().first(size_).copy_from(block_->bytes().first(size_));
      block_->release();
    }
    block_ = bigger;
  }
  size_ = new_size;      // (the bytes past the old end are zeros: shrinking cleared them, a new block is zeroed)
  return Status::Ok;
}

Status MemoryFile::resize(uint64_t new_size) {
  kernel::ScopedMutex hold(lock_);
  if (new_size == size_) return Status::Ok;
  if (new_size > size_) {
    if (seals_ & kSealGrow) return Status::AccessDenied;
    return grow_locked(new_size);
  }
  if (seals_ & kSealShrink) return Status::AccessDenied;
  block_->bytes().subview(new_size, size_ - new_size).fill(0);      // (what is cut off reads as zeros if the file grows again, as on Linux)
  size_ = new_size;
  return Status::Ok;
}

Status MemoryFile::allocate(uint64_t offset, uint64_t length) {
  kernel::ScopedMutex hold(lock_);
  if (length == 0) return Status::InvalidArgument;
  if (offset > kMaxSize || length > kMaxSize - offset) return Status::NoSpace;
  if (offset + length <= size_) return Status::Ok;
  if (seals_ & kSealGrow) return Status::AccessDenied;
  return grow_locked(offset + length);
}

BlockRef MemoryFile::block(uint64_t* size) {
  kernel::ScopedMutex hold(lock_);
  *size = size_;
  if (!block_ || size_ == 0) return BlockRef();
  block_->retain();
  return BlockRef(block_);
}

bool MemoryFile::add_seals(uint32_t seals) {
  kernel::ScopedMutex hold(lock_);
  if (seals_ & kSealSeal) return false;
  seals_ |= seals & (kSealSeal | kSealShrink | kSealGrow | kSealWrite | kSealFutureWrite);
  return true;
}

uint32_t MemoryFile::seals() const {
  kernel::ScopedMutex hold(lock_);
  return seals_;
}

}  // namespace process
