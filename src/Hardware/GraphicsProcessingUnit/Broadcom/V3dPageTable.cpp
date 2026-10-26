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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dPageTable.hpp"

namespace hardware {

bool V3dPageTable::map(uint64_t iova, uint64_t bus_address, uint64_t pages, bool writeable) {
  if ((iova | bus_address) & (kPageSize - 1)) return false;
  const uint64_t first = iova >> v3d::kMmuPageShift;
  const uint64_t first_bus_page = bus_address >> v3d::kMmuPageShift;
  if (first > entries_.size() || pages > entries_.size() - first) return false;
  if (first_bus_page > v3d::kPtePageMask || pages > v3d::kPtePageMask - first_bus_page + 1) return false;
  for (uint64_t i = 0; i < pages; ++i) {
    entries_[first + i] = static_cast<uint32_t>(first_bus_page + i) | v3d::kPteValid | (writeable ? v3d::kPteWriteable : 0);
  }
  return true;
}

bool V3dPageTable::unmap(uint64_t iova, uint64_t pages) {
  if (iova & (kPageSize - 1)) return false;
  const uint64_t first = iova >> v3d::kMmuPageShift;
  if (first > entries_.size() || pages > entries_.size() - first) return false;
  for (uint64_t i = 0; i < pages; ++i) entries_[first + i] = 0;
  return true;
}

bool V3dPageTable::is_mapped(uint64_t iova, bool* writeable) const {
  uint32_t entry = 0;
  if (!entries_.try_get(iova >> v3d::kMmuPageShift, &entry) || !(entry & v3d::kPteValid)) return false;
  if (writeable) *writeable = (entry & v3d::kPteWriteable) != 0;
  return true;
}

void V3dPageTable::clear() {
  for (uint32_t& entry : entries_) entry = 0;
}

}  // namespace hardware
