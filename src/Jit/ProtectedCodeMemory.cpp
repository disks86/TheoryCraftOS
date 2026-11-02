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
#include "Jit/ProtectedCodeMemory.hpp"

namespace jit {

bool ProtectedCodeMemory::allocate(uint64_t size, CodeRegion* out) {
  if (size == 0 || size > UINT64_MAX - (kCodePage - 1)) return false;
  const uint64_t page_count = (size + kCodePage - 1) / kCodePage;
  uint8_t* memory = pages_.allocate_pages(page_count);
  if (!memory) return false;
  CodeRegion region;
  region.bytes = ByteView(memory, page_count * kCodePage);
  if (!protect(region, PageAccess::ReadWrite)) {
    // (if the pages cannot be put back to how they rest, they are leaked rather than reused as they are)
    if (protect(region, PageAccess::ReadWrite)) pages_.release_pages(memory, page_count);
    return false;
  }
  region.bytes.fill(0);
  *out = region;
  return true;
}

bool ProtectedCodeMemory::make_executable(const CodeRegion& region) {
  if (region.empty()) return false;
  // Both caches are made coherent while the pages are still writable, so no stale instruction can be fetched once they are executable.
  flush_instruction_cache(region.bytes);
  if (sync_ && !sync_->sync(region.bytes)) return false;
  if (!protect(region, PageAccess::ReadExecute)) {
    protect(region, PageAccess::ReadWrite);
    return false;
  }
  return true;
}

bool ProtectedCodeMemory::make_writable(const CodeRegion& region) { return !region.empty() && protect(region, PageAccess::ReadWrite); }

void ProtectedCodeMemory::release(const CodeRegion& region) {
  if (region.empty()) return;
  if (protect(region, PageAccess::ReadWrite)) pages_.release_pages(region.bytes.data(), region.bytes.size() / kCodePage);
}

}  // namespace jit
