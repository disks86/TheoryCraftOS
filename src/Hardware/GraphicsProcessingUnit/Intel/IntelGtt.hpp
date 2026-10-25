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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {

// Graphics address translation of Intel GPUs from Broadwell on (Gen8 to Gen11 layout): the global GTT (GGTT), which lives in a table inside the register BAR, and
// the per-context page tables (PPGTT). Layouts follow the MIT-licensed i915 driver (gt/intel_ggtt.c, gt/gen8_ppgtt.c, gt/intel_gtt.h).

constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kEntriesPerPage = 512;

constexpr uint64_t kPtePresent = 1ull << 0;
constexpr uint64_t kPteWritable = 1ull << 1;
constexpr uint64_t kPteWriteThrough = 1ull << 3;   // PWT
constexpr uint64_t kPteCacheDisable = 1ull << 4;   // PCD
constexpr uint64_t kPtePat = 1ull << 7;            // PAT

enum class Caching : uint8_t {
  Uncached,  // not cached by the GPU's caches
  Cached,    // write-back, snooped: the CPU and the GPU see each other's writes without flushes (needs an LLC)
};

// Entries of the page tables a context walks: 4 KiB pages, a page directory entry per 2 MiB, a page table entry per page.
constexpr uint64_t ppgtt_pte(uint64_t dma_address, Caching caching) {
  return (dma_address & ~(kPageSize - 1)) | kPtePresent | kPteWritable | (caching == Caching::Cached ? kPtePat : (kPteWriteThrough | kPteCacheDisable));
}
constexpr uint64_t ppgtt_pde(uint64_t dma_address, Caching caching) {
  return (dma_address & ~(kPageSize - 1)) | kPtePresent | kPteWritable | (caching == Caching::Cached ? 0 : (kPteWriteThrough | kPteCacheDisable));
}
// A global GTT entry: just the address and the present bit.
constexpr uint64_t ggtt_pte(uint64_t dma_address) { return (dma_address & ~(kPageSize - 1)) | kPtePresent; }

// Where the global GTT is and how big, from the GMCH control word (PCI configuration offset 0x50 of the GPU, bits 7:6 give the size: 0 none, 1 = 2 MiB,
// 2 = 4 MiB, 3 = 8 MiB) and the size of the register BAR (the table starts halfway: 8 MiB into a 16 MiB BAR).
struct GgttGeometry {
  uint64_t table_offset = 0;  // from the start of the register BAR
  uint64_t table_bytes = 0;
  uint64_t entries = 0;       // 8 bytes each
  uint64_t address_space = 0; // bytes of graphics address space the table maps
};
bool ggtt_geometry(uint16_t gmch_control, uint64_t register_bar_size, GgttGeometry* out);

// The global GTT table, seen through its window. The driver may only change the part it owns (so the entries the firmware set up for the display stay as
// they are): pages [first_page, first_page + page_count) of the graphics address space.
class Ggtt {
 public:
  Ggtt(const MmioRegion& table, uint64_t first_page, uint64_t page_count) : table_(table), first_page_(first_page), page_count_(page_count) {}

  // True when the owned range lies inside the table the window covers.
  bool valid() const { return table_.valid() && page_count_ != 0 && first_page_ <= entries() && page_count_ <= entries() - first_page_; }
  uint64_t entries() const { return table_.length() / 8; }
  // Graphics address of page `index` of the owned range.
  uint64_t address_of(uint64_t index) const { return (first_page_ + index) * kPageSize; }
  uint64_t page_count() const { return page_count_; }

  // Points page `index` of the owned range at `dma_address` (4 KiB aligned). False when the index is outside the range.
  bool map(uint64_t index, uint64_t dma_address) const;
  // Points every page of `[index, index + count)` at `scratch_dma` (nothing the GPU does there can then fault or reach memory it should not).
  bool clear(uint64_t index, uint64_t count, uint64_t scratch_dma) const;
  // Writes an entry as it is (to put back what was there). False when the index is outside the range.
  bool write_raw(uint64_t index, uint64_t entry) const;
  // The entry of page `index`, for checks.
  bool read(uint64_t index, uint64_t* entry) const;

 private:
  MmioRegion table_;
  uint64_t first_page_, page_count_;
};

// The table that says what each of the 8 page attribute indices (PAT) means. The page tables above select index 0 for GGTT entries (the hardware always uses 0
// there) and index 4 (cached) or 3 (uncached) for PPGTT entries. This is the Gen9 (not Gen9 LP) table of the i915 driver: index 0 write-back LLC, 1 write-combining, 2
// write-back, 3 uncached, 4-7 write-back LLC+eLLC with ages 0-3. Other generations have other tables.
struct PatTable {
  uint32_t low, high;
};
constexpr PatTable gen9_private_pat() { return {0x00030907u, 0x3B2B1B0Bu}; }
constexpr uint64_t kPrivatePatLow = 0x40E0, kPrivatePatHigh = 0x40E4;
void program_private_pat(const MmioRegion& registers, const PatTable& table);
void program_private_pat(IRegisterBank& registers, const PatTable& table);

// Gen12 (Xe-LP) keeps its table in eight registers of its own, one per index. Index 3 is uncached and 4 write-back, which is what the page table entries above select.
// Values as the i915 driver sets them (tgl_setup_private_ppat): index 0 write-back, 1 write-combining, 2 write-through, 3 uncached, 4-7 write-back.
constexpr uint64_t kGen12PatIndexBase = 0x4800;
void program_gen12_pat(IRegisterBank& registers);

// Asks the GPU to drop what it cached of the global GTT; needed after changing entries. `registers` is the register BAR (at least 0x102000 bytes).
constexpr uint64_t kGttFlushRegister = 0x101008;
void ggtt_invalidate(const MmioRegion& registers);
void ggtt_invalidate(IRegisterBank& registers);

// Looks for `count` consecutive global GTT entries nothing is using, from the top of the table down (the firmware sets up the display's mappings from the bottom):
// not present, or all the same value (the scratch page the firmware or a driver points unused entries at). Skips the last `guard` entries. False if there is no such place.
bool find_unused_ggtt_range(const MmioRegion& table, uint64_t count, uint64_t guard, uint64_t* first_page);

// Memory for page tables: 4 KiB, zeroed, with the address the GPU uses to reach it.
class IGpuPageSource {
 public:
  virtual bool allocate_page(ArrayView<uint64_t>* entries, uint64_t* dma_address) = 0;
  virtual void free_page(ArrayView<uint64_t> entries) = 0;

 protected:
  constexpr IGpuPageSource() = default;
  ~IGpuPageSource() = default;
};

// A context's address space of 32 bits (the "legacy" mode of Gen8-Gen11 contexts): four page directories of 512 entries, each entry a page table of 512
// pages. Everything not mapped points at one scratch page, so a stray access (or the prefetching the hardware does) reads zeros instead of faulting.
class Ppgtt32 {
 public:
  explicit Ppgtt32(IGpuPageSource& pages) : pages_(pages) {}
  ~Ppgtt32() { release(); }
  Ppgtt32(const Ppgtt32&) = delete;
  Ppgtt32& operator=(const Ppgtt32&) = delete;

  // `four_level`: the 48-bit form of Gen12 contexts, in which a page-map level 4 page (whose address goes into the context image as its first page directory
  // pointer) points at one page of directory pointers that holds these four directories; everything else in the two levels points at scratch pages.
  bool initialize(bool four_level = false);
  // Gives the pages back (only when the GPU can no longer reach them), or - with abandon() - forgets them without freeing, for a GPU that may still be using them.
  void shutdown() { release(); }
  void abandon();
  // Maps the page at graphics address `gpu_address` (4 KiB aligned, below 4 GiB) to `dma_address`. False when out of range or out of memory.
  bool map(uint64_t gpu_address, uint64_t dma_address);
  // Points the page back at the scratch page (a no-op if its table does not exist). False for an address that is out of range or unaligned.
  bool unmap(uint64_t gpu_address);
  // The address of page directory `n` (0-3), which goes into the context image.
  uint64_t page_directory_dma(uint32_t n) const { return n < 4 ? directories_dma_[n] : 0; }
  // The top of a four-level space (0 for the 32-bit form).
  uint64_t root_dma() const { return pml4_dma_; }
  // What the hardware would find: the page a graphics address is mapped to (or the scratch page). False for addresses of 4 GiB and more.
  bool translate(uint64_t gpu_address, uint64_t* dma_address) const;
  uint64_t scratch_dma() const { return scratch_dma_; }

 private:
  static constexpr uint32_t kMaxTables = 4 * kEntriesPerPage;
  void release();

  IGpuPageSource& pages_;
  bool ready_ = false;
  ArrayView<uint64_t> scratch_page_;
  uint64_t scratch_dma_ = 0;
  ArrayView<uint64_t> scratch_table_;
  uint64_t scratch_table_dma_ = 0;
  ArrayView<uint64_t> directories_[4];
  uint64_t directories_dma_[4] = {};
  ArrayView<uint64_t> pml4_, pdpt_, scratch_pdpt_, scratch_pd_;
  uint64_t pml4_dma_ = 0, pdpt_dma_ = 0, scratch_pdpt_dma_ = 0, scratch_pd_dma_ = 0;
  ArrayView<uint64_t> tables_[kMaxTables];
  uint64_t tables_dma_[kMaxTables] = {};
};

}  // namespace intel_gt
}  // namespace hardware
