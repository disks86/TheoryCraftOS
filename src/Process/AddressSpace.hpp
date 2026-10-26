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
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

constexpr uint8_t kProtRead = 1, kProtWrite = 2, kProtExecute = 4;

// The memory of one user-mode process: a set of page-aligned regions, each with its own permissions and its own zero-filled backing store. This is the
// kernel's view of the address space (the bounds-checked way a system call reaches user memory); it does not touch the CPU's page tables. When the
// ring-3 entry exists, the same regions are what gets mapped into the process's hardware page tables.
//
// Regions never overlap and the whole space is below kUserTop. A request that does not fit (overlap, beyond the top, over the byte budget, out of memory)
// fails and changes nothing.
class AddressSpace final : public IUserMemory {
 public:
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kUserTop = 0x0000800000000000ull;      // the lower half of 48-bit addresses

  // `max_bytes` limits what all regions together may hold.
  explicit AddressSpace(allocators::IAllocator& allocator, uint64_t max_bytes = 256ull << 20) : allocator_(allocator), regions_(allocator), max_bytes_(max_bytes) {}
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
  // Unmaps a range that is exactly one or more whole regions. (Splitting a region is not supported yet.)
  bool unmap(uint64_t address, uint64_t size);
  // The lowest page-aligned address at or above `hint` where `size` bytes fit between regions and below kUserTop.
  bool find_free(uint64_t size, uint64_t hint, uint64_t* out) const;
  // Changes the permissions of a range that is exactly one or more whole regions.
  bool protect(uint64_t address, uint64_t size, uint8_t protection);

  // The kernel's own access, which ignores the permissions (a loader fills read-only code pages this way). Fails if any byte is unmapped.
  bool read_privileged(uint64_t address, ByteView out) const { return check(address, out.size(), 0) && copy_out(address, out); }
  bool write_privileged(uint64_t address, ConstByteView data) { return check(address, data.size(), 0) && copy_in(address, data); }

  // IUserMemory: the program's access. Needs read (or write) permission on every page; fails without changing anything otherwise.
  bool read(uint64_t address, ByteView out) override { return check(address, out.size(), kProtRead) && copy_out(address, out); }
  bool write(uint64_t address, ConstByteView data) override { return check(address, data.size(), kProtWrite) && copy_in(address, data); }

  // Is every byte of the range mapped with at least these permissions?
  bool check(uint64_t address, uint64_t size, uint8_t protection) const;
  bool protection_at(uint64_t address, uint8_t* out) const;
  uint64_t mapped_bytes() const { return mapped_bytes_; }
  uint64_t region_count() const { return regions_.count(); }

 private:
  struct Region {
    explicit Region(allocators::IAllocator& allocator) : data(allocator) {}
    uint64_t base = 0, size = 0;
    uint8_t protection = 0;
    ByteBuffer data;
  };

  // The index of the first region whose end is above `address` (count() if none).
  uint64_t first_ending_after(uint64_t address) const;
  // Both assume check() passed for the range.
  bool copy_out(uint64_t address, ByteView out) const;
  bool copy_in(uint64_t address, ConstByteView data);
  bool whole_regions(uint64_t address, uint64_t size, uint64_t* first, uint64_t* last) const;

  allocators::IAllocator& allocator_;
  List<UniquePointer<Region>> regions_;      // sorted by base
  uint64_t max_bytes_;
  uint64_t mapped_bytes_ = 0;
};

}  // namespace process
