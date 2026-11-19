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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMmu.hpp"

namespace hardware {
namespace nv {

namespace {

// ---- Entry layouts, from memory of nouveau's vmm*.c (no NVIDIA documentation at hand). ----
// Aperture codes: VRAM 0, system coherent ("HOST") 2, system non-coherent ("NCOH") 3.
//
// GP100 PTE   bit 0 valid, 2:1 aperture, 3 volatile, 5 privileged, 6 read-only, 63:56 kind, address >> 4 (bits 53:8 = PA >> 12)   confident
// GP100 PDE   2:1 aperture (VRAM 1, HOST 2, NCOH 3; 0 = no table), 3 volatile (HOST), address >> 4                                  confident
//             PD0 entries are 16 bytes: the big-page table first, the small-page table second                                         fairly sure
// GF100 PTE   bit 0 valid, 1 privileged, 2 read-only, 32 volatile, 34:33 aperture, 43:36 kind, address >> 8                              confident
// GF100 PDE   bits 1:0 big-PT target (VRAM 1, HOST 2, NCOH 3), 35 big volatile, 31:4 big-PT address >> 12;
//             bits 33:32 small-PT target, 34 small volatile, 63:36 small-PT address >> 12 (pt->addr << 24)                              fairly sure (volatile bits unsure)
// NV50 PTE    bit 0 valid, 3 read-only, 5:4 aperture, 6 privileged, 9:7 contiguous-block log2 (0 here), 46:40 kind, 39:12 address       fairly sure
// NV50 PDE    bit 0 present, bit 1 small (4 KiB) pages, 3:2 target (VRAM 0, HOST 2, NCOH 3), 6:5 small-PT size (0 = full 1 MiB), 39:12   unsure
// PDB         instance 0x200: bits 1:0 target (VRAM 0, HOST 2, NCOH 3), 2 volatile, 10 "version 2" (GP100+), 11 64 KiB big pages,
//             63:12 address; 0x208 the VA limit - 1 (gf100_vmm_join_)                                                                fairly sure
// GV100+      0x21c = 0, 0x298/0x29c subcontext valid mask, 0x2a0 + i * 0x10 the PDB of subcontext i (1 when unused), 0x2a8 + i*0x10 0   fairly sure
//
// ---- TLB invalidate (BAR0). ----
// NV50        0x100c80 = engine << 16 | 1, wait for bit 0 to clear; engine 0 GR, 6 BAR (nv50_vmm_flush)                                unsure (engine ids)
// GF100..GV100 wait 0x100c80 & 0x00ff0000 (free slots), 0x100cb8 = (PD >> 12) << 4 | target, (GP100+: 0x100cec = upper 32 bits),
//             0x100cbc = 0x80000000 | type, wait 0x100c80 & 0x00008000. type: 1 PAGE_ALL, 4 HUB_ONLY, GP100+ (5 - depth) << 24     fairly sure
// TU102, GA10x 0xb830a0 = PD >> 8, 0xb830a4 = upper bits, 0x100e68 = 0, 0xb830b0 = 0x80000000 | type, wait bit 31 to clear           fairly sure
constexpr uint32_t kNv50Flush = 0x100c80;
constexpr uint32_t kGfMmuCtrl = 0x100c80, kGfPdb = 0x100cb8, kGfTrigger = 0x100cbc, kGpPdbHi = 0x100cec;
constexpr uint32_t kTuPdbLo = 0xb830a0, kTuPdbHi = 0xb830a4, kTuUnknown = 0x100e68, kTuTrigger = 0xb830b0;

uint64_t aperture(MemoryTarget t) {
  switch (t) {
    case MemoryTarget::Vram: return 0;
    case MemoryTarget::SystemCoherent: return 2;
    case MemoryTarget::SystemNoncoherent: return 3;
  }
  return 0;
}
MemoryTarget target_of(uint64_t code) { return code == 2 ? MemoryTarget::SystemCoherent : code == 3 ? MemoryTarget::SystemNoncoherent : MemoryTarget::Vram; }
// PDE target codes (GF100 and GP100 PDEs use 1 for VRAM, so 0 can mean "no table").
uint64_t pde_target(MemoryTarget t) { return t == MemoryTarget::Vram ? 1 : aperture(t); }

}  // namespace

VmmLayout vmm_layout(const Chip& chip, uint32_t big_page_shift) {
  VmmLayout l;
  switch (chip.family) {
    case Family::Tesla:
      l.format = VmmFormat::Nv50;
      l.va_bits = 40;
      l.big_shift = 16;
      l.dir_levels = 1;
      l.dir[0] = {29, 11, 8};
      l.small_pt = {12, 17, 8};
      l.big_pt = {16, 13, 8};
      l.nv50_pd_offset = chip.chipset == 0x50 ? 0x1400 : 0x200;
      break;
    case Family::Fermi: case Family::Kepler: case Family::Maxwell: {
      const uint8_t big = big_page_shift == 16 ? 16 : 17;
      l.format = VmmFormat::Gf100;
      l.va_bits = 40;
      l.big_shift = big;
      l.dir_levels = 1;
      if (big == 17) { l.dir[0] = {27, 13, 8}; l.small_pt = {12, 15, 8}; l.big_pt = {17, 10, 8}; }
      else { l.dir[0] = {26, 14, 8}; l.small_pt = {12, 14, 8}; l.big_pt = {16, 10, 8}; }
      l.pdb_big_64k = big == 16 && chip.family == Family::Maxwell && chip.chipset >= 0x120;
      break;
    }
    // GA10x: nouveau's ga100_vmm is the GP100 ("v2") format with gv100's join (subcontexts) and tu102's flush; Hopper's v3 is not done.
    case Family::Pascal: case Family::Volta: case Family::Turing: case Family::Ampere:
      l.format = VmmFormat::Gp100;
      l.va_bits = 49;
      l.big_shift = 16;
      l.dir_levels = 4;
      l.dir[0] = {47, 2, 8};
      l.dir[1] = {38, 9, 8};
      l.dir[2] = {29, 9, 8};
      l.dir[3] = {21, 8, 16};
      l.small_pt = {12, 9, 8};
      l.big_pt = {16, 5, 8};
      l.pdb_big_64k = true;
      l.subcontexts = chip.family != Family::Pascal;
      break;
    default: break;
  }
  return l;
}

uint64_t encode_pte(VmmFormat format, uint64_t address, MemoryTarget target, const MapFlags& f) {
  const uint64_t aper = aperture(target), vol = target == MemoryTarget::SystemCoherent ? 1 : 0;
  switch (format) {
    case VmmFormat::Gp100:
      return (address >> 4) | 1 | aper << 1 | vol << 3 | uint64_t(f.privileged) << 5 | uint64_t(f.read_only) << 6 | uint64_t(f.kind) << 56;
    case VmmFormat::Gf100:
      return (address >> 8) | 1 | uint64_t(f.privileged) << 1 | uint64_t(f.read_only) << 2 | vol << 32 | aper << 33 | uint64_t(f.kind) << 36;
    case VmmFormat::Nv50:
      return (address & 0x000000FFFFFFF000ull) | 1 | uint64_t(f.read_only) << 3 | aper << 4 | uint64_t(f.privileged) << 6 |
             uint64_t(f.kind & 0x7F) << 40;
    default: return 0;
  }
}

uint64_t encode_gp100_pde(const GpuMemory& table) {
  uint64_t d = table.gpu_address >> 4;
  d |= pde_target(table.target) << 1;
  if (table.target == MemoryTarget::SystemCoherent) d |= 1ull << 3;
  return d;
}

uint64_t encode_gf100_pde(const GpuMemory* big, const GpuMemory* small) {
  uint64_t d = 0;
  if (big) {
    d |= pde_target(big->target);
    if (big->target == MemoryTarget::SystemCoherent) d |= 1ull << 35;
    d |= big->gpu_address >> 8;
  }
  if (small) {
    d |= pde_target(small->target) << 32;
    if (small->target == MemoryTarget::SystemCoherent) d |= 1ull << 34;
    d |= small->gpu_address << 24;
  }
  return d;
}

uint64_t encode_nv50_pde(const GpuMemory& table, PageSize page) {
  uint64_t d = page == PageSize::Big ? 0x1 : 0x3;   // small: 1 MiB table, size field 0
  d |= aperture(table.target) << 2;
  d |= table.gpu_address & 0x000000FFFFFFF000ull;
  return d;
}

uint64_t encode_pdb(const VmmLayout& layout, const GpuMemory& root) {
  uint64_t base = aperture(root.target);
  if (root.target == MemoryTarget::SystemCoherent) base |= 1ull << 2;
  if (layout.format == VmmFormat::Gp100) base |= 1ull << 10;
  if (layout.pdb_big_64k) base |= 1ull << 11;
  return base | (root.gpu_address & ~uint64_t{0xFFF});
}

// ---- NvTlb ----

bool NvTlb::poll(uint32_t offset, uint32_t mask, uint32_t expected, bool equal) {
  for (uint32_t i = 0; i <= timeouts_.register_us; ++i) {
    const uint32_t v = regs_.read32(offset) & mask;
    if (equal ? v == expected : v != 0) return true;
    regs_.delay_us(1);
  }
  return false;
}

bool NvTlb::invalidate(const GpuMemory& root, bool hub_only) {
  if (!root.valid()) return false;
  NvLockGuard guard(lock_);
  ++count_;
  const uint32_t target = static_cast<uint32_t>(aperture(root.target));
  switch (chip_.family) {
    case Family::Tesla:
      regs_.write32(kNv50Flush, (hub_only ? 6u : 0u) << 16 | 1);
      return poll(kNv50Flush, 1, 0, true);
    case Family::Fermi: case Family::Kepler: case Family::Maxwell: case Family::Pascal: case Family::Volta: {
      if (!poll(kGfMmuCtrl, 0x00FF0000, 0, false)) return false;
      const uint64_t addr = ((root.gpu_address >> 12) << 4) | target;
      regs_.write32(kGfPdb, static_cast<uint32_t>(addr));
      uint32_t type = 1 | (hub_only ? 4 : 0);
      if (chip_.family >= Family::Pascal) {
        regs_.write32(kGpPdbHi, static_cast<uint32_t>(addr >> 32));
        type |= 5u << 24;
      }
      regs_.write32(kGfTrigger, 0x80000000u | type);
      return poll(kGfMmuCtrl, 0x00008000, 0, false);
    }
    case Family::Turing: case Family::Ampere: {   // (ga100_vmm.flush = tu102_vmm_flush)
      const uint64_t addr = root.gpu_address >> 8;
      regs_.write32(kTuPdbLo, static_cast<uint32_t>(addr));
      regs_.write32(kTuPdbHi, static_cast<uint32_t>(addr >> 32));
      regs_.write32(kTuUnknown, 0);
      regs_.write32(kTuTrigger, 0x80000000u | 5u << 24 | 1 | (hub_only ? 4 : 0));
      return poll(kTuTrigger, 0x80000000u, 0, true);
    }
    default: return false;
  }
}

// ---- NvVmm ----

NvVmm::NvVmm(INvMemory& memory, const Chip& chip, INvLock& lock, NvTlb* tlb, uint32_t big_page_shift)
    : memory_(memory), chip_(chip), lock_(lock), tlb_(tlb), layout_(vmm_layout(chip, big_page_shift)) {}

NvVmm::~NvVmm() {
  for (Table& t : ArrayView<Table>(tables_).first(table_count_)) memory_.release(t.memory);
}

uint64_t NvVmm::coverage(uint8_t level) const {
  if (level == 0) return uint64_t{1} << layout_.va_bits;
  return uint64_t{1} << layout_.dir[level - 1].shift;
}

Optional<uint32_t> NvVmm::find(uint8_t level, PageSize page, uint64_t base) const {
  const ArrayView<const Table> tables = ArrayView<const Table>(tables_).first(table_count_);
  for (uint32_t i = 0; i < tables.size(); ++i) {
    const Table& t = tables[i];
    if (t.level == level && t.base == base && (level < layout_.dir_levels || t.page == page)) return i;
  }
  return {};
}

Optional<uint32_t> NvVmm::find_by_address(uint64_t address) const {
  const ArrayView<const Table> tables = ArrayView<const Table>(tables_).first(table_count_);
  for (uint32_t i = 0; i < tables.size(); ++i)
    if (tables[i].memory.gpu_address == address) return i;
  return {};
}

bool NvVmm::write64(const GpuMemory& m, uint64_t offset, uint64_t value) {
  // The valid/present bits are in the low word: make the entry valid last, invalid first.
  const uint32_t lo = static_cast<uint32_t>(value), hi = static_cast<uint32_t>(value >> 32);
  if (lo & 1) return memory_.write32(m, offset + 4, hi) && memory_.write32(m, offset, lo);
  return memory_.write32(m, offset, lo) && memory_.write32(m, offset + 4, hi);
}

bool NvVmm::read64(const GpuMemory& m, uint64_t offset, uint64_t* value) {
  uint32_t lo = 0, hi = 0;
  if (!memory_.read32(m, offset, &lo) || !memory_.read32(m, offset + 4, &hi)) return false;
  *value = uint64_t(hi) << 32 | lo;
  return true;
}

bool NvVmm::init(uint64_t va_start, uint64_t va_limit, bool hub) {
  NvLockGuard guard(lock_);
  if (layout_.format == VmmFormat::None || root_.valid()) return false;
  const uint64_t max = uint64_t{1} << layout_.va_bits;
  if (va_limit > max || va_start >= va_limit) return false;
  root_ = memory_.allocate(MemoryTarget::Vram, layout_.dir[0].table_bytes(), 4096);
  if (!root_.valid()) return false;
  Table& t = ArrayView<Table>(tables_)[0];
  t.level = 0;
  t.base = 0;
  t.memory = root_;
  table_count_ = 1;
  va_limit_ = va_limit;
  hub_ = hub;
  return va_.reset(va_start, va_limit - va_start);
}

Optional<uint64_t> NvVmm::allocate_va(uint64_t bytes, uint64_t alignment) {
  NvLockGuard guard(lock_);
  if (!root_.valid()) return {};
  if (alignment < 4096) alignment = 4096;
  if (bytes > ~uint64_t{0} - 4095) return {};
  return va_.allocate((bytes + 4095) & ~uint64_t{4095}, alignment);
}

bool NvVmm::free_va(uint64_t va) {
  NvLockGuard guard(lock_);
  return va_.release(va);
}

bool NvVmm::write_dir_entry(uint8_t level, uint64_t va) {
  const Optional<uint32_t> parent = find(level, PageSize::Small, va & ~(coverage(level) - 1));
  const Optional<uint32_t> child = find(level + 1, PageSize::Small, va & ~(coverage(level + 1) - 1));
  if (!parent || !child) return false;
  const VmmLevel& lv = layout_.dir[level];
  const uint64_t index = (va >> lv.shift) & ((uint64_t{1} << lv.bits) - 1);
  return write64(table_at(*parent).memory, index * lv.entry_bytes, encode_gp100_pde(table_at(*child).memory));
}

bool NvVmm::write_pt_pointer(uint64_t va) {
  const uint8_t d = layout_.dir_levels - 1;
  const Optional<uint32_t> parent = find(d, PageSize::Small, va & ~(coverage(d) - 1));
  if (!parent) return false;
  const uint64_t pt_base = va & ~(coverage(d + 1) - 1);
  const Optional<uint32_t> big = find(d + 1, PageSize::Big, pt_base), small = find(d + 1, PageSize::Small, pt_base);
  const VmmLevel& lv = layout_.dir[d];
  const uint64_t index = (va >> lv.shift) & ((uint64_t{1} << lv.bits) - 1);
  const GpuMemory& pd = table_at(*parent).memory;
  switch (layout_.format) {
    case VmmFormat::Gp100:
      return write64(pd, index * 16, big ? encode_gp100_pde(table_at(*big).memory) : 0) &&
             write64(pd, index * 16 + 8, small ? encode_gp100_pde(table_at(*small).memory) : 0);
    case VmmFormat::Gf100:
      return write64(pd, index * 8, encode_gf100_pde(big ? &table_at(*big).memory : nullptr, small ? &table_at(*small).memory : nullptr));
    case VmmFormat::Nv50: {
      if (big && small) return false;
      const uint64_t value = big ? encode_nv50_pde(table_at(*big).memory, PageSize::Big)
                                 : small ? encode_nv50_pde(table_at(*small).memory, PageSize::Small) : 0;
      if (!write64(pd, index * 8, value)) return false;
      for (const GpuMemory& inst : ArrayView<const GpuMemory>(joined_).first(joined_count_))
        if (!write64(inst, layout_.nv50_pd_offset + index * 8, value)) return false;
      return true;
    }
    default: return false;
  }
}

Optional<uint32_t> NvVmm::table_for(uint8_t level, PageSize page, uint64_t va, bool create) {
  const uint64_t base = va & ~(coverage(level) - 1);
  if (const Optional<uint32_t> found = find(level, page, base)) return found;
  if (!create || level == 0) return {};
  if (!table_for(level - 1, PageSize::Small, va, true)) return {};
  const bool pt = level == layout_.dir_levels;
  if (pt && layout_.format == VmmFormat::Nv50 && find(level, page == PageSize::Big ? PageSize::Small : PageSize::Big, base))
    return {};  // an NV50 PDE holds one page size only
  if (table_count_ >= kMaxTables) return {};
  const VmmLevel& lv = pt ? (page == PageSize::Big ? layout_.big_pt : layout_.small_pt) : layout_.dir[level];
  GpuMemory m = memory_.allocate(MemoryTarget::Vram, lv.table_bytes(), 4096);
  if (!m.valid()) return {};
  const uint32_t index = table_count_++;
  Table& t = ArrayView<Table>(tables_)[index];
  t.level = level;
  t.page = pt ? page : PageSize::Small;
  t.base = base;
  t.memory = m;
  const bool linked = pt ? write_pt_pointer(va) : write_dir_entry(level - 1, va);
  if (!linked) {
    memory_.release(t.memory);
    --table_count_;
    return {};
  }
  return index;
}

bool NvVmm::map_locked(uint64_t va, uint64_t phys, MemoryTarget target, PageSize page, const MapFlags& flags, bool valid) {
  const Optional<uint32_t> pt = table_for(layout_.dir_levels, page, va, valid);
  if (!pt) return !valid;  // unmapping a page that never had a table is a no-op
  const VmmLevel& lv = page == PageSize::Big ? layout_.big_pt : layout_.small_pt;
  const uint64_t index = (va >> lv.shift) & ((uint64_t{1} << lv.bits) - 1);
  return write64(table_at(*pt).memory, index * 8, valid ? encode_pte(layout_.format, phys, target, flags) : 0);
}

bool NvVmm::map(uint64_t va, const GpuMemory& memory, uint64_t offset, uint64_t bytes, PageSize page, MapFlags flags) {
  NvLockGuard guard(lock_);
  if (!root_.valid() || !memory.valid() || bytes == 0) return false;
  const uint64_t size = uint64_t{1} << (page == PageSize::Big ? layout_.big_shift : 12);
  if (va % size || offset % size || bytes % size || memory.gpu_address % size) return false;
  if (offset > memory.bytes || bytes > memory.bytes - offset) return false;
  if (va > va_limit_ || bytes > va_limit_ - va) return false;
  for (uint64_t at = 0; at < bytes; at += size)
    if (!map_locked(va + at, memory.gpu_address + offset + at, memory.target, page, flags, true)) return false;
  return true;
}

bool NvVmm::unmap(uint64_t va, uint64_t bytes, PageSize page) {
  NvLockGuard guard(lock_);
  if (!root_.valid()) return false;
  const uint64_t size = uint64_t{1} << (page == PageSize::Big ? layout_.big_shift : 12);
  if (va % size || bytes % size || va > va_limit_ || bytes > va_limit_ - va) return false;
  for (uint64_t at = 0; at < bytes; at += size)
    if (!map_locked(va + at, 0, MemoryTarget::Vram, page, MapFlags{}, false)) return false;
  return true;
}

bool NvVmm::join(const GpuMemory& inst) {
  NvLockGuard guard(lock_);
  if (!root_.valid() || !inst.valid()) return false;
  if (layout_.format == VmmFormat::Nv50) {
    if (joined_count_ >= kMaxJoined) return false;
    ArrayView<GpuMemory>{joined_}[joined_count_++] = inst;
    // Copy every PDE written so far (the PD itself is the master copy).
    const VmmLevel& lv = layout_.dir[0];
    for (uint64_t index = 0; index < (uint64_t{1} << lv.bits); ++index) {
      uint64_t value = 0;
      if (!read64(root_, index * 8, &value)) return false;
      if (value && !write64(inst, layout_.nv50_pd_offset + index * 8, value)) return false;
    }
    return true;
  }
  if (!write64(inst, 0x200, encode_pdb(layout_, root_)) || !write64(inst, 0x208, va_limit_ - 1)) return false;
  if (!layout_.subcontexts) return true;
  uint32_t lo = 0, hi = 0;
  if (!memory_.read32(inst, 0x200, &lo) || !memory_.read32(inst, 0x204, &hi)) return false;
  if (!memory_.write32(inst, 0x21c, 0)) return false;
  for (uint32_t i = 0; i < 64; ++i) {
    const bool used = i == 0;  // subcontext 0 only (mask bit 0)
    if (!memory_.write32(inst, 0x2a4 + i * 0x10, used ? hi : 1) || !memory_.write32(inst, 0x2a0 + i * 0x10, used ? lo : 1) ||
        !memory_.write32(inst, 0x2a8 + i * 0x10, 0))
      return false;
  }
  return memory_.write32(inst, 0x298, 1) && memory_.write32(inst, 0x29c, 0);
}

bool NvVmm::flush() {
  if (!tlb_) return true;
  memory_.flush(root_);
  return tlb_->invalidate(root_, hub_);
}

Optional<NvVmm::Translation> NvVmm::translate(uint64_t va) {
  NvLockGuard guard(lock_);
  if (!root_.valid() || va >= (uint64_t{1} << layout_.va_bits)) return {};
  uint32_t table = 0;  // the root
  // Directory levels above the last one (GP100 only): follow single PDEs.
  for (uint8_t level = 0; level + 1 < layout_.dir_levels; ++level) {
    const VmmLevel& lv = layout_.dir[level];
    uint64_t pde = 0;
    if (!read64(table_at(table).memory, ((va >> lv.shift) & ((uint64_t{1} << lv.bits) - 1)) * lv.entry_bytes, &pde)) return {};
    if (((pde >> 1) & 3) == 0) return {};
    const Optional<uint32_t> next = find_by_address(((pde >> 8) & ((uint64_t{1} << 46) - 1)) << 12);
    if (!next) return {};
    table = *next;
  }
  const VmmLevel& lv = layout_.dir[layout_.dir_levels - 1];
  const uint64_t index = (va >> lv.shift) & ((uint64_t{1} << lv.bits) - 1);
  struct Candidate { uint64_t address; PageSize page; bool present; };
  Candidate candidates[2] = {};
  switch (layout_.format) {
    case VmmFormat::Gp100: {
      uint64_t big = 0, small = 0;
      if (!read64(table_at(table).memory, index * 16, &big) || !read64(table_at(table).memory, index * 16 + 8, &small)) return {};
      candidates[0] = {((small >> 8) & ((uint64_t{1} << 46) - 1)) << 12, PageSize::Small, ((small >> 1) & 3) != 0};
      candidates[1] = {((big >> 8) & ((uint64_t{1} << 46) - 1)) << 12, PageSize::Big, ((big >> 1) & 3) != 0};
      break;
    }
    case VmmFormat::Gf100: {
      uint64_t pde = 0;
      if (!read64(table_at(table).memory, index * 8, &pde)) return {};
      const uint32_t lo = static_cast<uint32_t>(pde), hi = static_cast<uint32_t>(pde >> 32);
      candidates[0] = {uint64_t(hi & ~0xFu) << 8, PageSize::Small, (hi & 3) != 0};
      candidates[1] = {uint64_t(lo & ~0xFu) << 8, PageSize::Big, (lo & 3) != 0};
      break;
    }
    case VmmFormat::Nv50: {
      uint64_t pde = 0;
      if (!read64(table_at(table).memory, index * 8, &pde)) return {};
      candidates[0] = {pde & 0x000000FFFFFFF000ull, (pde & 2) ? PageSize::Small : PageSize::Big, (pde & 1) != 0};
      break;
    }
    default: return {};
  }
  for (const Candidate& c : candidates) {
    if (!c.present) continue;
    const Optional<uint32_t> pt = find_by_address(c.address);
    if (!pt) continue;
    const VmmLevel& plv = c.page == PageSize::Big ? layout_.big_pt : layout_.small_pt;
    uint64_t pte = 0;
    if (!read64(table_at(*pt).memory, ((va >> plv.shift) & ((uint64_t{1} << plv.bits) - 1)) * 8, &pte)) return {};
    if (!(pte & 1)) continue;
    Translation t;
    t.page = c.page;
    t.pte = pte;
    const uint64_t in_page = va & ((uint64_t{1} << plv.shift) - 1);
    switch (layout_.format) {
      case VmmFormat::Gp100: t.address = (((pte >> 8) & ((uint64_t{1} << 46) - 1)) << 12) + in_page; t.target = target_of((pte >> 1) & 3); break;
      case VmmFormat::Gf100: t.address = ((pte & 0xFFFFFFF0ull) << 8) + in_page; t.target = target_of((pte >> 33) & 3); break;
      default: t.address = (pte & 0x000000FFFFFFF000ull) + in_page; t.target = target_of((pte >> 4) & 3); break;
    }
    return t;
  }
  return {};
}

}  // namespace nv
}  // namespace hardware
