#pragma once
#include <stdint.h>

namespace allocators {

// Hands out raw memory. Contents are returned as found (never initialized).
class IAllocator {
 public:
  // Returns nullptr when size is 0 or the request cannot be satisfied.
  virtual void* allocate(uint64_t size) = 0;
  // Releases a pointer from allocate(); nullptr is ignored.
  virtual void deallocate(void* ptr) = 0;

 protected:
  constexpr IAllocator() = default;
  ~IAllocator() = default;
};

}  // namespace allocators
