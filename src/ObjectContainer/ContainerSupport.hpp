// Small helpers the containers share (no <utility> / <new> dependencies beyond
// placement new). Private to ObjectContainer/.
#pragma once
#include <new>
#include <stdint.h>
#include "Allocators/IAllocator.hpp"

namespace container_support {

template <class T>
constexpr T&& move(T& value) {
  return static_cast<T&&>(value);
}

// Memory from the allocator is at least 64-byte aligned.
template <class T>
inline constexpr bool kFitsAllocatorAlignment = alignof(T) <= 64;

// Allocates room for `count` Ts, or nullptr (also on size overflow).
template <class T>
T* allocate_array(allocators::IAllocator& allocator, uint64_t count) {
  static_assert(kFitsAllocatorAlignment<T>, "type is over-aligned for the allocator");
  if (count == 0 || count > UINT64_MAX / sizeof(T)) return nullptr;
  return static_cast<T*>(allocator.allocate(count * sizeof(T)));
}

}  // namespace container_support
