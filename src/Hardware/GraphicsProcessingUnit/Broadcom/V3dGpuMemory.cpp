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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpuMemory.hpp"
#include "Hardware/Platform/RaspberryPi/Bcm2711.hpp"
#include "Hardware/Platform/RaspberryPi/PiCache.hpp"

namespace hardware {

bool V3dSystemMemory::allocate(uint32_t bytes, uint32_t alignment, V3dGpuBlock* out) {
  if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1))) return false;
  const uint32_t line = alignment < 64 ? 64 : alignment;  // whole cache lines, so cleaning one block never touches another
  const uint64_t size = (uint64_t{bytes} + 63) & ~uint64_t{63};
  void* memory = allocator_.allocate_dma(size, line);
  if (!memory) return false;
  const uint64_t address = reinterpret_cast<uint64_t>(memory);
  if (size > (uint64_t{1} << 30) || address > (uint64_t{1} << 30) - size) {  // beyond what the bus alias names
    allocator_.deallocate(memory);
    return false;
  }
  out->cpu = ArrayView<uint8_t>(static_cast<uint8_t*>(memory), size);
  for (uint8_t& b : out->cpu) b = 0;
  out->gpu_address = bcm2711::arm_to_bus(address);
  out->cookie = address;
  pi_cache_clean_invalidate(memory, size);
  return true;
}

void V3dSystemMemory::release(const V3dGpuBlock& block) {
  if (block.valid()) allocator_.deallocate(block.cpu.data());
}

void V3dSystemMemory::clean(const V3dGpuBlock& block) { pi_cache_clean_invalidate(block.cpu.data(), block.cpu.size()); }
// Clean and invalidate: safe here because the CPU never leaves dirty lines in the blocks the GPU writes (images are only read by the CPU, and cleaned at creation).
void V3dSystemMemory::invalidate(const V3dGpuBlock& block) { pi_cache_clean_invalidate(block.cpu.data(), block.cpu.size()); }

}  // namespace hardware
