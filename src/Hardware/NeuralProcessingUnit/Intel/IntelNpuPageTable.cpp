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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuPageTable.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
// Host memory from IAllocator::allocate_dma is identity mapped and below 4 GiB, so a table's address is also its bus address; this is the only place that
// turns one into the other. The pointer is to memory this file allocated and owns.
uint64_t bus_address_of(const void* table) { return reinterpret_cast<uint64_t>(table); }

constexpr uint32_t kUpperShifts[3] = {39, 30, 21};  // the index bits of the top three levels
constexpr uint64_t index_at(uint64_t vpu_addr, uint32_t shift) { return (vpu_addr >> shift) & (IntelNpuPageTable::kEntries - 1); }
}  // namespace

uint64_t intel_npu_bus_address(const void* dma_memory) { return bus_address_of(dma_memory); }

void intel_npu_flush_cache(ConstByteView bytes) {
  constexpr uint64_t kLine = 64;
  if (bytes.empty()) return;
  const uint64_t start = bus_address_of(bytes.data());
  const uint64_t end = start + bytes.size();
  for (uint64_t at = start & ~(kLine - 1); at < end; at += kLine) __builtin_ia32_clflush(bytes.data() + (at < start ? 0 : at - start));
  __builtin_ia32_sfence();
}

IntelNpuPageTable::~IntelNpuPageTable() {
  for (auto& page : pages_) release_view(allocator_, page.value);
  release_view(allocator_, root_);
}

ArrayView<uint64_t> IntelNpuPageTable::new_table(uint64_t* bus) {
  ArrayView<uint64_t> table = allocate_dma_view<uint64_t>(allocator_, kEntries, kPageSize);
  if (table.empty()) return table;
  table.fill(0);
  *bus = bus_address_of(table.data());
  if (*bus >= kBusLimit || (*bus & (kPageSize - 1)) != 0) {  // unusable for the device
    release_view(allocator_, table);
    return table;
  }
  if (!snoop_) intel_npu_flush_cache(ConstByteView(reinterpret_cast<const uint8_t*>(table.data()), table.size_bytes()));
  return table;
}

bool IntelNpuPageTable::ensure_root() {
  if (!root_.empty()) return true;
  root_ = new_table(&root_bus_);
  return !root_.empty();
}

void IntelNpuPageTable::write_entry(uint64_t* slot, uint64_t value) {
  *slot = value;
  if (!snoop_) intel_npu_flush_cache(ConstByteView(reinterpret_cast<const uint8_t*>(slot), sizeof(*slot)));
}

uint64_t* IntelNpuPageTable::find_leaf(uint64_t vpu_addr) const {
  if (root_.empty() || vpu_addr >= kAddressLimit) return nullptr;
  ArrayView<uint64_t> table = root_;
  for (const uint32_t shift : kUpperShifts) {
    const uint64_t entry = table[index_at(vpu_addr, shift)];
    if (!(entry & kValid)) return nullptr;
    const ArrayView<uint64_t>* child = pages_.find(entry & kAddressMask);
    if (!child) return nullptr;  // not one of ours: never followed
    table = *child;
  }
  return &table[index_at(vpu_addr, 12)];
}

uint64_t* IntelNpuPageTable::leaf(uint64_t vpu_addr, bool create) {
  if (!create) return find_leaf(vpu_addr);
  if (vpu_addr >= kAddressLimit || !ensure_root()) return nullptr;
  ArrayView<uint64_t> table = root_;
  for (const uint32_t shift : kUpperShifts) {
    uint64_t* slot = &table[index_at(vpu_addr, shift)];
    if (!(*slot & kValid)) {
      uint64_t bus = 0;
      ArrayView<uint64_t> child = new_table(&bus);
      if (child.empty()) return nullptr;
      if (!pages_.put(bus, child)) {
        release_view(allocator_, child);
        return nullptr;
      }
      write_entry(slot, bus | kTableEntry);
      table = child;
    } else {
      const ArrayView<uint64_t>* child = pages_.find(*slot & kAddressMask);
      if (!child) return nullptr;
      table = *child;
    }
  }
  return &table[index_at(vpu_addr, 12)];
}

bool IntelNpuPageTable::lookup(uint64_t vpu_addr, uint64_t* entry) const {
  const uint64_t* slot = find_leaf(vpu_addr);
  if (!slot) return false;
  *entry = *slot;
  return true;
}

bool IntelNpuPageTable::map(uint64_t vpu_addr, uint64_t bus_addr, uint64_t size, MapFlags flags) {
  if (!range_ok(vpu_addr, size) || (bus_addr & (kPageSize - 1)) != 0 || bus_addr >= kBusLimit || size > kBusLimit - bus_addr) return false;
  if (!ensure_root()) return false;
  uint64_t prot = kMapped;
  if (flags.llc_coherent) prot |= kLlcCoherent;
  if (flags.read_only) prot |= kReadOnly;
  uint64_t done = 0;
  while (done < size) {
    const uint64_t va = vpu_addr + done, pa = bus_addr + done;
    const bool cont = flags.allow_contiguous && size - done >= kContiguousSize && ((va | pa) & (kContiguousSize - 1)) == 0;
    const uint64_t step = cont ? kContiguousSize : kPageSize;
    for (uint64_t off = 0; off < step; off += kPageSize) {
      uint64_t* slot = leaf(va + off, true);
      if (!slot) {
        unmap(vpu_addr, done + off);
        return false;
      }
      write_entry(slot, (pa + off) | prot | (cont ? kContiguous : 0));
    }
    done += step;
  }
  return true;
}

void IntelNpuPageTable::unmap(uint64_t vpu_addr, uint64_t size) {
  if (!range_ok(vpu_addr, size)) return;
  for (uint64_t off = 0; off < size; off += kPageSize) {
    uint64_t* slot = leaf(vpu_addr + off, false);
    if (slot) write_entry(slot, kInvalidEntry);
  }
}

// A 64 KiB hint covers sixteen pages that all change together; clearing the hint on the whole block lets one of them change alone.
void IntelNpuPageTable::split_contiguous(uint64_t vpu_addr) {
  const uint64_t start = vpu_addr & ~(kContiguousSize - 1);
  for (uint64_t off = 0; off < kContiguousSize; off += kPageSize) {
    uint64_t* slot = leaf(start + off, false);
    if (slot && (*slot & kValid)) write_entry(slot, *slot & ~kContiguous);
  }
}

bool IntelNpuPageTable::set_read_only(uint64_t vpu_addr, uint64_t size) {
  if (!range_ok(vpu_addr, size)) return false;
  for (uint64_t off = 0; off < size; off += kPageSize) {
    const uint64_t* slot = find_leaf(vpu_addr + off);
    if (!slot || !(*slot & kValid)) return false;
  }
  if ((vpu_addr & (kContiguousSize - 1)) != 0) split_contiguous(vpu_addr);
  const uint64_t end = vpu_addr + size;
  if ((end & (kContiguousSize - 1)) != 0) split_contiguous(end);
  for (uint64_t off = 0; off < size; off += kPageSize) {
    uint64_t* slot = leaf(vpu_addr + off, false);
    write_entry(slot, *slot | kReadOnly);
  }
  return true;
}

}  // namespace hardware
