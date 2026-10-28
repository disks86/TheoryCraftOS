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
#include "Process/AddressSpace.hpp"

namespace process {

bool AddressSpace::Region::allocate(uint64_t bytes) {
  release();
  void* memory = allocator_.allocate_aligned(bytes, kPageSize);
  if (!memory) return false;
  storage = static_cast<uint8_t*>(memory);
  capacity = bytes;
  ByteView(storage, bytes).fill(0);
  return true;
}

void AddressSpace::Region::release() {
  if (shared) {
    if (!retired_.deferred || !retired_.storages.push_back(RetiredPiece{nullptr, shared})) shared->release();      // (the mapping is let go of when no CPU can see it any more)
    shared = nullptr;
  } else if (storage) {
    if (!retired_.deferred || !retired_.storages.push_back(RetiredPiece{storage, nullptr})) allocator_.deallocate(storage);      // (kept for reclaim(), unless there is no room to remember it)
  }
  storage = nullptr;
  capacity = 0;
}

void AddressSpace::reclaim() {
  for (const RetiredPiece& piece : retired_.storages.view()) {
    if (piece.shared) piece.shared->release();
    else allocator_.deallocate(piece.storage);
  }
  retired_.storages.clear();
}

bool AddressSpace::overlaps_reserved(uint64_t start, uint64_t end, uint64_t* reserved_end) const {
  for (const AddressRange& r : reserved_) {
    if (r.size == 0 || start >= r.base + r.size || end <= r.base) continue;
    if (reserved_end) *reserved_end = r.base + r.size;
    return true;
  }
  return false;
}

bool AddressSpace::region_at(uint64_t index, RegionInfo* out) const {
  if (index >= regions_.count()) return false;
  const Region& r = *regions_[index];
  *out = RegionInfo{r.base, r.size, r.protection, r.storage, r.allocation, r.attributes};
  return true;
}

uint64_t AddressSpace::first_ending_after(uint64_t address) const {
  uint64_t low = 0, high = regions_.count();
  while (low < high) {
    const uint64_t mid = low + (high - low) / 2;
    const Region& r = *regions_[mid];
    if (r.base + r.size > address) high = mid; else low = mid + 1;      // base + size <= kUserTop, so this cannot wrap
  }
  return low;
}

bool AddressSpace::map(uint64_t address, uint64_t size, uint8_t protection) {
  uint64_t rounded = 0;
  if (size == 0 || address % kPageSize != 0 || !round_up(size, &rounded)) return false;
  if (address >= kUserTop || rounded > kUserTop - address) return false;
  if (rounded > max_bytes_ || mapped_bytes_ > max_bytes_ - rounded) return false;
  const uint64_t index = first_ending_after(address);
  if (index < regions_.count() && regions_[index]->base < address + rounded) return false;      // overlaps the region after
  if (overlaps_reserved(address, address + rounded)) return false;
  UniquePointer<Region> region = UniquePointer<Region>::make(allocator_, allocator_, retired_);
  if (!region) return false;
  region->base = address;
  region->size = rounded;
  region->protection = protection;
  region->allocation = next_allocation_;
  if (!region->allocate(rounded)) return false;
  if (!regions_.insert(index, static_cast<UniquePointer<Region>&&>(region))) return false;
  mapped_bytes_ += rounded;
  ++next_allocation_;
  ++generation_;
  return true;
}

bool AddressSpace::map_shared(uint64_t address, uint64_t size, uint8_t protection, uint8_t* storage, SharedMemory* owner) {
  uint64_t rounded = 0;
  if (!storage || !owner || size == 0 || address % kPageSize != 0 || !round_up(size, &rounded)) return false;
  if (address >= kUserTop || rounded > kUserTop - address) return false;
  if (rounded > max_bytes_ || mapped_bytes_ > max_bytes_ - rounded) return false;
  const uint64_t index = first_ending_after(address);
  if (index < regions_.count() && regions_[index]->base < address + rounded) return false;
  if (overlaps_reserved(address, address + rounded)) return false;
  UniquePointer<Region> region = UniquePointer<Region>::make(allocator_, allocator_, retired_);
  if (!region) return false;
  region->base = address;
  region->size = rounded;
  region->protection = protection;
  region->allocation = next_allocation_;
  if (!regions_.insert(index, static_cast<UniquePointer<Region>&&>(region))) return false;      // (the region has no memory yet: nothing to let go of)
  regions_[index]->storage = storage;
  regions_[index]->capacity = rounded;
  regions_[index]->shared = owner;
  owner->retain();
  mapped_bytes_ += rounded;
  ++next_allocation_;
  ++generation_;
  return true;
}

SharedMemory* AddressSpace::shared_at(uint64_t address) const {
  const uint64_t i = first_ending_after(address);
  return i < regions_.count() && regions_[i]->base <= address ? regions_[i]->shared : nullptr;
}

bool AddressSpace::is_shared(uint64_t address) const {
  const uint64_t i = first_ending_after(address);
  return i < regions_.count() && regions_[i]->base <= address && regions_[i]->shared != nullptr;
}

bool AddressSpace::prepare_range(uint64_t address, uint64_t size, uint64_t* end) const {
  uint64_t rounded = 0;
  if (size == 0 || address % kPageSize != 0 || !round_up(size, &rounded) || address >= kUserTop || rounded > kUserTop - address) return false;
  *end = address + rounded;
  return true;
}

bool AddressSpace::split_at(uint64_t address) {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base >= address) return true;      // already a boundary (or unmapped)
  Region& r = *regions_[i];
  const uint64_t offset = address - r.base;
  UniquePointer<Region> tail = UniquePointer<Region>::make(allocator_, allocator_, retired_);
  if (!tail) return false;
  tail->base = address;
  tail->size = r.size - offset;
  tail->protection = r.protection;
  tail->allocation = r.allocation;
  tail->attributes = r.attributes;
  if (r.shared) {      // the tail maps the same memory further on
    tail->storage = r.storage + offset;
    tail->capacity = tail->size;
    tail->shared = r.shared;
    r.shared->retain();
  } else if (!tail->allocate(tail->size) || !ByteView(tail->storage, tail->size).copy_from(ConstByteView(r.storage + offset, tail->size))) {
    return false;
  }
  if (!regions_.insert(i + 1, static_cast<UniquePointer<Region>&&>(tail))) {
    tail.reset();      // (releases the mapping the tail took)
    return false;
  }
  r.size = offset;      // its memory stays (the tail pages are simply unused until it is released)
  ++generation_;
  return true;
}

bool AddressSpace::unmap(uint64_t address, uint64_t size) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  const uint64_t first = first_ending_after(address);
  while (first < regions_.count() && regions_[first]->base < end) {
    mapped_bytes_ -= regions_[first]->size;
    regions_.remove_at(first);
  }
  ++generation_;
  return true;
}

bool AddressSpace::protect(uint64_t address, uint64_t size, uint8_t protection) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  for (uint64_t i = first_ending_after(address); i < regions_.count() && regions_[i]->base < end; ++i) regions_[i]->protection = protection;
  ++generation_;
  return true;
}

bool AddressSpace::set_attributes(uint64_t address, uint64_t size, const PageAttributes& attributes) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  for (uint64_t i = first_ending_after(address); i < regions_.count() && regions_[i]->base < end; ++i) regions_[i]->attributes = attributes;
  ++generation_;
  return true;
}

bool AddressSpace::set_page_protect(uint64_t address, uint64_t size, uint32_t protect, bool reserved) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  for (uint64_t i = first_ending_after(address); i < regions_.count() && regions_[i]->base < end; ++i) {
    regions_[i]->attributes.protect = protect;
    regions_[i]->attributes.reserved = reserved;
  }
  ++generation_;
  return true;
}

bool AddressSpace::break_copy_on_write(uint64_t address, uint64_t size) {
  if (size == 0 || address >= kUserTop || size > kUserTop - address) return false;
  bool any = false;
  for (uint64_t page = round_down(address); page < address + size; page += kPageSize) {
    uint64_t i = first_ending_after(page);
    if (i >= regions_.count() || regions_[i]->base > page) continue;
    const uint32_t protect = regions_[i]->attributes.protect;
    if (protect != 0x08 && protect != 0x80) continue;
    if (!split_at(page) || !split_at(page + kPageSize)) return false;
    i = first_ending_after(page);
    Region& r = *regions_[i];
    if (r.shared) {      // a copy of this page for this address space alone
      uint8_t* copy = static_cast<uint8_t*>(allocator_.allocate_aligned(kPageSize, kPageSize));
      if (!copy) return false;
      if (!ByteView(copy, kPageSize).copy_from(ConstByteView(r.storage, kPageSize))) { allocator_.deallocate(copy); return false; }
      r.release();      // (the mapping is let go of; its memory is not ours)
      r.storage = copy;
      r.capacity = kPageSize;
    }
    r.protection |= kProtWrite;
    r.attributes.protect = protect == 0x08 ? 0x04 : 0x40;
    r.attributes.copied = true;
    any = true;
  }
  if (any) ++generation_;
  return any;
}

bool AddressSpace::join_allocation(uint64_t address, uint64_t size, uint64_t with) {
  const uint64_t target = first_ending_after(with);
  if (target >= regions_.count() || regions_[target]->base > with) return false;
  const uint64_t id = regions_[target]->allocation;
  uint64_t end = 0;
  if (!prepare_range(address, size, &end)) return false;
  for (uint64_t i = first_ending_after(address); i < regions_.count() && regions_[i]->base < end; ++i) regions_[i]->allocation = id;
  return true;
}

bool AddressSpace::unmap_allocation(uint64_t address) {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address) return false;
  const uint64_t id = regions_[i]->allocation;
  for (uint64_t k = regions_.count(); k > 0; --k) {
    if (regions_[k - 1]->allocation != id) continue;
    mapped_bytes_ -= regions_[k - 1]->size;
    regions_.remove_at(k - 1);
  }
  ++generation_;
  return true;
}

bool AddressSpace::allocation_base(uint64_t address, uint64_t* base) const {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address) return false;
  const uint64_t id = regions_[i]->allocation;
  for (uint64_t k = 0; k <= i; ++k) if (regions_[k]->allocation == id) { *base = regions_[k]->base; return true; }
  return false;
}

bool AddressSpace::attributes_at(uint64_t address, PageAttributes* out) const {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address) return false;
  *out = regions_[i]->attributes;
  return true;
}

bool AddressSpace::resize(uint64_t base, uint64_t size) {
  const uint64_t i = first_ending_after(base);
  if (base % kPageSize != 0 || i >= regions_.count() || regions_[i]->base != base) return false;
  Region& r = *regions_[i];
  if (r.shared) return false;      // a view of a section has the size of the view
  uint64_t rounded = 0;
  if (!round_up(size, &rounded)) return false;
  if (rounded == 0) {
    mapped_bytes_ -= r.size;
    regions_.remove_at(i);
    ++generation_;
    return true;
  }
  if (rounded > r.size) {
    const uint64_t extra = rounded - r.size;
    if (base >= kUserTop || rounded > kUserTop - base || extra > max_bytes_ || mapped_bytes_ > max_bytes_ - extra) return false;
    if (i + 1 < regions_.count() && regions_[i + 1]->base < base + rounded) return false;
    if (overlaps_reserved(base + r.size, base + rounded)) return false;
    if (rounded > r.capacity) {
      // Move to bigger page-aligned memory.
      Region bigger(allocator_, retired_);
      if (!bigger.allocate(rounded)) return false;
      if (r.size) ByteView(bigger.storage, rounded).first(r.size).copy_from(ConstByteView(r.storage, r.size));
      uint8_t* old = r.storage;
      r.storage = bigger.storage;
      r.capacity = bigger.capacity;
      bigger.storage = old;      // `bigger` now owns (and frees) the old memory
      bigger.capacity = 0;
    } else {
      ByteView(r.storage + r.size, extra).fill(0);      // pages that were cut off earlier come back zeroed
    }
    mapped_bytes_ += extra;
  } else {
    mapped_bytes_ -= r.size - rounded;
  }
  r.size = rounded;
  ++generation_;
  return true;
}

bool AddressSpace::region_extent(uint64_t address, uint64_t* base, uint64_t* size) const {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address) return false;
  *base = regions_[i]->base;
  *size = regions_[i]->size;
  return true;
}

bool AddressSpace::find_free(uint64_t size, uint64_t hint, uint64_t* out) const {
  uint64_t rounded = 0;
  if (size == 0 || !round_up(size, &rounded) || rounded > kUserTop) return false;
  uint64_t candidate = round_down(hint);
  if (candidate < kPageSize) candidate = kPageSize;      // never hand out page zero
  for (;;) {
    if (candidate > kUserTop - rounded) return false;
    uint64_t after = 0;
    const uint64_t i = first_ending_after(candidate);
    if (i < regions_.count() && regions_[i]->base < candidate + rounded) {
      candidate = regions_[i]->base + regions_[i]->size;      // occupied: look after that region
    } else if (overlaps_reserved(candidate, candidate + rounded, &after)) {
      if (!round_up(after, &candidate)) return false;
    } else {
      *out = candidate;
      return true;
    }
  }
}

bool AddressSpace::check(uint64_t address, uint64_t size, uint8_t protection) const {
  if (size == 0) return true;
  if (address >= kUserTop || size > kUserTop - address) return false;
  uint64_t cursor = address;
  const uint64_t end = address + size;
  for (uint64_t i = first_ending_after(address); cursor < end; ++i) {
    if (i >= regions_.count()) return false;
    const Region& r = *regions_[i];
    if (r.base > cursor || (r.protection & protection) != protection) return false;
    cursor = r.base + r.size;
  }
  return true;
}

bool AddressSpace::allocation_extent(uint64_t address, uint64_t* base, uint64_t* size) const {
  uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address) return false;
  const uint64_t id = regions_[i]->allocation;
  uint64_t first = i, last = i;
  while (first > 0 && regions_[first - 1]->allocation == id && regions_[first - 1]->base + regions_[first - 1]->size == regions_[first]->base) --first;
  while (last + 1 < regions_.count() && regions_[last + 1]->allocation == id && regions_[last]->base + regions_[last]->size == regions_[last + 1]->base) ++last;
  *base = regions_[first]->base;
  *size = regions_[last]->base + regions_[last]->size - *base;
  return true;
}

bool AddressSpace::protection_at(uint64_t address, uint8_t* out) const {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address) return false;
  *out = regions_[i]->protection;
  return true;
}

bool AddressSpace::copy_out(uint64_t address, ByteView out) const {
  uint64_t done = 0;
  for (uint64_t i = first_ending_after(address); done < out.size(); ++i) {
    if (i >= regions_.count()) return false;
    const Region& r = *regions_[i];
    const uint64_t at = address + done;
    const uint64_t offset = at - r.base;
    const uint64_t n = out.size() - done < r.size - offset ? out.size() - done : r.size - offset;
    if (!out.subview(done, n).copy_from(ConstByteView(r.storage + offset, n))) return false;
    done += n;
  }
  return true;
}

bool AddressSpace::copy_in(uint64_t address, ConstByteView data) {
  uint64_t done = 0;
  for (uint64_t i = first_ending_after(address); done < data.size(); ++i) {
    if (i >= regions_.count()) return false;
    Region& r = *regions_[i];
    const uint64_t at = address + done;
    const uint64_t offset = at - r.base;
    const uint64_t n = data.size() - done < r.size - offset ? data.size() - done : r.size - offset;
    if (!r.bytes().subview(offset, n).copy_from(data.subview(done, n))) return false;
    done += n;
  }
  return true;
}

}  // namespace process
