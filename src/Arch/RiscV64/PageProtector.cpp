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
#include "Arch/RiscV64/PageProtector.hpp"

namespace arch_riscv64 {

namespace {
constexpr uint64_t kV = 1 << 0, kR = 1 << 1, kW = 1 << 2, kX = 1 << 3, kA = 1 << 6, kD = 1 << 7;
constexpr uint64_t kNapot = uint64_t{1} << 63;
constexpr uint64_t kPpnMask = 0x003FFFFFFFFFFC00ull;   // bits 10..53
constexpr uint64_t kPage = 4096;

bool is_leaf(uint64_t entry) { return (entry & (kR | kW | kX)) != 0; }
// The page tables are reached by their physical address, which is also the pointer (identity mapped): the one place that converts.
uint64_t* table_at(uint64_t entry) { return reinterpret_cast<uint64_t*>((entry & kPpnMask) << 2); }
uint64_t table_entry(const uint64_t* table) { return ((reinterpret_cast<uint64_t>(table) >> 12) << 10) | kV; }
uint64_t load(const uint64_t* entry) { return __atomic_load_n(entry, __ATOMIC_ACQUIRE); }
void store(uint64_t* entry, uint64_t value) { __atomic_store_n(entry, value, __ATOMIC_RELEASE); }
}  // namespace

PageProtector::PageProtector(IPagingHost& host, allocators::IAllocator& allocator) : host_(host), allocator_(allocator) {
  const uint64_t satp = host_.satp();
  const uint64_t mode = satp >> 60;
  if (mode == 8) levels_ = 3;
  else if (mode == 9) levels_ = 4;
  else if (mode == 10) levels_ = 5;
  if (levels_) root_ = reinterpret_cast<uint64_t*>((satp & 0xFFFFFFFFFFFull) << 12);
}

bool PageProtector::split(uint64_t* entry, int level) {
  const uint64_t old = load(entry);
  if (old & kNapot) return false;
  auto* table = static_cast<uint64_t*>(allocator_.allocate_aligned(kPage, kPage));
  if (!table) return false;
  // 512 entries of the level below with the same flags, each covering its share of the superpage.
  const uint64_t step = uint64_t{1} << (9 * (level - 1));   // pages (of the level below) per entry, in ppn units
  const uint64_t ppn = (old & kPpnMask) >> 10;
  for (uint64_t i = 0; i < 512; ++i) table[i] = (old & ~kPpnMask) | ((ppn + i * step) << 10);
  store(entry, table_entry(table));
  return true;
}

bool PageProtector::protect_page(uint64_t address, jit::PageAccess access) {
  uint64_t* table = root_;
  for (int level = levels_ - 1;; --level) {
    uint64_t* entry = &table[(address >> (12 + 9 * level)) & 511];
    const uint64_t value = load(entry);
    if (!(value & kV)) return false;
    if (!is_leaf(value)) {
      if (level == 0) return false;   // (a pointer at the last level is malformed)
      table = table_at(value);
      continue;
    }
    if (value & kNapot) return false;
    if (level > 0) {
      if (!split(entry, level)) return false;
      table = table_at(load(entry));
      continue;   // the same address, one level down
    }
    const uint64_t base = value & ~(kR | kW | kX);
    store(entry, base | (access == jit::PageAccess::ReadWrite ? (kR | kW | kA | kD) : (kR | kX | kA)));
    return true;
  }
}

bool PageProtector::protect(uint64_t base, uint64_t length, jit::PageAccess access) {
  if (!levels_) return true;
  if (base % kPage || length % kPage || length > UINT64_MAX - base) return false;
  bool ok = true;
  {
    jit::SpinFlagGuard guard(lock_);
    for (uint64_t offset = 0; offset < length && ok; offset += kPage) ok = protect_page(base + offset, access);
  }
  // (also after a partial change: the pages that did change must not keep a stale translation)
  return host_.flush_tlb_all_harts() && ok;
}

}  // namespace arch_riscv64
