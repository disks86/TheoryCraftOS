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
#include "Utility/ArrayView.hpp"

// Allocating memory as a view, and giving it back. Buffers and descriptor rings
// that used to be a raw pointer plus a separate constant for their size become
// one ArrayView, so every later index is checked against the allocation. The
// memory is NOT zeroed.

// `count` elements aligned to `alignment` bytes (at least the type's own); an
// empty view if the allocator is out of memory.
template <class T>
ArrayView<T> allocate_view(allocators::IAllocator& allocator, uint64_t count, uint64_t alignment = alignof(T)) {
  if (count == 0 || count > ~uint64_t{0} / sizeof(T)) return ArrayView<T>();
  T* memory = static_cast<T*>(allocator.allocate_aligned(count * sizeof(T), alignment < alignof(T) ? alignof(T) : alignment));
  return memory ? ArrayView<T>(memory, count) : ArrayView<T>();
}

// The same for memory a device reads or writes by itself (see IAllocator::allocate_dma): in the first 4 GiB.
template <class T>
ArrayView<T> allocate_dma_view(allocators::IAllocator& allocator, uint64_t count, uint64_t alignment = alignof(T)) {
  if (count == 0 || count > ~uint64_t{0} / sizeof(T)) return ArrayView<T>();
  T* memory = static_cast<T*>(allocator.allocate_dma(count * sizeof(T), alignment < alignof(T) ? alignof(T) : alignment));
  return memory ? ArrayView<T>(memory, count) : ArrayView<T>();
}

// Frees the memory behind the view (nullptr/empty is fine) and leaves it empty.
template <class T>
void release_view(allocators::IAllocator& allocator, ArrayView<T>& view) {
  allocator.deallocate(const_cast<void*>(static_cast<const void*>(view.data())));
  view = ArrayView<T>();
}
