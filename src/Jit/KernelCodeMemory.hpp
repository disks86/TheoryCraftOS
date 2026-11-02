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
#include "Jit/CodeMemory.hpp"
#include "Jit/CodePageSource.hpp"
#include "Jit/SpinFlag.hpp"
#include "Kernel/Paging.hpp"

namespace jit {

// Tells the other cores to drop their cached translations for a range whose permissions just changed (an IPI-driven TLB shootdown). PageTables only flushes
// the calling core, and a core that still holds the old "writable, not executable" translation of a page that just became code would fault on entering
// it. Optional: without one, only the calling core is flushed, which is right while the other cores are not running JIT code yet.
class ITlbShootdown {
 public:
  // False if some core did not answer (then the permission change must not be relied on).
  virtual bool invalidate_range(uint64_t address, uint64_t bytes) = 0;

 protected:
  constexpr ITlbShootdown() = default;
  ~ITlbShootdown() = default;
};

// ICodeMemory for the kernel: pages from an ICodePageSource (a CodePool of 2 MiB windows, or the allocator), whose permissions are switched in the page tables the kernel already owns (kernel::PageTables,
// identity-mapped). allocate() hands out pages that are writable and not executable; make_executable() maps them executable and read-only; make_writable()
// maps them back; release() restores the mapping every ordinary allocation has (writable, executable if the firmware map was) before the pages go back
// to the allocator, so the next user finds ordinary memory.
//
// Every region owns whole 4 KiB pages and nothing else lives in them, so changing a region's permissions never affects other data. With the allocator as
// the source, a 2 MiB or 1 GiB firmware page that holds a region is split into smaller pages (PageTables does that on demand), wherever regions happen to
// land; a CodePool confines that to its own 2 MiB windows.
//
// Without NX (PageAttributes::executable is ignored then) W^X cannot be enforced; can_execute() is still true and the pages stay executable, so the code
// runs but the hardening is absent. nx_enforced() says which case this is.
class KernelCodeMemory final : public ICodeMemory {
 public:
  // Pages from `pages` (a CodePool, normally).
  KernelCodeMemory(kernel::PageTables& tables, ICodePageSource& pages, bool nx_enforced, ITlbShootdown* shootdown = nullptr)
      : tables_(tables), pages_(pages), nx_(nx_enforced), shootdown_(shootdown) {}
  // Pages straight from the allocator, one allocation per region.
  KernelCodeMemory(kernel::PageTables& tables, allocators::IAllocator& allocator, bool nx_enforced, ITlbShootdown* shootdown = nullptr)
      : tables_(tables), own_source_(allocator), pages_(own_source_), nx_(nx_enforced), shootdown_(shootdown) {}

  // After the local instruction cache flush of make_executable(), also synchronise the other cores (see IInstructionCacheSync). Optional; x86 needs none
  // (the TLB shootdown's interrupt is serialising).
  void set_instruction_sync(IInstructionCacheSync* sync) { icache_sync_ = sync; }

  uint64_t page_size() const override { return kPage; }
  bool allocate(uint64_t size, CodeRegion* out) override;
  bool make_executable(const CodeRegion& region) override;
  bool make_writable(const CodeRegion& region) override;
  void release(const CodeRegion& region) override;
  bool can_execute() const override { return true; }
  bool nx_enforced() const { return nx_; }

 private:
  static constexpr uint64_t kPage = kCodePage;
  bool set(const CodeRegion& region, bool writable, bool executable);

  kernel::PageTables& tables_;
  AllocatorPageSource own_source_;
  ICodePageSource& pages_;
  bool nx_;
  ITlbShootdown* shootdown_;
  IInstructionCacheSync* icache_sync_ = nullptr;
  SpinFlag tables_lock_;   // (PageTables is not thread safe; regions are made on whichever thread compiles)
};

}  // namespace jit
