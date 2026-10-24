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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelGtt.hpp"

namespace hardware {
namespace intel_gt {

bool ggtt_geometry(uint16_t gmch_control, uint64_t register_bar_size, GgttGeometry* out) {
  const uint32_t size_code = (gmch_control >> 6) & 0x3;
  if (size_code == 0 || register_bar_size == 0 || (register_bar_size & 1)) return false;
  GgttGeometry g;
  g.table_bytes = (uint64_t{1} << size_code) << 20;
  g.table_offset = register_bar_size / 2;
  if (g.table_bytes > register_bar_size - g.table_offset) return false;
  g.entries = g.table_bytes / 8;
  g.address_space = g.entries * kPageSize;
  *out = g;
  return true;
}

bool Ggtt::map(uint64_t index, uint64_t dma_address) const {
  if (!valid() || index >= page_count_ || (dma_address & (kPageSize - 1))) return false;
  table_.write64((first_page_ + index) * 8, ggtt_pte(dma_address));
  return true;
}

bool Ggtt::clear(uint64_t index, uint64_t count, uint64_t scratch_dma) const {
  if (!valid() || index > page_count_ || count > page_count_ - index || (scratch_dma & (kPageSize - 1))) return false;
  for (uint64_t i = 0; i < count; ++i) table_.write64((first_page_ + index + i) * 8, ggtt_pte(scratch_dma));
  return true;
}

bool Ggtt::write_raw(uint64_t index, uint64_t entry) const {
  if (!valid() || index >= page_count_) return false;
  table_.write64((first_page_ + index) * 8, entry);
  return true;
}

bool Ggtt::read(uint64_t index, uint64_t* entry) const {
  if (!valid() || index >= page_count_) return false;
  *entry = table_.read64((first_page_ + index) * 8);
  return true;
}

bool find_unused_ggtt_range(const MmioRegion& table, uint64_t count, uint64_t guard, uint64_t* first_page) {
  const uint64_t entries = table.length() / 8;
  if (!table.valid() || count == 0 || guard > entries || count > entries - guard) return false;
  for (uint64_t first = entries - guard - count;; --first) {
    bool absent = true, same = true;
    const uint64_t value = table.read64(first * 8);
    for (uint64_t i = 0; i < count && (absent || same); ++i) {
      const uint64_t e = table.read64((first + i) * 8);
      absent = absent && !(e & kPtePresent);
      same = same && e == value;
    }
    if (absent || same) {
      *first_page = first;
      return true;
    }
    if (first == 0) return false;
  }
}

void program_private_pat(const MmioRegion& registers, const PatTable& table) {
  registers.write32(kPrivatePatLow, table.low);
  registers.write32(kPrivatePatHigh, table.high);
}

void program_private_pat(IRegisterBank& registers, const PatTable& table) {
  registers.write32(kPrivatePatLow, table.low);
  registers.write32(kPrivatePatHigh, table.high);
}

void ggtt_invalidate(IRegisterBank& registers) {
  registers.write32(kGttFlushRegister, 1);
  registers.read32(kGttFlushRegister);
}

void ggtt_invalidate(const MmioRegion& registers) {
  registers.write32(kGttFlushRegister, 1);  // GFX_FLSH_CNTL_EN
  registers.read32(kGttFlushRegister);      // posting read
}

void Ppgtt32::release() {
  for (ArrayView<uint64_t>& t : tables_)
    if (!t.empty()) { pages_.free_page(t); t = ArrayView<uint64_t>(); }
  for (ArrayView<uint64_t>& d : directories_)
    if (!d.empty()) { pages_.free_page(d); d = ArrayView<uint64_t>(); }
  if (!scratch_table_.empty()) { pages_.free_page(scratch_table_); scratch_table_ = ArrayView<uint64_t>(); }
  if (!scratch_page_.empty()) { pages_.free_page(scratch_page_); scratch_page_ = ArrayView<uint64_t>(); }
  ready_ = false;
}

void Ppgtt32::abandon() {
  for (ArrayView<uint64_t>& t : tables_) t = ArrayView<uint64_t>();
  for (ArrayView<uint64_t>& d : directories_) d = ArrayView<uint64_t>();
  scratch_table_ = ArrayView<uint64_t>();
  scratch_page_ = ArrayView<uint64_t>();
  ready_ = false;
}

bool Ppgtt32::initialize() {
  release();
  if (!pages_.allocate_page(&scratch_page_, &scratch_dma_) || !pages_.allocate_page(&scratch_table_, &scratch_table_dma_)) { release(); return false; }
  for (uint64_t& e : scratch_table_) e = ppgtt_pte(scratch_dma_, Caching::Cached);
  for (uint32_t i = 0; i < 4; ++i) {
    if (!pages_.allocate_page(&directories_[i], &directories_dma_[i])) { release(); return false; }
    for (uint64_t& e : directories_[i]) e = ppgtt_pde(scratch_table_dma_, Caching::Cached);
  }
  ready_ = true;
  return true;
}

bool Ppgtt32::map(uint64_t gpu_address, uint64_t dma_address) {
  if (!ready_ || gpu_address >= (uint64_t{1} << 32) || (gpu_address & (kPageSize - 1)) || (dma_address & (kPageSize - 1))) return false;
  const uint32_t dir = static_cast<uint32_t>(gpu_address >> 30);
  const uint32_t pde = static_cast<uint32_t>((gpu_address >> 21) & 511);
  const uint32_t pte = static_cast<uint32_t>((gpu_address >> 12) & 511);
  const uint32_t slot = dir * kEntriesPerPage + pde;
  if (tables_[slot].empty()) {
    if (!pages_.allocate_page(&tables_[slot], &tables_dma_[slot])) return false;
    for (uint64_t& e : tables_[slot]) e = ppgtt_pte(scratch_dma_, Caching::Cached);
    directories_[dir][pde] = ppgtt_pde(tables_dma_[slot], Caching::Cached);
  }
  tables_[slot][pte] = ppgtt_pte(dma_address, Caching::Cached);
  return true;
}

bool Ppgtt32::unmap(uint64_t gpu_address) {
  if (!ready_ || gpu_address >= (uint64_t{1} << 32) || (gpu_address & (kPageSize - 1))) return false;
  const uint32_t slot = static_cast<uint32_t>(gpu_address >> 30) * kEntriesPerPage + static_cast<uint32_t>((gpu_address >> 21) & 511);
  if (!tables_[slot].empty()) tables_[slot][(gpu_address >> 12) & 511] = ppgtt_pte(scratch_dma_, Caching::Cached);
  return true;
}

bool Ppgtt32::translate(uint64_t gpu_address, uint64_t* dma_address) const {
  if (!ready_ || gpu_address >= (uint64_t{1} << 32)) return false;
  const uint32_t dir = static_cast<uint32_t>(gpu_address >> 30);
  const uint32_t pde = static_cast<uint32_t>((gpu_address >> 21) & 511);
  const uint32_t pte = static_cast<uint32_t>((gpu_address >> 12) & 511);
  const uint32_t slot = dir * kEntriesPerPage + pde;
  const uint64_t entry = tables_[slot].empty() ? scratch_table_[pte] : tables_[slot][pte];
  *dma_address = (entry & ~(kPageSize - 1) & ((uint64_t{1} << 48) - 1)) | (gpu_address & (kPageSize - 1));
  return true;
}

}  // namespace intel_gt
}  // namespace hardware
