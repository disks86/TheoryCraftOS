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

AddressSpace::~AddressSpace() {
  retired_.deferred = false;
  reclaim();
  pages_.remove(0, PageMap::kTop, [&](uint8_t* page) { allocator_.deallocate(page); });
}

void AddressSpace::Region::release() {
  if (shared) {
    if (!retired_.deferred || !retired_.storages.push_back(RetiredPiece{nullptr, shared})) shared->release();      // (the mapping is let go of when no CPU can see it any more)
    shared = nullptr;
  }
  storage = nullptr;
}

void AddressSpace::retire(uint8_t* page) {
  if (!retired_.deferred || !retired_.storages.push_back(RetiredPiece{page, nullptr})) allocator_.deallocate(page);      // (kept for reclaim(), unless there is no room to remember it)
}

void AddressSpace::reclaim() {
  for (const RetiredPiece& piece : retired_.storages.view()) {
    if (piece.shared) piece.shared->release();
    else allocator_.deallocate(piece.storage);
  }
  retired_.storages.clear();
}

void AddressSpace::drop_pages(uint64_t start, uint64_t end) {
  pages_.remove(start, end, [&](uint8_t* page) { retire(page); });
}

void AddressSpace::note_change(uint64_t start, uint64_t end) {
  if (start >= end) return;
  for (uint32_t i = 0; i < change_count_; ++i) {
    Change& c = changes_[i];
    if (start <= c.end && end >= c.start) {      // touches one already noted: grows it
      if (start < c.start) c.start = start;
      if (end > c.end) c.end = end;
      return;
    }
  }
  if (change_count_ == kMaxChanges) {      // many scattered changes: one range that covers them all
    for (uint32_t i = 1; i < change_count_; ++i) {
      if (changes_[i].start < changes_[0].start) changes_[0].start = changes_[i].start;
      if (changes_[i].end > changes_[0].end) changes_[0].end = changes_[i].end;
    }
    if (start < changes_[0].start) changes_[0].start = start;
    if (end > changes_[0].end) changes_[0].end = end;
    change_count_ = 1;
    return;
  }
  changes_[change_count_++] = Change{start, end};
}

bool AddressSpace::take_change(uint64_t* start, uint64_t* end) {
  if (change_count_ == 0) return false;
  --change_count_;
  *start = changes_[change_count_].start;
  *end = changes_[change_count_].end;
  return true;
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

void AddressSpace::remove_region(uint64_t index) {
  Region& r = *regions_[index];
  mapped_bytes_ -= r.size;
  if (r.shared) shared_bytes_ -= r.size;
  else drop_pages(r.base, r.base + r.size);
  note_change(r.base, r.base + r.size);
  regions_.remove_at(index);
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
  if (!regions_.insert(index, static_cast<UniquePointer<Region>&&>(region))) return false;
  mapped_bytes_ += rounded;
  ++next_allocation_;
  ++generation_;
  note_change(address, address + rounded);
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
  regions_[index]->shared = owner;
  owner->retain();
  shared_bytes_ += rounded;
  mapped_bytes_ += rounded;
  ++next_allocation_;
  ++generation_;
  note_change(address, address + rounded);
  return true;
}

SharedMemory* AddressSpace::shared_at(uint64_t address) const {
  const uint64_t i = first_ending_after(address);
  return i < regions_.count() && regions_[i]->base <= address ? regions_[i]->shared : nullptr;
}

uint32_t AddressSpace::views_of(const SharedMemory* owner, ViewPart* out, uint32_t capacity) const {
  uint32_t found = 0;
  for (uint64_t i = 0; i < regions_.count(); ++i) {
    const Region& r = *regions_[i];
    if (r.shared != owner || owner == nullptr) continue;
    if (found < capacity) { out[found].base = r.base; out[found].size = r.size; out[found].storage = r.storage; }
    ++found;
  }
  return found;
}

const uint8_t* AddressSpace::storage_at(uint64_t address) const {
  const uint64_t i = first_ending_after(address);
  if (i >= regions_.count() || regions_[i]->base > address || !regions_[i]->shared) return nullptr;
  return regions_[i]->storage + (address - regions_[i]->base);
}

bool AddressSpace::is_shared(uint64_t address) const {
  const uint64_t i = first_ending_after(address);
  return i < regions_.count() && regions_[i]->base <= address && regions_[i]->shared != nullptr;
}

namespace {
// The futex word inside a page: `page` is page-aligned memory of this address space and `offset` a multiple of 4 below kPageSize, so the word is aligned
// and lies wholly inside memory that lives as long as the page (the caller holds the layout steady for the call).
uint32_t* word_in_page(uint8_t* page, uint64_t offset) { return reinterpret_cast<uint32_t*>(page + offset); }
}  // namespace

bool AddressSpace::compare_exchange32(uint64_t address, uint32_t expected, uint32_t desired, uint32_t* observed) {
  if (address & 3) return false;
  if (!(check(address, 4, kProtWrite) || (break_copy_on_write(address, 4) && check(address, 4, kProtWrite)))) return false;
  uint8_t* page = materialize(address);
  if (!page) return false;
  uint32_t* word = word_in_page(page, address & (kPageSize - 1));
  __atomic_compare_exchange_n(word, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  *observed = expected;      // (the builtin leaves the value it found there)
  return true;
}

const uint8_t* AddressSpace::page_memory(uint64_t page) const {
  const uint64_t i = first_ending_after(page);
  if (i >= regions_.count() || regions_[i]->base > page) return nullptr;
  const Region& r = *regions_[i];
  return r.shared ? r.storage + (round_down(page) - r.base) : pages_.get(round_down(page));
}

uint8_t* AddressSpace::materialize(uint64_t page) {
  page = round_down(page);
  const uint64_t i = first_ending_after(page);
  if (i >= regions_.count() || regions_[i]->base > page) return nullptr;
  const Region& r = *regions_[i];
  if (r.shared) return r.storage + (page - r.base);
  if (uint8_t* have = pages_.get(page)) return have;
  uint8_t* memory = static_cast<uint8_t*>(allocator_.allocate_aligned(kPageSize, kPageSize));
  if (!memory) return nullptr;
  ByteView(memory, kPageSize).fill(0);
  if (!pages_.set(page, memory)) {
    allocator_.deallocate(memory);
    return nullptr;
  }
  return memory;
}

bool AddressSpace::discard(uint64_t address, uint64_t size) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!pages_.any(address, end)) return true;
  drop_pages(address, end);
  note_change(address, end);
  ++generation_;
  return true;
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
    tail->shared = r.shared;
    r.shared->retain();
  }
  if (!regions_.insert(i + 1, static_cast<UniquePointer<Region>&&>(tail))) {
    tail.reset();      // (releases the mapping the tail took)
    return false;
  }
  regions_[i]->size = offset;      // (the private pages stay where they are: they belong to addresses, not regions)
  ++generation_;
  return true;
}

bool AddressSpace::same_kind(const Region& a, const Region& b) const {
  return a.base + a.size == b.base && !a.shared && !b.shared && a.protection == b.protection && a.allocation == b.allocation && a.attributes == b.attributes;
}

void AddressSpace::coalesce(uint64_t first, uint64_t last) {
  if (regions_.count() < 2) return;
  if (first > 0) --first;
  if (last + 1 >= regions_.count()) last = regions_.count() - 1;
  for (uint64_t i = first; i < last && i + 1 < regions_.count();) {
    Region& a = *regions_[i];
    const Region& b = *regions_[i + 1];
    if (!same_kind(a, b)) { ++i; continue; }
    a.size += b.size;
    regions_.remove_at(i + 1);
    --last;
  }
}

bool AddressSpace::unmap(uint64_t address, uint64_t size) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  const uint64_t first = first_ending_after(address);
  while (first < regions_.count() && regions_[first]->base < end) remove_region(first);
  ++generation_;
  return true;
}

bool AddressSpace::protect(uint64_t address, uint64_t size, uint8_t protection) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  const uint64_t first = first_ending_after(address);
  uint64_t last = first;
  bool changed = false;
  for (uint64_t i = first; i < regions_.count() && regions_[i]->base < end; ++i) {
    changed |= regions_[i]->protection != protection;
    regions_[i]->protection = protection;
    last = i;
  }
  if (changed) {
    note_change(address, end);
    ++generation_;
  }
  coalesce(first, last);
  return true;
}

bool AddressSpace::set_attributes(uint64_t address, uint64_t size, const PageAttributes& attributes) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  const uint64_t first = first_ending_after(address);
  uint64_t last = first;
  for (uint64_t i = first; i < regions_.count() && regions_[i]->base < end; ++i) {
    regions_[i]->attributes = attributes;
    last = i;
  }
  coalesce(first, last);
  ++generation_;
  return true;
}

bool AddressSpace::set_page_protect(uint64_t address, uint64_t size, uint32_t protect, bool reserved) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  const uint64_t first = first_ending_after(address);
  uint64_t last = first;
  for (uint64_t i = first; i < regions_.count() && regions_[i]->base < end; ++i) {
    regions_[i]->attributes.protect = protect;
    regions_[i]->attributes.reserved = reserved;
    last = i;
  }
  coalesce(first, last);
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
    if (!privatize(page)) return false;      // a shared page gets a copy for this address space alone (the mapping is let go of; its memory is not ours)
    i = first_ending_after(page);
    Region& own = *regions_[i];
    own.protection |= kProtWrite;
    own.attributes.protect = protect == 0x08 ? 0x04 : 0x40;
    own.attributes.copied = true;
    note_change(page, page + kPageSize);
    any = true;
  }
  if (any) ++generation_;
  return any;
}

bool AddressSpace::keeps_private_copies(const Region& r) {
  return r.shared && (r.attributes.type == PageAttributes::Image || r.attributes.protect == 0x08 || r.attributes.protect == 0x80);
}

bool AddressSpace::privatize(uint64_t page) {
  uint64_t i = first_ending_after(page);
  if (i >= regions_.count() || regions_[i]->base > page) return false;
  if (!regions_[i]->shared) return true;
  if (!split_at(page) || !split_at(page + kPageSize)) return false;
  i = first_ending_after(page);
  Region& r = *regions_[i];
  uint8_t* copy = static_cast<uint8_t*>(allocator_.allocate_aligned(kPageSize, kPageSize));
  if (!copy) return false;
  if (!ByteView(copy, kPageSize).copy_from(ConstByteView(r.storage, kPageSize)) || !pages_.set(page, copy)) { allocator_.deallocate(copy); return false; }
  shared_bytes_ -= kPageSize;
  r.release();
  r.attributes.copied = true;
  note_change(page, page + kPageSize);
  ++generation_;
  return true;
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
    if (regions_[k - 1]->allocation == id) remove_region(k - 1);
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
    remove_region(i);
    ++generation_;
    return true;
  }
  if (rounded > r.size) {
    const uint64_t extra = rounded - r.size;
    if (base >= kUserTop || rounded > kUserTop - base || extra > max_bytes_ || mapped_bytes_ > max_bytes_ - extra) return false;
    if (i + 1 < regions_.count() && regions_[i + 1]->base < base + rounded) return false;
    if (overlaps_reserved(base + r.size, base + rounded)) return false;
    note_change(base + r.size, base + rounded);      // (the new pages have no memory: they are zero)
    mapped_bytes_ += extra;
  } else if (rounded < r.size) {
    mapped_bytes_ -= r.size - rounded;
    drop_pages(base + rounded, base + r.size);
    note_change(base + rounded, base + r.size);
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
    if (r.shared) {
      if (!out.subview(done, n).copy_from(ConstByteView(r.storage + offset, n))) return false;
      done += n;
      continue;
    }
    for (uint64_t moved = 0; moved < n;) {      // page by page: a page without memory reads as zeros
      const uint64_t page = round_down(at + moved), within = at + moved - page;
      const uint64_t piece = n - moved < kPageSize - within ? n - moved : kPageSize - within;
      const ByteView target = out.subview(done + moved, piece);
      if (const uint8_t* memory = pages_.get(page)) {
        if (!target.copy_from(ConstByteView(memory + within, piece))) return false;
      } else {
        target.fill(0);
      }
      moved += piece;
    }
    done += n;
  }
  return true;
}

bool AddressSpace::copy_in(uint64_t address, ConstByteView data) {
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t at = address + done;
    const uint64_t i = first_ending_after(at);
    if (i >= regions_.count() || regions_[i]->base > at) return false;
    const uint64_t offset = at - regions_[i]->base;
    const uint64_t n = data.size() - done < regions_[i]->size - offset ? data.size() - done : regions_[i]->size - offset;
    if (regions_[i]->shared && keeps_private_copies(*regions_[i])) {      // the pages written get copies of their own first (the region is cut, so look again)
      for (uint64_t page = round_down(at); page < at + n; page += kPageSize) if (!privatize(page)) return false;
      continue;
    }
    if (regions_[i]->shared) {
      if (!ByteView(regions_[i]->storage + offset, n).copy_from(data.subview(done, n))) return false;
      done += n;
      continue;
    }
    for (uint64_t moved = 0; moved < n;) {
      const uint64_t page = round_down(at + moved), within = at + moved - page;
      const uint64_t piece = n - moved < kPageSize - within ? n - moved : kPageSize - within;
      const ConstByteView source = data.subview(done + moved, piece);
      uint8_t* memory = pages_.get(page);
      if (!memory) {
        bool zero = true;      // writing zeros into a page that has no memory changes nothing
        for (uint8_t b : source) if (b) { zero = false; break; }
        if (!zero && !(memory = materialize(page))) return false;
      }
      if (memory && !ByteView(memory + within, piece).copy_from(source)) return false;
      moved += piece;
    }
    done += n;
  }
  return true;
}

}  // namespace process
