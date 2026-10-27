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
#include "ObjectContainer/List.hpp"
#include "Process/Process.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

// A range of addresses the address space must keep free (the kernel's own pages that the CPU's user page tables have to map).
struct AddressRange {
  uint64_t base = 0, size = 0;
};

constexpr uint8_t kProtRead = 1, kProtWrite = 2, kProtExecute = 4;

// The memory of one user-mode process: a set of page-aligned regions, each with its own permissions and its own zero-filled backing store. This is the
// kernel's view of the address space (the bounds-checked way a system call reaches user memory); it does not touch the CPU's page tables. When the
// ring-3 entry exists, the same regions are what gets mapped into the process's hardware page tables.
//
// Regions never overlap and the whole space is below kUserTop. A request that does not fit (overlap, beyond the top, over the byte budget, out of memory)
// fails and changes nothing.
// Memory that several address spaces map at once (a section: CreateFileMapping). The owner counts the mappings and frees the memory when the last is gone.
class SharedMemory {
 public:
  virtual void retain() = 0;
  virtual void release() = 0;
  virtual void flush() {}      // write what is in the memory back to where it came from (FlushViewOfFile)

 protected:
  SharedMemory() = default;
  ~SharedMemory() = default;
};

// What the Windows layer knows about a run of pages beyond the permissions: the PAGE_* constant it was given (copy-on-write, guard ...), the one the allocation started
// with, what kind of memory it is and whether it is only reserved. protect == 0: nothing recorded, the Windows layer derives it from the permissions.
struct PageAttributes {
  enum Type : uint8_t { Private = 0, Mapped = 1, Image = 2 };
  uint32_t protect = 0, allocation_protect = 0;
  uint8_t type = Private;
  bool reserved = false;      // reserved, not committed
  bool file_view = false;     // a view of a file (a view of plain memory can still be committed)
  bool operator==(const PageAttributes& o) const { return protect == o.protect && allocation_protect == o.allocation_protect && type == o.type && reserved == o.reserved && file_view == o.file_view; }
};

class AddressSpace final : public IUserMemory {
 public:
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kUserTop = 0x0000800000000000ull;      // the lower half of 48-bit addresses

  // `max_bytes` limits what all regions together may hold.
  explicit AddressSpace(allocators::IAllocator& allocator, uint64_t max_bytes = 256ull << 20) : allocator_(allocator), retired_(allocator), regions_(allocator), max_bytes_(max_bytes) {}
  ~AddressSpace() {
    retired_.deferred = false;
    reclaim();
  }
  AddressSpace(const AddressSpace&) = delete;
  AddressSpace& operator=(const AddressSpace&) = delete;

  static constexpr uint64_t round_down(uint64_t value) { return value & ~(kPageSize - 1); }
  // False on overflow (the value has no page-aligned ceiling).
  static bool round_up(uint64_t value, uint64_t* out) {
    if (value > ~uint64_t{0} - (kPageSize - 1)) return false;
    *out = (value + kPageSize - 1) & ~(kPageSize - 1);
    return true;
  }

  // Maps [address, address + size) with `protection` (size rounded up to whole pages), zero-filled. `address` must be page-aligned and size non-zero.
  bool map(uint64_t address, uint64_t size, uint8_t protection);
  // Maps memory that is not the address space's own: `storage` (page-aligned, `size` bytes, rounded up to pages) stays as it is and is shared with whoever else maps it.
  // `owner` is told (retain) once for the mapping and again for every piece it is cut into, and (release) when a piece is unmapped, which with deferred freeing is when the
  // CPUs can no longer see it.
  bool map_shared(uint64_t address, uint64_t size, uint8_t protection, uint8_t* storage, SharedMemory* owner);
  bool is_shared(uint64_t address) const;
  SharedMemory* shared_at(uint64_t address) const;
  // Unmaps whatever is mapped inside [address, address + size) (size rounded up); regions that stick out are cut. Gaps are fine. False for a bad range
  // or if cutting a region ran out of memory (nothing is unmapped then).
  bool unmap(uint64_t address, uint64_t size);
  // The lowest page-aligned address at or above `hint` where `size` bytes fit between regions and below kUserTop.
  bool find_free(uint64_t size, uint64_t hint, uint64_t* out) const;
  // Changes the permissions of a range, cutting regions at its ends. Every page of the range must be mapped, or nothing changes.
  bool protect(uint64_t address, uint64_t size, uint8_t protection);
  // Makes the region that starts at `base` exactly `size` bytes (rounded up to pages) long: new pages are zero, a size of 0 removes it. Fails if the pages
  // after it are in use, past kUserTop, or over the byte budget. This is how a heap grows.
  bool resize(uint64_t base, uint64_t size);
  // The region that contains `address`: where it starts and how long it is.
  bool region_extent(uint64_t address, uint64_t* base, uint64_t* size) const;
  // The whole allocation that contains `address`: the pages one map() call created, which stay together even after protect() or unmap() cut them into
  // several regions. Returns where the surviving pieces start and the total length up to the last contiguous piece.
  bool allocation_extent(uint64_t address, uint64_t* base, uint64_t* size) const;

  // With this on, the memory behind a region that is unmapped, cut or replaced is not given back at once but kept until reclaim(): hardware page tables that other CPUs
  // still run on may point at it. The user of the page tables calls reclaim() when no CPU can have the old mappings any more.
  void set_deferred_free(bool on) { retired_.deferred = on; }
  void reclaim();

  // Addresses no region may cover (map, resize and find_free keep away from them). The ranges must outlive the address space.
  void set_reserved(ArrayView<const AddressRange> ranges) { reserved_ = ranges; }

  // For building hardware page tables: region `index` (0 .. region_count() - 1, in address order), with the memory that backs it. The backing memory is
  // page-aligned, `size` bytes long and stays where it is until the layout changes (generation() moves).
  struct RegionInfo {
    uint64_t base = 0, size = 0;
    uint8_t protection = 0;
    const uint8_t* storage = nullptr;
    uint64_t allocation = 0;
    PageAttributes attributes;
  };
  bool region_at(uint64_t index, RegionInfo* out) const;
  // Counts every change to the layout (map, unmap, protect, resize), so a user of the page tables knows when to rebuild them.
  uint64_t generation() const { return generation_; }

  // The kernel's own access, which ignores the permissions (a loader fills read-only code pages this way). Fails if any byte is unmapped.
  bool read_privileged(uint64_t address, ByteView out) const { return check(address, out.size(), 0) && copy_out(address, out); }
  bool write_privileged(uint64_t address, ConstByteView data) { return check(address, data.size(), 0) && copy_in(address, data); }

  // IUserMemory: the program's access. Needs read (or write) permission on every page; fails without changing anything otherwise.
  bool read(uint64_t address, ByteView out) override { return check(address, out.size(), kProtRead) && copy_out(address, out); }
  bool write(uint64_t address, ConstByteView data) override { return (check(address, data.size(), kProtWrite) || (break_copy_on_write(address, data.size()) && check(address, data.size(), kProtWrite))) && copy_in(address, data); }

  // Is every byte of the range mapped with at least these permissions?
  bool check(uint64_t address, uint64_t size, uint8_t protection) const;
  bool protection_at(uint64_t address, uint8_t* out) const;
  // The Windows attributes of [address, address + size), which must be mapped; the regions are cut at its ends.
  bool set_attributes(uint64_t address, uint64_t size, const PageAttributes& attributes);
  bool attributes_at(uint64_t address, PageAttributes* out) const;
  // Changes only the PAGE_* constant and the reserved flag of [address, address + size), keeping the rest of each region's attributes.
  // A write to copy-on-write pages (PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY) of [address, address + size): each such page gets a copy of its own (a view of a section stops
  // sharing it), becomes writable and is PAGE_READWRITE / PAGE_EXECUTE_READWRITE from then on. False if the range holds no such page or memory ran out.
  bool break_copy_on_write(uint64_t address, uint64_t size);
  bool set_page_protect(uint64_t address, uint64_t size, uint32_t protect, bool reserved);
  uint64_t mapped_bytes() const { return mapped_bytes_; }
  uint64_t region_count() const { return regions_.count(); }

 private:
  // Memory of regions that went away while deferred_free is on.
  struct RetiredPiece {
    uint8_t* storage = nullptr;
    SharedMemory* shared = nullptr;      // set: a mapping to let go of instead of memory to free
  };
  struct Retired {
    explicit Retired(allocators::IAllocator& allocator) : storages(allocator) {}
    List<RetiredPiece> storages;
    bool deferred = false;
  };
  // One run of pages and the page-aligned memory behind it (`capacity` may exceed `size` after a cut or a shrink).
  struct Region {
    Region(allocators::IAllocator& allocator, Retired& retired) : allocator_(allocator), retired_(retired) {}
    ~Region() { release(); }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    // Gets `bytes` of zeroed, page-aligned memory (replacing any). False if it ran out.
    bool allocate(uint64_t bytes);
    void release();
    ByteView bytes() const { return ByteView(storage, size); }
    uint64_t base = 0, size = 0, allocation = 0;      // `allocation` ties together the pieces of one map() call
    uint8_t protection = 0;
    PageAttributes attributes;
    uint8_t* storage = nullptr;
    uint64_t capacity = 0;
    SharedMemory* shared = nullptr;      // not ours: a section's memory, which we only map

   private:
    allocators::IAllocator& allocator_;
    Retired& retired_;
  };
  bool overlaps_reserved(uint64_t start, uint64_t end, uint64_t* reserved_end = nullptr) const;

  // The index of the first region whose end is above `address` (count() if none).
  uint64_t first_ending_after(uint64_t address) const;
  // Both assume check() passed for the range.
  bool copy_out(uint64_t address, ByteView out) const;
  bool copy_in(uint64_t address, ConstByteView data);
  // Makes `address` (page-aligned) a region boundary, copying the tail of the region it falls inside.
  bool split_at(uint64_t address);
  bool prepare_range(uint64_t address, uint64_t size, uint64_t* end) const;

  allocators::IAllocator& allocator_;
  Retired retired_;
  List<UniquePointer<Region>> regions_;      // sorted by base
  uint64_t max_bytes_;
  uint64_t mapped_bytes_ = 0;
  uint64_t next_allocation_ = 1;
  uint64_t generation_ = 0;
  ArrayView<const AddressRange> reserved_;
};

}  // namespace process
