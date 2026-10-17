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
// What it does not do: share changes with other cores (the page tables are shared, but a core that cached a translation keeps it until it flushes:
// make mappings before the other cores start, or flush them yourself), free the tables of pages that were replaced, or work with 5-level paging.
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

 protected:
  IPagingHost() = default;
  ~IPagingHost() = default;
};

// The host for the CPU this code runs on.
IPagingHost& cpu_paging_host();

// The PAT value that enable_write_combining() settled on (0 if write-combining was never set up), and a way for a core that starts later to load it: the
// PAT register belongs to each core, and memory mapped write-combining must mean the same on all of them.
uint64_t shared_pat_value();
void load_shared_pat_on_this_core();

class PageTables {
 public:
  // `allocator` supplies the pages for new tables (4 KiB aligned; they are used by their address, so memory must be mapped 1:1).
  PageTables(IPagingHost& host, allocators::IAllocator& allocator) : host_(host), allocator_(allocator) {}

  // Looks at the CPU: false for 5-level paging, which is not supported.
  bool initialize();

  // Programs the page attribute table so that write-combining can be used (entry 4: PAT bit set, PCD and PWT clear). Each core has its own PAT register:
  // call load_pat() on every other core as well. False if the CPU has no PAT.
  bool enable_write_combining();
  void load_pat();
  bool write_combining_enabled() const { return wc_enabled_; }

  // Maps [address, address + bytes) (widened to whole pages) 1:1 with these attributes, replacing whatever mapping was there. False if memory for
  // tables ran out (part of the range may be done), the range is beyond 128 TiB, or the cache mode cannot be had. Flushes this core's TLB.
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
  bool apply(uint64_t address, uint64_t bytes, const PageAttributes& attributes, Mode mode);
  bool walk(uint64_t* table, int level, uint64_t table_base, uint64_t start, uint64_t end, const PageAttributes& attributes, Mode mode);
  uint64_t* new_table();
  bool split(uint64_t* entry, int level);
  uint64_t leaf_entry(uint64_t address, int level, const PageAttributes& attributes) const;
  bool pat_index_for(CacheMode mode, uint32_t* index) const;
  CacheMode mode_of(uint32_t pat_index) const;

  IPagingHost& host_;
  allocators::IAllocator& allocator_;
  bool nx_ = false, gigabyte_ = false, pat_ = false, wc_enabled_ = false, ready_ = false;
  uint64_t pat_value_ = 0x0007040600070406ull;   // the power-on contents: WB, WT, UC-, UC, WB, WT, UC-, UC
  bool changed_ = false;
};

}  // namespace kernel
