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
  void deallocate(void* ptr) override {
    ScopedLock guard(lock_);
    inner_.deallocate(ptr);
  }

 private:
  IAllocator& inner_;
  SpinLock lock_;
};

}  // namespace allocators
