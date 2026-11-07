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

uint64_t* UserPageTables::new_table(uint32_t into) {
  uint64_t* table = static_cast<uint64_t*>(allocator_.allocate_aligned(kPageSize, kPageSize));
  if (!table) return nullptr;
  ArrayView<uint64_t>(table, kEntries).fill(0);
  if (!sets_[into].push_back(table)) {
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

uint64_t* UserPageTables::leaf(uint64_t root, uint64_t address, bool make, uint32_t into) {
  uint64_t table = root;
  const uint32_t shifts[4] = {39, 30, 21, 12};
  for (uint32_t level = 0; level < 3; ++level) {
    uint64_t& entry = table_at(table)[(address >> shifts[level]) & (kEntries - 1)];
    if (!(entry & kPresent)) {
      if (!make) return nullptr;
      uint64_t* next = new_table(into);
      if (!next) return nullptr;
      // Intermediate levels allow everything; the leaf decides (user access needs the user bit at every level). (The table is whole before a CPU can see it.)
      __atomic_store_n(&entry, address_of(next) | kPresent | kWritable | kUser, __ATOMIC_RELEASE);
    }
    table = entry & kAddressMask;
  }
  return &table_at(table)[(address >> 12) & (kEntries - 1)];
}

bool UserPageTables::map_page(uint64_t address, uint64_t physical, uint64_t flags) {
  uint64_t* entry = leaf(building_root_, address, true, 1 - active_);
  if (!entry) return false;
  *entry = (physical & kAddressMask) | flags;
  return true;
}

void UserPageTables::release_previous() {
  for (uint64_t* table : sets_[1 - active_].view()) allocator_.deallocate(table);
  sets_[1 - active_].clear();
}

uint64_t UserPageTables::leaf_flags(uint8_t protection) const {
  return kPresent | kUser | ((protection & kProtWrite) ? kWritable : 0) | ((nx_ && !(protection & kProtExecute)) ? kNoExecute : 0);
}

uint64_t UserPageTables::wanted(const AddressSpace& space, uint64_t address) const {
  AddressSpace::RegionInfo region;
  if (!space.region_at(space.region_index(address), &region) || region.base > address || region.protection == 0) return 0;      // (no access: no entry, any touch faults)
  const uint8_t* memory = space.page_memory(address);
  return memory ? (address_of(memory) & kAddressMask) | leaf_flags(region.protection) : 0;
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
  uint64_t* root = new_table(1 - active_);
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
    const uint64_t flags = leaf_flags(region.protection);
    if (region.storage) {      // shared: every page has its memory
      for (uint64_t offset = 0; offset < region.size; offset += kPageSize) {
        if (!map_page(region.base + offset, address_of(region.storage) + offset, flags)) return abandon();
      }
      continue;
    }
    bool ok = true;
    space.for_each_private_page(region.base, region.base + region.size, [&](uint64_t page, const uint8_t* memory) { ok = ok && map_page(page, address_of(memory), flags); });
    if (!ok) return abandon();
  }
  // Done: the new set replaces the old one.
  if (!keep_previous) release();
  active_ = 1 - active_;
  root_ = building_root_;
  building_root_ = 0;
  built_ = true;
  return true;
}

void UserPageTables::refresh(const AddressSpace& space, uint64_t table, uint32_t level, uint64_t base, uint64_t start, uint64_t end, bool* flush) {
  const uint32_t shift = 39 - 9 * level;
  const uint64_t span = uint64_t{1} << shift;
  const uint64_t first = start > base ? (start - base) >> shift : 0;
  ArrayView<uint64_t> entries = table_at(table);
  for (uint64_t i = first; i < kEntries; ++i) {
    const uint64_t at = base + i * span;
    if (at >= end) return;
    uint64_t& entry = entries[i];
    const uint64_t old = __atomic_load_n(&entry, __ATOMIC_RELAXED);
    if (!(old & kPresent) || !(old & kUser)) continue;      // (nothing there, or one of the kernel's own pages)
    if (level < 3) {
      refresh(space, old & kAddressMask, level + 1, at, start, end, flush);
      continue;
    }
    const uint64_t now = wanted(space, at);
    if (now == old) continue;
    // A CPU may have the old entry cached: it has to forget it unless the new one only allows more of the same page.
    const bool more_only = (now & kPresent) && (now & kAddressMask) == (old & kAddressMask) && ((now & kWritable) || !(old & kWritable)) && (!(now & kNoExecute) || (old & kNoExecute));
    if (!more_only && flush) *flush = true;
    __atomic_store_n(&entry, now, __ATOMIC_RELEASE);
  }
}

bool UserPageTables::update(const AddressSpace& space, uint64_t start, uint64_t end, bool* flush) {
  if (!built_) return false;
  start = AddressSpace::round_down(start);
  if (end > AddressSpace::kUserTop) end = AddressSpace::kUserTop;
  if (start >= end) return true;
  // What is there: each entry becomes what it should be (or goes).
  refresh(space, root_, 0, 0, start, end, flush);
  // What is missing: the pages of the range that may be used and have memory.
  bool ok = true;
  auto install = [&](uint64_t page, uint64_t physical, uint64_t flags) {
    uint64_t* entry = leaf(root_, page, true, active_);
    if (!entry) { ok = false; return; }
    if (!(__atomic_load_n(entry, __ATOMIC_RELAXED) & kPresent)) __atomic_store_n(entry, (physical & kAddressMask) | flags, __ATOMIC_RELEASE);
  };
  AddressSpace::RegionInfo region;
  for (uint64_t i = space.region_index(start); ok && space.region_at(i, &region) && region.base < end; ++i) {
    if (region.protection == 0) continue;
    const uint64_t from = region.base > start ? region.base : start, to = region.base + region.size < end ? region.base + region.size : end;
    const uint64_t flags = leaf_flags(region.protection);
    if (region.storage) {
      for (uint64_t page = from; ok && page < to; page += kPageSize) install(page, address_of(region.storage) + (page - region.base), flags);
    } else {
      space.for_each_private_page(from, to, [&](uint64_t page, const uint8_t* memory) { if (ok) install(page, address_of(memory), flags); });
    }
  }
  return ok;
}

uint64_t UserPageTables::entry_of(uint64_t address) {
  const uint64_t* entry = built_ ? leaf(root_, address, false, active_) : nullptr;
  return entry ? __atomic_load_n(entry, __ATOMIC_RELAXED) : 0;
}

bool UserPageTables::sync(AddressSpace& space, bool keep_previous, bool* flush) {
  uint64_t start = 0, end = 0;
  if (!built_) {
    while (space.take_change(&start, &end)) {}      // (all of it goes in)
    return rebuild(space, keep_previous);
  }
  while (space.take_change(&start, &end)) {
    if (!update(space, start, end, flush)) return false;
  }
  return true;
}

}  // namespace process
