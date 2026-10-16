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
