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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// A block of memory both the CPU and the V3D can reach: the bytes as the CPU sees them, the address the GPU uses for the first byte, and an id the pool keeps for it.
struct V3dGpuBlock {
  ArrayView<uint8_t> cpu;
  uint32_t gpu_address = 0;
  uint64_t cookie = 0;
  bool valid() const { return !cpu.empty(); }
};

// Where the V3D's memory comes from. The pool also does the cache maintenance that makes the CPU's and the GPU's views agree: the GPU goes around the CPU's caches.
class IV3dGpuMemory {
 public:
  // Zeroed memory of at least `bytes` bytes starting at an address that is a multiple of `alignment` (a power of two). False when none is left.
  virtual bool allocate(uint32_t bytes, uint32_t alignment, V3dGpuBlock* out) = 0;
  virtual void release(const V3dGpuBlock& block) = 0;
  // Writes the CPU's cached changes of the block to memory before the GPU reads them.
  virtual void clean(const V3dGpuBlock& block) { (void)block; }
  // Drops the CPU's cached copy of the block before the CPU reads what the GPU wrote.
  virtual void invalidate(const V3dGpuBlock& block) { (void)block; }

 protected:
  constexpr IV3dGpuMemory() = default;
  ~IV3dGpuMemory() = default;
};

// Memory from the OS's allocator (DMA memory), which the GPU reaches through the VideoCore's uncached 0xC0000000 bus alias. That alias covers the first 1 GiB of ARM
// memory only: a block above it is refused. With the V3D's MMU off the GPU address is that bus address.
class V3dSystemMemory final : public IV3dGpuMemory {
 public:
  explicit V3dSystemMemory(allocators::IAllocator& allocator) : allocator_(allocator) {}
  bool allocate(uint32_t bytes, uint32_t alignment, V3dGpuBlock* out) override;
  void release(const V3dGpuBlock& block) override;
  void clean(const V3dGpuBlock& block) override;
  void invalidate(const V3dGpuBlock& block) override;

 private:
  allocators::IAllocator& allocator_;
};

}  // namespace hardware
