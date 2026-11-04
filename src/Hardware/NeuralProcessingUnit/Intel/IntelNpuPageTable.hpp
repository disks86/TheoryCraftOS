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


#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Writes the cache lines of `bytes` out of the CPU caches, for memory a device reads without snooping (the NPU's MMU reads its tables that way unless the
// buttress forces snooping). x86 only.
void intel_npu_flush_cache(ConstByteView bytes);

// The bus (DMA) address of memory from IAllocator::allocate_dma: the kernel identity maps RAM and the DMA pool is below 4 GiB, so it is the pointer value. The one
// place a pointer becomes an address; only for memory that came from allocate_dma.
uint64_t intel_npu_bus_address(const void* dma_memory);

// A piece of host memory the NPU may be given access to: a bus (DMA) address and a length, both multiples of the page size.
struct IntelNpuDmaExtent {
  uint64_t bus_addr = 0;
  uint64_t length = 0;
};

// The page table of one NPU MMU context, in the format Intel's NPU (an ARM SMMUv3 with a stage-1 only configuration) walks and the Linux ivpu driver builds
// (ivpu_mmu_context.c): four levels of 512 eight-byte entries covering 48 bits of NPU address space in 4 KiB pages, with a contiguous-hint bit that lets sixteen
// aligned pages share one TLB entry (64 KiB). Every table is a page of DMA memory from the allocator; the page table owns them and frees them in the destructor.
// Not thread safe: the owner serializes calls (ivpu takes the context mutex).
class IntelNpuPageTable {
 public:
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kEntries = 512;
  static constexpr uint64_t kContiguousSize = 16 * kPageSize;
  static constexpr uint64_t kAddressLimit = 1ull << 48;  // NPU addresses are 48 bits
  static constexpr uint64_t kBusLimit = 1ull << 52;      // bus addresses fit entry bits 51:12
  static constexpr uint64_t kAddressMask = ((1ull << 52) - 1) & ~(kPageSize - 1);
  // Entry flags (IVPU_MMU_ENTRY_FLAG_*).
  static constexpr uint64_t kValid = 1ull << 0;
  static constexpr uint64_t kTypePage = 1ull << 1;
  static constexpr uint64_t kLlcCoherent = 1ull << 2;
  static constexpr uint64_t kUser = 1ull << 6;
  static constexpr uint64_t kReadOnly = 1ull << 7;
  static constexpr uint64_t kAccessed = 1ull << 10;
  static constexpr uint64_t kNonGlobal = 1ull << 11;
  static constexpr uint64_t kContiguous = 1ull << 52;
  static constexpr uint64_t kTableEntry = kTypePage | kValid;                       // a pointer to the next level
  static constexpr uint64_t kMapped = kAccessed | kUser | kNonGlobal | kTableEntry;  // a leaf mapping
  static constexpr uint64_t kInvalidEntry = 0xdeadb000;                             // what an unmapped leaf holds: no valid bit, a recognizable address

  struct MapFlags {
    bool read_only = false;
    bool llc_coherent = false;
    bool allow_contiguous = true;
  };

  // `snoop`: the device snoops the CPU caches, so table writes need not be flushed.
  IntelNpuPageTable(allocators::IAllocator& allocator, bool snoop) : allocator_(allocator), snoop_(snoop), pages_(allocator) {}
  ~IntelNpuPageTable();
  IntelNpuPageTable(const IntelNpuPageTable&) = delete;
  IntelNpuPageTable& operator=(const IntelNpuPageTable&) = delete;

  // Allocates the top-level table if it does not exist yet. False if out of memory.
  bool ensure_root();
  // The bus address of the top-level table (what a context descriptor points at); 0 before ensure_root().
  uint64_t root_bus_address() const { return root_bus_; }
  uint64_t table_count() const { return pages_.count(); }

  // Maps [vpu_addr, vpu_addr + size) to the bus addresses starting at bus_addr. Everything page aligned and inside the address limits; the range must not wrap.
  // On failure nothing stays mapped by this call.
  bool map(uint64_t vpu_addr, uint64_t bus_addr, uint64_t size, MapFlags flags);
  // Marks the pages unmapped (the tables stay). Pages that were never mapped are left alone.
  void unmap(uint64_t vpu_addr, uint64_t size);
  // Makes mapped pages read-only (ivpu_mmu_context_set_pages_ro). False, and nothing changed, if any page of the range is not mapped.
  bool set_read_only(uint64_t vpu_addr, uint64_t size);
  // The leaf entry for the page containing vpu_addr, if the tables to it exist.
  bool lookup(uint64_t vpu_addr, uint64_t* entry) const;

  static bool range_ok(uint64_t vpu_addr, uint64_t size) {
    return size != 0 && (vpu_addr & (kPageSize - 1)) == 0 && (size & (kPageSize - 1)) == 0 && vpu_addr < kAddressLimit && size <= kAddressLimit - vpu_addr;
  }

 private:
  ArrayView<uint64_t> new_table(uint64_t* bus);
  uint64_t* leaf(uint64_t vpu_addr, bool create);
  // The leaf slot if the tables to it exist (the tables are views of memory we own, so a const walk may hand out a writable slot).
  uint64_t* find_leaf(uint64_t vpu_addr) const;
  void write_entry(uint64_t* slot, uint64_t value);
  void split_contiguous(uint64_t vpu_addr);

  allocators::IAllocator& allocator_;
  bool snoop_;
  ArrayView<uint64_t> root_;
  uint64_t root_bus_ = 0;
  Dictionary<uint64_t, ArrayView<uint64_t>> pages_;  // every table below the root, by bus address
};

}  // namespace hardware
