#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/RandomAccessMemory/IRandomAccessMemory.hpp"

namespace allocators {

// Default allocator: a memory pool that carves the RAM's usable region into
// blocks of one cache line. Every allocation is a run of blocks preceded by a
// one-block header, so returned pointers are cache-line aligned. Free runs are
// kept address-ordered and coalesced on deallocate.
class Allocator final : public IAllocator {
 public:
  static constexpr uint64_t kBlockSize = 64;  // x86_64 cache line

  explicit constexpr Allocator(hardware::IRandomAccessMemory& ram) : ram_(ram) {}

  // Claims the RAM's usable region. Returns false if it is too small.
  bool initialize();

  void* allocate(uint64_t size) override;
  void* allocate_aligned(uint64_t size, uint64_t alignment) override;
  void deallocate(void* ptr) override;

  uint64_t free_bytes() const;  // payload bytes still available (sum over runs)

 private:
  struct Header;

  hardware::IRandomAccessMemory& ram_;
  Header* free_list_ = nullptr;
};

}  // namespace allocators
