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
