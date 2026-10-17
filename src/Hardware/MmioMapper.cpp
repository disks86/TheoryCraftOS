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

#include "Hardware/MmioMapper.hpp"

namespace hardware {

bool MmioMapper::add(uint64_t physical, uint64_t length, MmioKind kind, bool kept, MmioRegion* region) {
  if (length == 0 || physical + length < physical) return false;
  const uint64_t start = physical & ~0xFFFull, end = (physical + length + 0xFFF) & ~0xFFFull;
  if (ram_.overlaps_memory(start, end - start)) return false;
  // the same window again, or one that clashes
  for (uint32_t i = 0; i < count_; ++i) {
    Mapping& m = mappings_[i];
    if (m.end <= start || end <= m.start) continue;
    if (m.start == start && m.end == end && m.kind == kind && m.kept == kept) {
      ++m.uses;
      *region = MmioRegion(physical, length);
      return true;
    }
    if (m.kind != kind && !m.kept && !kept) return false;   // the same pages cannot be two memory types
  }
  if (count_ == kMaxMappings) return false;
  kernel::PageAttributes a;
  a.writable = true;
  a.executable = false;
  a.cache = kind == MmioKind::FrameBuffer && tables_.enable_write_combining() ? kernel::CacheMode::WriteCombining : kernel::CacheMode::Uncached;
  if (!(kept ? tables_.map_missing(start, end - start, a) : tables_.map(start, end - start, a))) return false;
  mappings_[count_++] = Mapping{start, end, kind, kept, 1};
  *region = MmioRegion(physical, length);
  return true;
}

bool MmioMapper::map(uint64_t physical, uint64_t length, MmioKind kind, MmioRegion* region) { return add(physical, length, kind, false, region); }
bool MmioMapper::ensure_mapped(uint64_t physical, uint64_t length, MmioRegion* region) { return add(physical, length, MmioKind::Registers, true, region); }

void MmioMapper::release(const MmioRegion& region) {
  const uint64_t start = region.address() & ~0xFFFull, end = (region.address() + region.length() + 0xFFF) & ~0xFFFull;
  for (uint32_t i = 0; i < count_; ++i)
    if (mappings_[i].start == start && mappings_[i].end == end) {
      if (--mappings_[i].uses == 0) mappings_[i] = mappings_[--count_];
      return;
    }
}

}  // namespace hardware
