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
#include "Process/AddressSpace.hpp"
#include "Utility/ArrayView.hpp"

namespace process {

// The x86-64 page tables a user-mode process runs on: its AddressSpace regions at their user addresses (4 KiB pages, user-accessible, the permissions of
// the region), plus a few kernel pages mapped 1:1 and for the kernel only (the entry code the CPU needs while it is still on these tables). Everything
// else is absent, so a stray user access faults.
//
// Table memory comes from the allocator and is used by its address, so it must be mapped 1:1 in the kernel (physical = virtual). The same holds for the
// regions' backing memory. root() is what goes into CR3.
class UserPageTables {
 public:
  // `nx` says the CPU has no-execute enabled (EFER.NXE); without it the bit would be a reserved bit and fault.
  UserPageTables(allocators::IAllocator& allocator, bool nx) : allocator_(allocator), nx_(nx) {}
  ~UserPageTables() { release(); release_previous(); }
  UserPageTables(const UserPageTables&) = delete;
  UserPageTables& operator=(const UserPageTables&) = delete;

  // A kernel-only page range mapped at its own address. `executable` for code pages. Added to every rebuild.
  struct Identity {
    uint64_t address = 0, size = 0;
    bool writable = false, executable = false;
  };
  bool add_identity(const Identity& range);

  // Builds the tables again from the address space. The new tables are made beside the old ones and replace them only when they are complete, so a failure (memory ran
  // out) leaves the old ones as they were, still usable by anything that runs on them.
  // With `keep_previous` the old set is not freed but kept until release_previous() (or the next rebuild): other CPUs may still be running on it.
  bool rebuild(const AddressSpace& space, bool keep_previous = false);
  // Brings the tables up to date: built the first time, after that only the ranges the space says changed (AddressSpace::take_change) are redone, in place, while
  // other CPUs may be running on the tables (each entry is one store). `*flush` (if given) is set when an entry a CPU may have cached went away, lost a permission or
  // points elsewhere now: those CPUs must drop their TLB before the memory the space let go of is reused. False if memory for a table ran out (the tables are then
  // only partly up to date: entries that went away are gone, new ones may be missing).
  bool sync(AddressSpace& space, bool keep_previous = false, bool* flush = nullptr);
  // Redoes the entries of [start, end) from the space. Pages the program may use and that have memory get entries; a page without memory (a private page never
  // touched) has none until the program's first touch asks for one (the page fault, which gives it memory and calls this for the page).
  bool update(const AddressSpace& space, uint64_t start, uint64_t end, bool* flush);
  void release_previous();
  // The entry of the page at `address` in the current tables (0 if there is none).
  uint64_t entry_of(uint64_t address);

  uint64_t root() const { return root_; }
  bool built() const { return built_; }

  // The entry flags used: present, writable, user, no-execute.
  static constexpr uint64_t kPresent = 1, kWritable = 2, kUser = 4, kNoExecute = 1ull << 63;

 private:
  // The entry for `address` in the tables whose root is `root` (made on the way when `make`, the tables going into set `into`): nullptr if a level is missing.
  uint64_t* leaf(uint64_t root, uint64_t address, bool make, uint32_t into);
  bool map_page(uint64_t address, uint64_t physical, uint64_t leaf_flags);
  uint64_t* new_table(uint32_t into);
  void release();
  // The entry `address` should have now (0: none).
  uint64_t wanted(const AddressSpace& space, uint64_t address) const;
  uint64_t leaf_flags(uint8_t protection) const;
  // The first pass of update(): every present user entry of [start, end) below `table` (a table of `level`, covering from `base`) set to what it should be.
  void refresh(const AddressSpace& space, uint64_t table, uint32_t level, uint64_t base, uint64_t start, uint64_t end, bool* flush);

  allocators::IAllocator& allocator_;
  List<Identity> identity_ = List<Identity>(allocator_);
  List<uint64_t*> sets_[2] = {List<uint64_t*>(allocator_), List<uint64_t*>(allocator_)};      // the tables of the current set, and of a rebuild in progress
  uint32_t active_ = 0;                                                                         // which of the two is the current set
  uint64_t building_root_ = 0;
  bool nx_;
  bool built_ = false;
  uint64_t root_ = 0;
};

}  // namespace process
