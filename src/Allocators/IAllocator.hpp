#pragma once
#include <stdint.h>

namespace allocators {

// Hands out raw memory. Contents are returned as found (never initialized).
class IAllocator {
 public:
  // Returns nullptr when size is 0 or the request cannot be satisfied.
  virtual void* allocate(uint64_t size) = 0;
  // Like allocate(), but the result is a multiple of `alignment` (a power of
  // two). Needed for DMA structures, which devices require page-aligned.
  virtual void* allocate_aligned(uint64_t size, uint64_t alignment) = 0;
  // Releases a pointer from allocate() or allocate_aligned(); nullptr is ignored.
  virtual void deallocate(void* ptr) = 0;

 protected:
  constexpr IAllocator() = default;
  ~IAllocator() = default;
};

}  // namespace allocators
