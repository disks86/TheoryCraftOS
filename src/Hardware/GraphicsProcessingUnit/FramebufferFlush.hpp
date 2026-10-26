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

// A display engine reads memory without looking into the CPU's caches, so what the CPU drew has to be written out of them before it shows. x86 only (clflush).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// Flushes the cache lines of the rectangle (x, y, width, height) of a framebuffer whose rows are `pitch_bytes` apart. Rows past the end of `memory` are left out.
inline void flush_framebuffer_rect(ByteView memory, uint32_t pitch_bytes, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || pitch_bytes == 0) return;
  constexpr uint64_t kLine = 64;
  for (uint32_t row = y; row < y + height; ++row) {
    const uint64_t start = uint64_t{row} * pitch_bytes + uint64_t{x} * 4;
    const uint64_t end = start + uint64_t{width} * 4;
    if (end > memory.size()) break;
    for (uint64_t at = start & ~(kLine - 1); at < end; at += kLine) __builtin_ia32_clflush(memory.data() + at);
  }
  __builtin_ia32_sfence();
}

}  // namespace hardware
