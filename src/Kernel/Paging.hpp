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

namespace kernel {

// The CPU's page tables, as far as an OS that keeps the firmware's identity map needs them: making sure a range of physical addresses is mapped
// 1:1 (virtual = physical) with the memory type and permissions wanted. What for: device registers (MMIO) that the firmware did not map, or
// mapped as ordinary memory, and RAM above 4 GiB the firmware left unmapped. 4-level paging (48-bit addresses; mappings go up to 128 TiB), 4 KiB,
// 2 MiB and 1 GiB pages: existing large pages are split when only part of one changes, and the largest page that fits is used for new mappings.
//
// Every core may map, unmap and query at the same time, with no lock: each entry is changed with a compare-and-swap, and a change that lost a race is
// decided again on the new value (see walk()). Changes to ranges that overlap take effect in some order, page table by page table; ranges that only share
// a table or a large page (two device registers in the same 2 MiB, say) both get what they asked for.
//
// Other cores' TLBs: with a shootdown set (set_shootdown()), every change is flushed on every core before map()/unmap() return; without one only the
// calling core is flushed (right while the other cores do not run yet, or for a caller that flushes them itself).
//
// Tables that a change unlinks (a page mapped or unmapped over a table as a whole) are freed once nothing can use them any more: after the shootdown has
// reached every core (so no CPU caches them) and after an RCU grace period (so no other core's map/query is still walking them). Only tables this class
// made are freed (their entries carry a software bit); the firmware's stay where they are. Without a shootdown, or if it fails, unlinked tables are kept.
//
// What it does not do: work with 5-level paging.
enum class CacheMode : uint8_t {
  WriteBack,        // ordinary memory
  WriteThrough,
  Uncached,         // device registers: every access happens, in order, at once
  WriteCombining,   // frame buffers: writes may be merged and delayed (needs enable_write_combining())
};

struct PageAttributes {
  CacheMode cache = CacheMode::WriteBack;
  bool writable = true;
  bool executable = true;   // ignored (the page stays executable) if the CPU does not have NX switched on
};

struct PageMapping {
  bool present = false;
  uint64_t physical = 0;     // of the page that contains the address
  uint64_t page_size = 0;    // 4 KiB, 2 MiB or 1 GiB
  PageAttributes attributes;
};

// What touches the real machine; a test supplies a pretend one.
class IPagingHost {
 public:
  virtual uint64_t root() = 0;              // physical address of the top-level table (CR3)
  virtual bool five_level() = 0;            // 5-level paging is on (not supported here)
  virtual bool nx_enabled() = 0;
  virtual bool gigabyte_pages() = 0;        // the CPU can map 1 GiB pages
  virtual bool pat_available() = 0;
  virtual uint64_t read_pat() = 0;          // the IA32_PAT register of this core
  virtual void write_pat(uint64_t value) = 0;
  virtual void invalidate_page(uint64_t address) = 0;
  virtual void invalidate_all() = 0;        // flush this core's translations
  // Brackets a change to the tables. The firmware may keep its own page tables read-only (CR0.WP set, the tables' pages mapped RO), which would fault the writes: the
  // real host switches write protection off on this core, with interrupts masked so that nothing else runs meanwhile, and end_table_edit() puts both back
  // (the token carries what to restore). A pretend host has nothing to do.
  virtual uint64_t begin_table_edit() { return 0; }
  virtual void end_table_edit(uint64_t) {}
  // May the caller wait for the other cores (a shootdown)? Not with interrupts masked (an interrupt handler, a spin lock held).
  virtual bool can_wait() { return true; }

 protected:
  IPagingHost() = default;
  ~IPagingHost() = default;
};

// Tells every core, the calling one included, to drop its cached translations of a range: an IPI-driven TLB shootdown (jit::CrossCoreTlbShootdown is the
// kernel's). A range that covers everything may be flushed as a whole, which also drops the CPUs' caches of page tables.
class ITlbShootdown {
 public:
  // False if some core did not answer (then the change must not be relied on).
  virtual bool invalidate_range(uint64_t address, uint64_t bytes) = 0;

 protected:
  constexpr ITlbShootdown() = default;
  ~ITlbShootdown() = default;
};

class RcuDomain;
struct RcuNode;

// The host for the CPU this code runs on.
IPagingHost& cpu_paging_host();

// The PAT value that enable_write_combining() settled on (0 if write-combining was never set up), and a way for a core that starts later to load it: the
// PAT register belongs to each core, and memory mapped write-combining must mean the same on all of them.
uint64_t shared_pat_value();
void load_shared_pat_on_this_core();

class PageTables {
 public:
  // `allocator` supplies the pages for new tables (4 KiB aligned; they are used by their address, so memory must be mapped 1:1) and takes back the ones that
  // were unlinked. `rcu` is the domain whose grace periods guard those (the default domain if null).
  PageTables(IPagingHost& host, allocators::IAllocator& allocator, RcuDomain* rcu = nullptr);

  // From now on every change is flushed on every core through `shootdown` (null: on this core only), and unlinked tables are freed. A shootdown waits for
  // the other cores: a caller that cannot wait (interrupts masked: an interrupt handler, a spin lock held) gets only its own core flushed and its
  // unlinked tables kept, as without a shootdown.
  void set_shootdown(ITlbShootdown* shootdown) { __atomic_store_n(&shootdown_, shootdown, __ATOMIC_RELEASE); }

  // Looks at the CPU: false for 5-level paging, which is not supported. Call it once, before the other members.
  bool initialize();

  // Programs the page attribute table so that write-combining can be used (entry 4: PAT bit set, PCD and PWT clear). Each core has its own PAT register:
  // call load_pat() on every other core as well. False if the CPU has no PAT. Safe from several cores at once (the first one sets it up).
  bool enable_write_combining();
  void load_pat();
  bool write_combining_enabled() const;

  // Maps [address, address + bytes) (widened to whole pages) 1:1 with these attributes, replacing whatever mapping was there. False if memory for
  // tables ran out (part of the range may be done), the range is beyond 128 TiB, the cache mode cannot be had, or the shootdown failed (the tables are
  // changed, but some core may still use its old translation). Flushes this core's TLB, or every core's with a shootdown.
  bool map(uint64_t address, uint64_t bytes, const PageAttributes& attributes);
  // The same, but pages that are already mapped keep their mapping and attributes: for making sure something is reachable without changing what the firmware set up.
  bool map_missing(uint64_t address, uint64_t bytes, const PageAttributes& attributes);
  // Removes the mapping of the pages in the range.
  bool unmap(uint64_t address, uint64_t bytes);

  // The mapping of the page that contains `address`.
  bool query(uint64_t address, PageMapping* out) const;
  // Is every page of the range mapped 1:1?
  bool is_mapped(uint64_t address, uint64_t bytes) const;

 private:
  enum class Mode : uint8_t { Replace, Missing, Remove };
  enum class Edit : uint8_t { Done, Raced, NoMemory };   // Raced: the entry changed before the compare-and-swap; read it again
  struct RetiredTable;
  struct Change {                       // what one map/unmap did
    bool changed = false;               // some entry changed: flush
    bool unlinked = false;              // a table was unlinked: flush everything (the CPUs cache tables, too)
    RetiredTable* retired = nullptr;    // the unlinked tables, to free after the shootdown
  };
  bool apply(uint64_t address, uint64_t bytes, const PageAttributes& attributes, Mode mode);
  bool walk(uint64_t* table, int level, uint64_t table_base, uint64_t start, uint64_t end, const PageAttributes& attributes, Mode mode, Change& change);
  uint64_t* new_table();
  Edit install_table(uint64_t* entry, uint64_t seen, Change& change);
  Edit split(uint64_t* entry, uint64_t seen, int level, Change& change);
  void unlinked(uint64_t entry, int level, Change& change);
  void free_subtree(uint64_t physical, int level, bool owned);
  static void free_retired(RcuNode* node);
  static void drop(RetiredTable* table);
  uint64_t pat() const { return __atomic_load_n(&pat_value_, __ATOMIC_ACQUIRE); }
  uint64_t leaf_entry(uint64_t address, int level, const PageAttributes& attributes) const;
  bool pat_index_for(CacheMode mode, uint32_t* index) const;
  CacheMode mode_of(uint32_t pat_index) const;

  IPagingHost& host_;
  allocators::IAllocator& allocator_;
  RcuDomain& rcu_;
  ITlbShootdown* shootdown_ = nullptr;
  bool nx_ = false, gigabyte_ = false, pat_ = false, ready_ = false;   // (set by initialize(), read-only after)
  uint64_t pat_value_ = 0x0007040600070406ull;   // the power-on contents: WB, WT, UC-, UC, WB, WT, UC-, UC; changed only by compare-and-swap
};

}  // namespace kernel
