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
#include "Process/UserPageTables.hpp"

namespace process {

namespace {
constexpr uint64_t kPageSize = AddressSpace::kPageSize;
constexpr uint64_t kEntries = 512;
constexpr uint64_t kAddressMask = 0x000FFFFFFFFFF000ull;

// The kernel runs with memory mapped 1:1, so a table's physical address is the address the kernel reads it at (and the other way round). These two are the
// only casts: the pointers come from the page allocator, which hands out 4 KiB-aligned memory that stays valid until deallocate().
ArrayView<uint64_t> table_at(uint64_t address) { return ArrayView<uint64_t>(reinterpret_cast<uint64_t*>(address), kEntries); }
uint64_t address_of(const void* memory) { return reinterpret_cast<uint64_t>(memory); }
}  // namespace

bool UserPageTables::add_identity(const Identity& range) {
  if (range.size == 0 || range.address % kPageSize != 0 || range.size % kPageSize != 0 || range.address >= (1ull << 48) || range.size > (1ull << 48) - range.address) return false;
  return identity_.push_back(range);
}

uint64_t* UserPageTables::new_table() {
  uint64_t* table = static_cast<uint64_t*>(allocator_.allocate_aligned(kPageSize, kPageSize));
  if (!table) return nullptr;
  ArrayView<uint64_t>(table, kEntries).fill(0);
  if (!sets_[1 - active_].push_back(table)) {
    allocator_.deallocate(table);
    return nullptr;
  }
  return table;
}

void UserPageTables::release() {
  for (uint64_t* table : sets_[active_].view()) allocator_.deallocate(table);
  sets_[active_].clear();
  root_ = 0;
  built_ = false;
}

bool UserPageTables::map_page(uint64_t address, uint64_t physical, uint64_t leaf_flags) {
  uint64_t table = building_root_;
  const uint32_t shifts[4] = {39, 30, 21, 12};
  for (uint32_t level = 0; level < 4; ++level) {
    uint64_t& entry = table_at(table)[(address >> shifts[level]) & (kEntries - 1)];
    if (level == 3) {
      entry = (physical & kAddressMask) | leaf_flags;
      return true;
    }
    if (!(entry & kPresent)) {
      uint64_t* next = new_table();
      if (!next) return false;
      // Intermediate levels allow everything; the leaf decides (user access needs the user bit at every level).
      entry = address_of(next) | kPresent | kWritable | kUser;
    }
    table = entry & kAddressMask;
  }
  return false;
}

void UserPageTables::release_previous() {
  for (uint64_t* table : sets_[1 - active_].view()) allocator_.deallocate(table);
  sets_[1 - active_].clear();
}

bool UserPageTables::rebuild(const AddressSpace& space, bool keep_previous) {
  release_previous();      // (what an earlier rebuild kept)
  // Everything goes into the spare set; the current one (what the CPU may be running on) is not touched until the new set is whole.
  auto abandon = [&]() {
    for (uint64_t* table : sets_[1 - active_].view()) allocator_.deallocate(table);
    sets_[1 - active_].clear();
    building_root_ = 0;
    return false;
  };
  uint64_t* root = new_table();
  if (!root) return abandon();
  building_root_ = address_of(root);
  for (const Identity& range : identity_.view()) {
    const uint64_t flags = kPresent | (range.writable ? kWritable : 0) | ((nx_ && !range.executable) ? kNoExecute : 0);      // no kUser: kernel only
    for (uint64_t offset = 0; offset < range.size; offset += kPageSize) {
      if (!map_page(range.address + offset, range.address + offset, flags)) return abandon();
    }
  }
  AddressSpace::RegionInfo region;
  for (uint64_t i = 0; space.region_at(i, &region); ++i) {
    // A region without access still gets pages in the address space, but none here: any touch faults, as it should.
    if (region.protection == 0) continue;
    const uint64_t flags = kPresent | kUser | ((region.protection & kProtWrite) ? kWritable : 0) | ((nx_ && !(region.protection & kProtExecute)) ? kNoExecute : 0);
    for (uint64_t offset = 0; offset < region.size; offset += kPageSize) {
      if (!map_page(region.base + offset, address_of(region.storage) + offset, flags)) return abandon();
    }
  }
  // Done: the new set replaces the old one.
  if (!keep_previous) release();
  active_ = 1 - active_;
  root_ = building_root_;
  building_root_ = 0;
  built_ = true;
  generation_ = space.generation();
  return true;
}

}  // namespace process
