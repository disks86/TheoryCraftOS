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
#include "Jit/KernelCodeMemory.hpp"

namespace jit {

bool KernelCodeMemory::set(const CodeRegion& region, bool writable, bool executable) {
  kernel::PageAttributes attributes;
  attributes.writable = writable;
  attributes.executable = executable;
  const uint64_t address = reinterpret_cast<uint64_t>(region.bytes.data());   // identity mapped: the address is the physical address
  {
    SpinFlagGuard guard(tables_lock_);
    if (!tables_.map(address, region.bytes.size(), attributes)) return false;
  }
  return !shootdown_ || shootdown_->invalidate_range(address, region.bytes.size());
}

bool KernelCodeMemory::allocate(uint64_t size, CodeRegion* out) {
  if (size == 0 || size > UINT64_MAX - (kPage - 1)) return false;
  const uint64_t pages = (size + kPage - 1) / kPage;
  uint8_t* memory = pages_.allocate_pages(pages);
  if (!memory) return false;
  CodeRegion region;
  region.bytes = ByteView(memory, pages * kPage);
  // Writable now, executable only after the code is in: the window in which the pages are both never exists.
  if (!set(region, true, false)) {
    // (map() may have changed part of the range: put it back to how it rests)
    if (set(region, true, pages_.pages_are_ordinary_memory())) pages_.release_pages(memory, pages);
    return false;
  }
  region.bytes.fill(0);
  *out = region;
  return true;
}

bool KernelCodeMemory::make_executable(const CodeRegion& region) {
  if (region.empty()) return false;
  // Data cache and instruction cache are made coherent while the pages are still writable, so no stale instruction can be fetched once they are executable.
  flush_instruction_cache(region.bytes);
  if (icache_sync_ && !icache_sync_->sync(region.bytes)) return false;
  if (!set(region, false, true)) {
    set(region, true, false);
    return false;
  }
  return true;
}

bool KernelCodeMemory::make_writable(const CodeRegion& region) { return !region.empty() && set(region, true, false); }

void KernelCodeMemory::release(const CodeRegion& region) {
  if (region.empty()) return;
  // Back to how the pages rest: ordinary memory for the allocator (writable, executable as firmware left it), writable and not executable for a code pool.
  // If that fails the pages are leaked rather than handed on in a mapping the next user does not expect.
  if (set(region, true, pages_.pages_are_ordinary_memory())) pages_.release_pages(region.bytes.data(), region.bytes.size() / kPage);
}

}  // namespace jit
