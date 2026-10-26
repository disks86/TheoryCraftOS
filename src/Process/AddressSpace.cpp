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
  UniquePointer<Region> region = UniquePointer<Region>::make(allocator_, allocator_);
  if (!region) return false;
  region->base = address;
  region->size = rounded;
  region->protection = protection;
  region->allocation = next_allocation_;
  if (!region->data.put_zeros(rounded)) return false;
  if (!regions_.insert(index, static_cast<UniquePointer<Region>&&>(region))) return false;
  mapped_bytes_ += rounded;
  ++next_allocation_;
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
  UniquePointer<Region> tail = UniquePointer<Region>::make(allocator_, allocator_);
  if (!tail) return false;
  tail->base = address;
  tail->size = r.size - offset;
  tail->protection = r.protection;
  tail->allocation = r.allocation;
  if (!tail->data.append(r.data.view().subview(offset))) return false;
  if (!regions_.insert(i + 1, static_cast<UniquePointer<Region>&&>(tail))) return false;
  r.size = offset;
  r.data.truncate(offset);
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
  return true;
}

bool AddressSpace::protect(uint64_t address, uint64_t size, uint8_t protection) {
  uint64_t end = 0;
  if (!prepare_range(address, size, &end) || !check(address, end - address, 0)) return false;
  if (!split_at(address) || !split_at(end)) return false;
  for (uint64_t i = first_ending_after(address); i < regions_.count() && regions_[i]->base < end; ++i) regions_[i]->protection = protection;
  return true;
}

bool AddressSpace::resize(uint64_t base, uint64_t size) {
  const uint64_t i = first_ending_after(base);
  if (base % kPageSize != 0 || i >= regions_.count() || regions_[i]->base != base) return false;
  Region& r = *regions_[i];
  uint64_t rounded = 0;
  if (!round_up(size, &rounded)) return false;
  if (rounded == 0) {
    mapped_bytes_ -= r.size;
    regions_.remove_at(i);
    return true;
  }
  if (rounded > r.size) {
    const uint64_t extra = rounded - r.size;
    if (base >= kUserTop || rounded > kUserTop - base || extra > max_bytes_ || mapped_bytes_ > max_bytes_ - extra) return false;
    if (i + 1 < regions_.count() && regions_[i + 1]->base < base + rounded) return false;
    if (!r.data.put_zeros(extra)) return false;
    mapped_bytes_ += extra;
  } else {
    mapped_bytes_ -= r.size - rounded;
    r.data.truncate(rounded);
  }
  r.size = rounded;
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
  for (uint64_t i = first_ending_after(candidate);; ++i) {
    if (candidate > kUserTop - rounded) return false;
    if (i >= regions_.count() || regions_[i]->base >= candidate + rounded) {
      *out = candidate;
      return true;
    }
    candidate = regions_[i]->base + regions_[i]->size;
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
    if (!out.subview(done, n).copy_from(r.data.view().subview(offset, n))) return false;
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
    if (!r.data.writable_view().subview(offset, n).copy_from(data.subview(done, n))) return false;
    done += n;
  }
  return true;
}

}  // namespace process
