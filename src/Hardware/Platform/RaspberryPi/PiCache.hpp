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

namespace hardware {

// Cleans and invalidates the CPU's data cache lines of [start, start + bytes) on AArch64 (the VideoCore reads and writes memory around the caches); nothing elsewhere
// (host tests).
inline void pi_cache_clean_invalidate(const void* start, uint64_t bytes) {
#if defined(__aarch64__)
  constexpr uint64_t kCacheLine = 64;
  const uint64_t begin = reinterpret_cast<uint64_t>(start) & ~(kCacheLine - 1);
  const uint64_t end = reinterpret_cast<uint64_t>(start) + bytes;
  for (uint64_t line = begin; line < end; line += kCacheLine) __asm__ volatile("dc civac, %0" ::"r"(line) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
#else
  (void)start;
  (void)bytes;
#endif
}

}  // namespace hardware
