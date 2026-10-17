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
#include "Synchronization/SpinLock.hpp"

namespace allocators {

// Makes any IAllocator usable from several cores at once by serializing its
// calls with a SpinLock. (The default Allocator keeps unsynchronized free-list
// state, so concurrent code - the Concurrent* containers on several cores,
// smart pointers released on worker cores - must go through one of these.)
class ThreadSafeAllocator final : public IAllocator {
 public:
  ThreadSafeAllocator(IAllocator& inner, const hardware::ICentralProcessingUnit& cpu)
      : inner_(inner), lock_(cpu) {}

  void* allocate(uint64_t size) override {
    ScopedLock guard(lock_);
    return inner_.allocate(size);
  }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ScopedLock guard(lock_);
    return inner_.allocate_aligned(size, alignment);
  }
  void* allocate_dma(uint64_t size, uint64_t alignment) override {
    ScopedLock guard(lock_);
    return inner_.allocate_dma(size, alignment);
  }
  void deallocate(void* ptr) override {
    ScopedLock guard(lock_);
    inner_.deallocate(ptr);
  }

  // Runs `function` with the allocator's lock held, for reading the inner allocator's statistics.
  template <class F>
  void locked(F&& function) {
    ScopedLock guard(lock_);
    function();
  }

 private:
  IAllocator& inner_;
  SpinLock lock_;
};

}  // namespace allocators
