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

#include "Kernel/Paging.hpp"

namespace kernel {

namespace {

constexpr uint64_t kPresent = 1ull << 0, kWritable = 1ull << 1, kUser = 1ull << 2, kWriteThrough = 1ull << 3, kCacheDisable = 1ull << 4,
                   kAccessed = 1ull << 5, kDirty = 1ull << 6, kPageSize = 1ull << 7, kGlobal = 1ull << 8, kPatSmall = 1ull << 7,
                   kPatLarge = 1ull << 12, kNoExecute = 1ull << 63;
constexpr uint64_t kAddressMask = 0x000FFFFFFFFFF000ull;
constexpr uint64_t kMaxAddress = 1ull << 47;   // the lower half of the 48-bit space: identity mappings stay canonical

constexpr uint64_t size_at(int level) { return 1ull << (12 + 9 * (level - 1)); }   // what one entry of a table at `level` covers
constexpr uint32_t index_at(uint64_t address, int level) { return static_cast<uint32_t>((address >> (12 + 9 * (level - 1))) & 511); }

uint64_t* as_table(uint64_t physical) { return reinterpret_cast<uint64_t*>(physical); }
bool is_leaf(uint64_t entry, int level) { return level == 1 || (level <= 3 && (entry & kPageSize)); }

}  // namespace

// ------------------------------------------------------------------------------------------------ the CPU

namespace {
class CpuPagingHost final : public IPagingHost {
 public:
  uint64_t root() override {
    uint64_t v;
    __asm__ volatile("movq %%cr3, %0" : "=r"(v));
    return v & kAddressMask;
  }
  bool five_level() override {
    uint64_t v;
    __asm__ volatile("movq %%cr4, %0" : "=r"(v));
    return (v >> 12) & 1;
  }
  bool nx_enabled() override { return (rdmsr(0xC0000080) >> 11) & 1; }
  bool gigabyte_pages() override {
    uint32_t a = 0x80000000, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (a < 0x80000001) return false;
    a = 0x80000001;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d >> 26) & 1;
  }
  bool pat_available() override {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d >> 16) & 1;
  }
  uint64_t read_pat() override { return rdmsr(0x277); }
  void write_pat(uint64_t value) override { wrmsr(0x277, value); }
  void invalidate_page(uint64_t address) override { __asm__ volatile("invlpg (%0)" ::"r"(address) : "memory"); }
  void invalidate_all() override {
    uint64_t v;
    __asm__ volatile("movq %%cr3, %0; movq %0, %%cr3" : "=r"(v)::"memory");
  }

 private:
  static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
  }
  static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)) : "memory");
  }
};
}  // namespace

IPagingHost& cpu_paging_host() {
  static CpuPagingHost host;
  return host;
}

// ------------------------------------------------------------------------------------------------ page tables

namespace {
uint64_t g_shared_pat = 0;
}
uint64_t shared_pat_value() { return g_shared_pat; }
void load_shared_pat_on_this_core() {
  if (g_shared_pat) cpu_paging_host().write_pat(g_shared_pat);
}

bool PageTables::initialize() {
  if (host_.five_level()) return false;
  nx_ = host_.nx_enabled();
  gigabyte_ = host_.gigabyte_pages();
  pat_ = host_.pat_available();
  if (pat_) pat_value_ = host_.read_pat();
  wc_enabled_ = false;
  for (uint32_t i = 0; i < 8; ++i)
    if (((pat_value_ >> (8 * i)) & 0xff) == 0x01) wc_enabled_ = true;   // somebody already set one up
  ready_ = true;
  return true;
}

bool PageTables::enable_write_combining() {
  if (!ready_ || !pat_) return false;
  if (wc_enabled_) return true;
  pat_value_ = (pat_value_ & ~(0xffull << 32)) | (0x01ull << 32);   // entry 4: write-combining
  host_.write_pat(pat_value_);
  g_shared_pat = pat_value_;
  wc_enabled_ = true;
  return true;
}

void PageTables::load_pat() {
  if (pat_ && wc_enabled_) host_.write_pat(pat_value_);
}

// Which PAT index (PAT bit, PCD, PWT) selects this memory type? The PAT holds, per index, 0 UC, 1 WC, 4 WT, 6 WB, 7 UC-.
bool PageTables::pat_index_for(CacheMode mode, uint32_t* index) const {
  const uint8_t wanted = mode == CacheMode::WriteBack ? 6 : mode == CacheMode::WriteThrough ? 4 : mode == CacheMode::Uncached ? 0 : 1;
  for (uint32_t i = 0; i < 8; ++i)
    if (((pat_value_ >> (8 * i)) & 0xff) == wanted) {
      *index = i;
      return true;
    }
  return false;
}

CacheMode PageTables::mode_of(uint32_t pat_index) const {
  switch ((pat_value_ >> (8 * pat_index)) & 0xff) {
    case 6: return CacheMode::WriteBack;
    case 4: case 5: return CacheMode::WriteThrough;
    case 1: return CacheMode::WriteCombining;
    default: return CacheMode::Uncached;   // UC and UC-
  }
}

uint64_t PageTables::leaf_entry(uint64_t address, int level, const PageAttributes& a) const {
  uint32_t index = 0;
  pat_index_for(a.cache, &index);   // the caller checked that it exists
  uint64_t e = (address & kAddressMask) | kPresent;
  if (a.writable) e |= kWritable;
  if (index & 1) e |= kWriteThrough;
  if (index & 2) e |= kCacheDisable;
  if (index & 4) e |= level == 1 ? kPatSmall : kPatLarge;
  if (!a.executable && nx_) e |= kNoExecute;
  if (level > 1) e |= kPageSize;
  return e;
}

uint64_t* PageTables::new_table() {
  uint64_t* t = static_cast<uint64_t*>(allocator_.allocate_aligned(4096, 4096));
  if (!t) return nullptr;
  for (int i = 0; i < 512; ++i) t[i] = 0;
  return t;
}

// Replaces a large page by a table of smaller pages that map the same thing in the same way.
bool PageTables::split(uint64_t* entry, int level) {
  uint64_t* t = new_table();
  if (!t) return false;
  const uint64_t e = *entry;
  const uint64_t child_size = size_at(level - 1);
  const uint64_t base = e & (level == 3 ? 0x000FFFFFC0000000ull : 0x000FFFFFFFE00000ull);
  uint64_t flags = e & (kWritable | kUser | kWriteThrough | kCacheDisable | kAccessed | kDirty | kGlobal | kNoExecute);
  const bool pat = e & kPatLarge;
  if (level == 2 && pat) flags |= kPatSmall;   // the PAT bit moves from bit 12 to bit 7
  if (level == 3) {
    flags |= kPageSize;                         // the pieces are 2 MiB pages: PAT stays in bit 12
    if (pat) flags |= kPatLarge;
  }
  for (uint64_t i = 0; i < 512; ++i) t[i] = ((base + i * child_size) & kAddressMask) | kPresent | flags;
  *entry = (reinterpret_cast<uint64_t>(t) & kAddressMask) | kPresent | kWritable | (e & kUser);
  changed_ = true;
  return true;
}

bool PageTables::walk(uint64_t* table, int level, uint64_t table_base, uint64_t start, uint64_t end, const PageAttributes& a, Mode mode) {
  const uint64_t span = size_at(level);   // what one entry covers
  for (uint64_t at = start; at < end;) {
    const uint32_t idx = index_at(at, level);
    const uint64_t entry_base = table_base + static_cast<uint64_t>(idx) * span;
    const uint64_t entry_end = entry_base + span;
    const uint64_t hi = end < entry_end ? end : entry_end;
    uint64_t* entry = &table[idx];
    const bool full = at == entry_base && hi == entry_end;
    const bool present = *entry & kPresent;
    const bool may_be_leaf = level == 1 || (level == 2) || (level == 3 && gigabyte_);

    if (mode == Mode::Remove) {
      if (present) {
        if (full && level <= 3) {
          *entry = 0;
          changed_ = true;
        } else {
          if (is_leaf(*entry, level) && !split(entry, level)) return false;
          if (!walk(as_table(*entry & kAddressMask), level - 1, entry_base, at, hi, a, mode)) return false;
        }
      }
    } else if (full && may_be_leaf && !(present && !is_leaf(*entry, level) && mode == Mode::Missing)) {
      // the whole entry is wanted as one page
      if (!(present && mode == Mode::Missing)) {
        *entry = leaf_entry(entry_base, level, a);
        changed_ = true;
      }
    } else {
      // part of the entry: it has to be a table
      if (!present) {
        uint64_t* t = new_table();
        if (!t) return false;
        *entry = (reinterpret_cast<uint64_t>(t) & kAddressMask) | kPresent | kWritable;
        changed_ = true;
      } else if (is_leaf(*entry, level)) {
        if (mode == Mode::Missing) {   // already mapped as a whole: leave it
          at = hi;
          continue;
        }
        if (!split(entry, level)) return false;
      } else if (!(*entry & kWritable)) {
        *entry |= kWritable;           // permissions are decided by the pages
        changed_ = true;
      }
      if (!walk(as_table(*entry & kAddressMask), level - 1, entry_base, at, hi, a, mode)) return false;
    }
    at = hi;
  }
  return true;
}

bool PageTables::apply(uint64_t address, uint64_t bytes, const PageAttributes& a, Mode mode) {
  if (!ready_ || bytes == 0) return false;
  const uint64_t start = address & ~0xFFFull;
  if (bytes > kMaxAddress || address + bytes > kMaxAddress || address + bytes < address) return false;
  const uint64_t end = (address + bytes + 0xFFF) & ~0xFFFull;
  if (end > kMaxAddress) return false;
  if (mode != Mode::Remove) {
    uint32_t index;
    if (!pat_index_for(a.cache, &index)) return false;
    if (index >= 4 && !pat_) return false;
  }
  changed_ = false;
  const bool ok = walk(as_table(host_.root()), 4, 0, start, end, a, mode);
  if (changed_) host_.invalidate_all();
  return ok;
}

bool PageTables::map(uint64_t address, uint64_t bytes, const PageAttributes& attributes) { return apply(address, bytes, attributes, Mode::Replace); }
bool PageTables::map_missing(uint64_t address, uint64_t bytes, const PageAttributes& attributes) { return apply(address, bytes, attributes, Mode::Missing); }
bool PageTables::unmap(uint64_t address, uint64_t bytes) { return apply(address, bytes, PageAttributes(), Mode::Remove); }

bool PageTables::query(uint64_t address, PageMapping* out) const {
  *out = PageMapping();
  if (!ready_ || address >= kMaxAddress) return false;
  const uint64_t* table = as_table(host_.root());
  for (int level = 4; level >= 1; --level) {
    const uint64_t e = table[index_at(address, level)];
    if (!(e & kPresent)) return true;   // answered: not present
    if (level > 1 && !is_leaf(e, level)) {
      table = as_table(e & kAddressMask);
      continue;
    }
    if (level == 4) return false;       // a large page here is not a thing
    out->present = true;
    out->page_size = size_at(level);
    out->physical = (e & kAddressMask) & ~(size_at(level) - 1);
    const uint32_t pat = (e & (level == 1 ? kPatSmall : kPatLarge)) ? 1 : 0;
    out->attributes.cache = mode_of((pat << 2) | ((e & kCacheDisable) ? 2 : 0) | ((e & kWriteThrough) ? 1 : 0));
    out->attributes.writable = e & kWritable;
    out->attributes.executable = !(nx_ && (e & kNoExecute));
    return true;
  }
  return false;
}

bool PageTables::is_mapped(uint64_t address, uint64_t bytes) const {
  if (bytes == 0) return false;
  for (uint64_t at = address & ~0xFFFull; at < address + bytes; ) {
    PageMapping m;
    if (!query(at, &m) || !m.present || m.physical != (at & ~(m.page_size - 1))) return false;
    at = (at & ~(m.page_size - 1)) + m.page_size;
  }
  return true;
}

}  // namespace kernel
