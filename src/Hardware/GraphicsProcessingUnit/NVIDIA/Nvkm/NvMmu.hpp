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

// The GPU's MMU (nouveau's nvkm/subdev/mmu: vmmnv50.c, vmmgf100.c, vmmgp100.c, vmmgv100.c, vmmtu102.c): the page-table formats, a VMM
// (one GPU virtual address space: its page directory, page tables and a VA allocator) and the TLB invalidate. A VMM is "joined" to an
// instance block (a channel's, or a BAR's), which is how an engine finds its page directory.
//
//   NV50 (Tesla)        2 levels, 40-bit VA: a PD of 2048 entries (512 MiB each) kept inside every joined instance block, then one PT
//                       per PDE, of 4 KiB or of 64 KiB pages (not both).
//   GF100 (Fermi..GM)   2 levels, 40-bit VA: a PD of 8192 entries (128 KiB big pages: 128 MiB each) whose 8-byte entry points at a big
//                       and a small page table at once.
//   GP100 (Pascal..TU)  the "version 2" format, 5 levels, 49-bit VA: PD3 (2 bits), PD2 (9), PD1 (9), PD0 (8 bits, 16-byte dual entries:
//                       big PT then small PT), PT (9 bits of 4 KiB or 5 bits of 64 KiB pages). GV100 and later add 64 subcontext PDBs.
// The bit layouts are in NvMmu.cpp's table, with how sure each one is.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFb.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Utility/Optional.hpp"

namespace hardware {
namespace nv {

enum class VmmFormat : uint8_t { None, Nv50, Gf100, Gp100 };
enum class PageSize : uint8_t { Small, Big };

struct VmmLevel {
  uint8_t shift = 0;        // the lowest VA bit this level indexes with
  uint8_t bits = 0;         // how many
  uint8_t entry_bytes = 8;
  uint64_t table_bytes() const { const uint64_t b = (uint64_t{1} << bits) * entry_bytes; return b < 4096 ? 4096 : b; }
};

struct VmmLayout {
  VmmFormat format = VmmFormat::None;
  uint8_t va_bits = 0;
  uint8_t big_shift = 0;          // 16 (64 KiB) or 17 (128 KiB)
  uint8_t dir_levels = 0;         // directory levels above the page tables (root first)
  VmmLevel dir[4];
  VmmLevel small_pt, big_pt;
  uint32_t nv50_pd_offset = 0;    // NV50: where the PDEs live inside a joined instance block
  bool subcontexts = false;       // GV100+: the PDB is repeated for the 64 subcontexts
  bool pdb_big_64k = false;       // the PDB says "64 KiB big pages" (GM200 with 16, GP100+)
};

// The layout nouveau uses for `chip` (`big_page_shift` 0: the family's default). format None for chips without one here.
VmmLayout vmm_layout(const Chip& chip, uint32_t big_page_shift = 0);

struct MapFlags {
  bool privileged = false;
  bool read_only = false;
  uint8_t kind = 0;               // the storage kind (0: pitch / generic memory)
};

// Entry encodings, exposed so the tests can check them against hand-computed words.
uint64_t encode_pte(VmmFormat format, uint64_t address, MemoryTarget target, const MapFlags& flags);
uint64_t encode_gp100_pde(const GpuMemory& table);                                   // one PDE (or one half of a PD0 dual entry)
uint64_t encode_gf100_pde(const GpuMemory* big_table, const GpuMemory* small_table);
uint64_t encode_nv50_pde(const GpuMemory& table, PageSize page);
uint64_t encode_pdb(const VmmLayout& layout, const GpuMemory& root);                  // the 64-bit word at instance 0x200 (GF100+)

struct NvTimeouts {
  uint32_t register_us = 2000000;   // nvkm_msec(device, 2000, ...)
  uint32_t fence_us = 2000000;      // waiting for an engine's semaphore
};

// The MMU's TLB invalidate (gf100_vmm_invalidate, tu102_vmm_flush): the PDB register pair and the trigger must not be interleaved
// between cores, so it takes its own lock.
class NvTlb {
 public:
  NvTlb(INvRegisters& regs, const Chip& chip, INvLock& lock, NvTimeouts timeouts = {}) : regs_(regs), chip_(chip), lock_(lock), timeouts_(timeouts) {}
  // Invalidates every TLB entry of the page directory `root` (`hub_only`: only the hub's, as for a BAR). False on a timeout.
  [[nodiscard]] bool invalidate(const GpuMemory& root, bool hub_only);
  uint32_t count() const { return count_; }

 private:
  bool poll(uint32_t offset, uint32_t mask, uint32_t expected, bool equal);
  INvRegisters& regs_;
  Chip chip_;
  INvLock& lock_;
  NvTimeouts timeouts_;
  uint32_t count_ = 0;
};

class NvVmm {
 public:
  static constexpr uint32_t kMaxTables = 256;
  static constexpr uint32_t kMaxJoined = 8;     // NV50 only: the instance blocks the PDEs are copied into
  static constexpr uint32_t kMaxVaRanges = 512;

  // Page tables come from `memory` (which must not itself need this VMM to reach VRAM). `tlb` may be null (no flush: before the GPU
  // uses the VMM, or in a test).
  NvVmm(INvMemory& memory, const Chip& chip, INvLock& lock, NvTlb* tlb, uint32_t big_page_shift = 0);
  ~NvVmm();
  NvVmm(const NvVmm&) = delete;
  NvVmm& operator=(const NvVmm&) = delete;

  // Allocates the page directory; the VA allocator covers [va_start, va_limit). `hub` marks a BAR's VMM (hub-only TLB flushes).
  [[nodiscard]] bool init(uint64_t va_start, uint64_t va_limit, bool hub = false);
  [[nodiscard]] Optional<uint64_t> allocate_va(uint64_t bytes, uint64_t alignment);
  [[nodiscard]] bool free_va(uint64_t va);

  // Maps [va, va + bytes) to `memory` from `offset` on (memory must be physically contiguous: a VRAM object or a contiguous DMA
  // buffer). va, offset and bytes must be multiples of the page size. Creates page tables as needed. Does not flush the TLB.
  [[nodiscard]] bool map(uint64_t va, const GpuMemory& memory, uint64_t offset, uint64_t bytes, PageSize page = PageSize::Small,
                         MapFlags flags = {});
  [[nodiscard]] bool unmap(uint64_t va, uint64_t bytes, PageSize page = PageSize::Small);
  // Writes this VMM's page directory into an instance block (0x200 PDB + limit, GV100+ subcontexts, NV50 the PDEs themselves).
  [[nodiscard]] bool join(const GpuMemory& instance);
  [[nodiscard]] bool flush();

  struct Translation {
    uint64_t address = 0;
    MemoryTarget target = MemoryTarget::Vram;
    PageSize page = PageSize::Small;
    uint64_t pte = 0;
  };
  // Walks the tables as written in memory (a check, and for debugging).
  [[nodiscard]] Optional<Translation> translate(uint64_t va);

  const GpuMemory& root() const { return root_; }
  const VmmLayout& layout() const { return layout_; }
  uint64_t limit() const { return va_limit_; }
  uint32_t table_count() const { return table_count_; }
  bool ready() const { return root_.valid(); }

 private:
  struct Table {
    uint8_t level = 0;            // 0 .. dir_levels - 1: a directory; dir_levels: a page table
    PageSize page = PageSize::Small;
    uint64_t base = 0;            // the lowest VA it covers
    GpuMemory memory;
  };
  Table& table_at(uint32_t index) { return ArrayView<Table>(tables_)[index]; }
  uint64_t coverage(uint8_t level) const;   // VA bytes one table of `level` covers
  Optional<uint32_t> find(uint8_t level, PageSize page, uint64_t base) const;
  Optional<uint32_t> find_by_address(uint64_t address) const;
  Optional<uint32_t> table_for(uint8_t level, PageSize page, uint64_t va, bool create);
  bool write_dir_entry(uint8_t level, uint64_t va);
  bool write_pt_pointer(uint64_t va);
  bool write64(const GpuMemory& m, uint64_t offset, uint64_t value);
  bool read64(const GpuMemory& m, uint64_t offset, uint64_t* value);
  bool map_locked(uint64_t va, uint64_t phys, MemoryTarget target, PageSize page, const MapFlags& flags, bool valid);

  INvMemory& memory_;
  Chip chip_;
  INvLock& lock_;
  NvTlb* tlb_;
  VmmLayout layout_;
  bool hub_ = false;
  GpuMemory root_;
  uint64_t va_limit_ = 0;
  Table tables_[kMaxTables];
  uint32_t table_count_ = 0;
  GpuMemory joined_[kMaxJoined];
  uint32_t joined_count_ = 0;
  RangeAllocator<kMaxVaRanges> va_;
};

}  // namespace nv
}  // namespace hardware
