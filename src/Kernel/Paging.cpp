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
#include "Kernel/Interrupts.hpp"
#include "Kernel/Rcu.hpp"
#include "Utility/UniquePointer.hpp"

namespace kernel {

namespace {

constexpr uint64_t kPresent = 1ull << 0, kWritable = 1ull << 1, kUser = 1ull << 2, kWriteThrough = 1ull << 3, kCacheDisable = 1ull << 4,
                   kAccessed = 1ull << 5, kDirty = 1ull << 6, kPageSize = 1ull << 7, kGlobal = 1ull << 8, kPatSmall = 1ull << 7,
                   kPatLarge = 1ull << 12, kNoExecute = 1ull << 63;
constexpr uint64_t kOwnedTable = 1ull << 9;   // (ignored by the CPU) on an entry that points to a table this class made, and may free
constexpr uint64_t kAddressMask = 0x000FFFFFFFFFF000ull;
constexpr uint64_t kMaxAddress = 1ull << 47;   // the lower half of the 48-bit space: identity mappings stay canonical

constexpr uint64_t size_at(int level) { return 1ull << (12 + 9 * (level - 1)); }   // what one entry of a table at `level` covers
constexpr uint32_t index_at(uint64_t address, int level) { return static_cast<uint32_t>((address >> (12 + 9 * (level - 1))) & 511); }

// Tables are used by their physical address: the memory they are in is mapped 1:1, and a table is never freed once it was linked in (see walk()), so the
// pointer stays valid for as long as anyone may follow it.
uint64_t* as_table(uint64_t physical) { return reinterpret_cast<uint64_t*>(physical); }
uint64_t table_address(const uint64_t* table) { return reinterpret_cast<uint64_t>(table) & kAddressMask; }
bool is_leaf(uint64_t entry, int level) { return level == 1 || (level <= 3 && (entry & kPageSize)); }

// Every entry is read and changed atomically: other cores edit the same tables at the same time, and the CPU itself sets the accessed and dirty bits with
// locked writes, so a change is a compare-and-swap from the value it was decided on (and is decided again if that value is gone).
uint64_t load_entry(const uint64_t* entry) { return __atomic_load_n(entry, __ATOMIC_ACQUIRE); }
bool swap_entry(uint64_t* entry, uint64_t expected, uint64_t desired) {
  return __atomic_compare_exchange_n(entry, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
bool has_write_combining(uint64_t pat) {
  for (uint32_t i = 0; i < 8; ++i)
    if (((pat >> (8 * i)) & 0xff) == 0x01) return true;
  return false;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the CPU

namespace {
class CpuPagingHost final : public IPagingHost {
 public:
  static constexpr uint64_t kCr0Wp = uint64_t{1} << 16;
  static constexpr uint64_t kWpWasSet = uint64_t{1} << 62;   // (above anything RFLAGS uses)
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
  uint64_t begin_table_edit() override {
    const uint64_t flags = kernel::irq_save();
    uint64_t cr0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & kCr0Wp)) return flags;
    __asm__ volatile("movq %0, %%cr0" ::"r"(cr0 & ~kCr0Wp) : "memory");
    return flags | kWpWasSet;
  }
  void end_table_edit(uint64_t token) override {
    if (token & kWpWasSet) {
      uint64_t cr0;
      __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
      __asm__ volatile("movq %0, %%cr0" ::"r"(cr0 | kCr0Wp) : "memory");
    }
    kernel::irq_restore(token & ~kWpWasSet);
  }
  bool can_wait() override { return kernel::irq_enabled(); }
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
uint64_t shared_pat_value() { return __atomic_load_n(&g_shared_pat, __ATOMIC_ACQUIRE); }
void load_shared_pat_on_this_core() {
  if (const uint64_t value = shared_pat_value()) cpu_paging_host().write_pat(value);
}

// One unlinked table (with what hangs below it), waiting for its grace period.
struct PageTables::RetiredTable : RcuNode {
  PageTables* owner = nullptr;
  uint64_t table = 0;    // physical address
  int level = 0;         // of the table's entries
  bool owned = false;    // made here (else the firmware's: only its children may be ours)
  UniquePointer<RetiredTable> self;   // the node owns itself until its callback has run
};

PageTables::PageTables(IPagingHost& host, allocators::IAllocator& allocator, RcuDomain* rcu)
    : host_(host), allocator_(allocator), rcu_(rcu ? *rcu : rcu_default_domain()) {}

bool PageTables::initialize() {
  if (host_.five_level()) return false;
  nx_ = host_.nx_enabled();
  gigabyte_ = host_.gigabyte_pages();
  pat_ = host_.pat_available();
  if (pat_) __atomic_store_n(&pat_value_, host_.read_pat(), __ATOMIC_RELEASE);   // (it may hold write-combining already: somebody set one up)
  __atomic_store_n(&ready_, true, __ATOMIC_RELEASE);
  return true;
}

bool PageTables::enable_write_combining() {
  if (!__atomic_load_n(&ready_, __ATOMIC_ACQUIRE) || !pat_) return false;
  uint64_t value = pat();
  while (!has_write_combining(value)) {
    const uint64_t wanted = (value & ~(0xffull << 32)) | (0x01ull << 32);   // entry 4: write-combining
    if (__atomic_compare_exchange_n(&pat_value_, &value, wanted, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {   // (a loser sees the winner's value)
      host_.write_pat(wanted);
      __atomic_store_n(&g_shared_pat, wanted, __ATOMIC_RELEASE);
      break;
    }
  }
  return true;
}

bool PageTables::write_combining_enabled() const { return has_write_combining(pat()); }

void PageTables::load_pat() {
  const uint64_t value = pat();
  if (pat_ && has_write_combining(value)) host_.write_pat(value);
}

// Which PAT index (PAT bit, PCD, PWT) selects this memory type? The PAT holds, per index, 0 UC, 1 WC, 4 WT, 6 WB, 7 UC-.
bool PageTables::pat_index_for(CacheMode mode, uint32_t* index) const {
  const uint8_t wanted = mode == CacheMode::WriteBack ? 6 : mode == CacheMode::WriteThrough ? 4 : mode == CacheMode::Uncached ? 0 : 1;
  const uint64_t value = pat();
  for (uint32_t i = 0; i < 8; ++i)
    if (((value >> (8 * i)) & 0xff) == wanted) {
      *index = i;
      return true;
    }
  return false;
}

CacheMode PageTables::mode_of(uint32_t pat_index) const {
  switch ((pat() >> (8 * pat_index)) & 0xff) {
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

// Links a new, empty table into the entry, which was `seen` (not present).
PageTables::Edit PageTables::install_table(uint64_t* entry, uint64_t seen, Change& change) {
  uint64_t* t = new_table();
  if (!t) return Edit::NoMemory;
  if (!swap_entry(entry, seen, table_address(t) | kPresent | kWritable | kOwnedTable)) {
    allocator_.deallocate(t);   // somebody else changed the entry first; nobody saw this table
    return Edit::Raced;
  }
  change.changed = true;
  return Edit::Done;
}

// Replaces a large page (the entry, `seen`) by a table of smaller pages that map the same thing in the same way.
PageTables::Edit PageTables::split(uint64_t* entry, uint64_t seen, int level, Change& change) {
  uint64_t* t = new_table();
  if (!t) return Edit::NoMemory;
  const uint64_t child_size = size_at(level - 1);
  const uint64_t base = seen & (level == 3 ? 0x000FFFFFC0000000ull : 0x000FFFFFFFE00000ull);
  uint64_t flags = seen & (kWritable | kUser | kWriteThrough | kCacheDisable | kAccessed | kDirty | kGlobal | kNoExecute);
  const bool pat = seen & kPatLarge;
  if (level == 2 && pat) flags |= kPatSmall;   // the PAT bit moves from bit 12 to bit 7
  if (level == 3) {
    flags |= kPageSize;                         // the pieces are 2 MiB pages: PAT stays in bit 12
    if (pat) flags |= kPatLarge;
  }
  for (uint64_t i = 0; i < 512; ++i) t[i] = ((base + i * child_size) & kAddressMask) | kPresent | flags;
  if (!swap_entry(entry, seen, table_address(t) | kPresent | kWritable | kOwnedTable | (seen & kUser))) {
    allocator_.deallocate(t);   // changed meanwhile (by another core, or the CPU set the dirty bit): decide again
    return Edit::Raced;
  }
  change.changed = true;
  return Edit::Done;
}

// Each entry is handled in a loop: read it, decide, and make the change with a compare-and-swap; if the entry changed in between, read it again. Two
// cores that need the same table (or the same large page split) both build one, one of them links it in and the other throws its copy away and uses
// the winner's. A table that a change unlinks (a page mapped or unmapped over it as a whole) is not freed at once: another core may still be walking it
// (its edits then simply come too late, as if they had happened before the change that unlinked it), and the CPUs may still cache it. It goes on the
// change's list, and apply() retires it through RCU once the shootdown is done.
bool PageTables::walk(uint64_t* table, int level, uint64_t table_base, uint64_t start, uint64_t end, const PageAttributes& a, Mode mode, Change& change) {
  const uint64_t span = size_at(level);   // what one entry covers
  for (uint64_t at = start; at < end;) {
    const uint32_t idx = index_at(at, level);
    const uint64_t entry_base = table_base + static_cast<uint64_t>(idx) * span;
    const uint64_t entry_end = entry_base + span;
    const uint64_t hi = end < entry_end ? end : entry_end;
    uint64_t* entry = &table[idx];
    const bool full = at == entry_base && hi == entry_end;
    const bool may_be_leaf = level == 1 || (level == 2) || (level == 3 && gigabyte_);

    for (;;) {
      const uint64_t e = load_entry(entry);
      const bool present = e & kPresent;
      if (mode == Mode::Remove) {
        if (!present) break;
        if (full && level <= 3) {
          if (!swap_entry(entry, e, 0)) continue;
          change.changed = true;
          if (!is_leaf(e, level)) unlinked(e, level, change);
          break;
        }
        if (is_leaf(e, level)) {
          if (split(entry, e, level, change) == Edit::NoMemory) return false;
          continue;   // now a table (ours or another core's)
        }
        if (!walk(as_table(e & kAddressMask), level - 1, entry_base, at, hi, a, mode, change)) return false;
        break;
      }
      if (full && may_be_leaf && !(present && !is_leaf(e, level) && mode == Mode::Missing)) {
        // the whole entry is wanted as one page
        if (present && mode == Mode::Missing) break;
        if (!swap_entry(entry, e, leaf_entry(entry_base, level, a))) continue;
        change.changed = true;
        if (present && !is_leaf(e, level)) unlinked(e, level, change);
        break;
      }
      // part of the entry: it has to be a table
      if (!present) {
        if (install_table(entry, e, change) == Edit::NoMemory) return false;
        continue;
      }
      if (is_leaf(e, level)) {
        if (mode == Mode::Missing) break;   // already mapped as a whole: leave it
        if (split(entry, e, level, change) == Edit::NoMemory) return false;
        continue;
      }
      if (!(e & kWritable)) {
        __atomic_fetch_or(entry, kWritable, __ATOMIC_ACQ_REL);   // permissions are decided by the pages
        change.changed = true;
      }
      if (!walk(as_table(e & kAddressMask), level - 1, entry_base, at, hi, a, mode, change)) return false;
      break;
    }
    at = hi;
  }
  return true;
}

// `entry` (at `level`) pointed to a table and was just replaced: remember the table for freeing. If no node can be had for it, it is kept.
void PageTables::unlinked(uint64_t entry, int level, Change& change) {
  change.unlinked = true;
  UniquePointer<RetiredTable> node = UniquePointer<RetiredTable>::make(allocator_);
  if (!node) return;
  RetiredTable* r = node.get();
  r->owner = this;
  r->table = entry & kAddressMask;
  r->level = level - 1;
  r->owned = entry & kOwnedTable;
  r->callback = &free_retired;
  r->next = change.retired;
  r->self = util::move(node);
  change.retired = r;
}

// Frees the tables this class made in the tree below `physical` (whose entries are at `level`), and that table itself if `owned`. Nothing can reach the
// tree any more: the walk has it to itself.
void PageTables::free_subtree(uint64_t physical, int level, bool owned) {
  uint64_t* t = as_table(physical);
  if (level > 1)
    for (uint32_t i = 0; i < 512; ++i) {
      const uint64_t e = load_entry(&t[i]);
      if ((e & kPresent) && !is_leaf(e, level)) free_subtree(e & kAddressMask, level - 1, e & kOwnedTable);
    }
  if (owned) allocator_.deallocate(t);
}

void PageTables::free_retired(RcuNode* node) {
  RetiredTable* r = static_cast<RetiredTable*>(node);
  r->owner->free_subtree(r->table, r->level, r->owned);
  drop(r);
}

// Frees the node (not the table).
void PageTables::drop(RetiredTable* table) { UniquePointer<RetiredTable> gone = util::move(table->self); }

bool PageTables::apply(uint64_t address, uint64_t bytes, const PageAttributes& a, Mode mode) {
  if (!__atomic_load_n(&ready_, __ATOMIC_ACQUIRE) || bytes == 0) return false;
  const uint64_t start = address & ~0xFFFull;
  if (bytes > kMaxAddress || address + bytes > kMaxAddress || address + bytes < address) return false;
  const uint64_t end = (address + bytes + 0xFFF) & ~0xFFFull;
  if (end > kMaxAddress) return false;
  if (mode != Mode::Remove) {
    uint32_t index;
    if (!pat_index_for(a.cache, &index)) return false;
    if (index >= 4 && !pat_) return false;
  }
  ITlbShootdown* shootdown = __atomic_load_n(&shootdown_, __ATOMIC_ACQUIRE);
  if (shootdown && !host_.can_wait()) shootdown = nullptr;
  Change change;
  bool ok;
  {
    RcuReader reader(rcu_);   // (other cores may free the tables this walk passes through only after it is done)
    const uint64_t edit = host_.begin_table_edit();
    ok = walk(as_table(host_.root()), 4, 0, start, end, a, mode, change);
    if (change.changed && !shootdown) host_.invalidate_all();
    host_.end_table_edit(edit);
  }
  // The shootdown runs with interrupts on again (it waits for the other cores). A table unlinked here may sit in any CPU's paging-structure caches,
  // whatever range it covered, so that takes a flush of everything.
  bool flushed = true;
  if (change.changed && shootdown) flushed = change.unlinked ? shootdown->invalidate_range(0, kMaxAddress) : shootdown->invalidate_range(start, end - start);
  for (RetiredTable* r = change.retired; r;) {
    RetiredTable* next = static_cast<RetiredTable*>(r->next);
    if (shootdown && flushed) rcu_.retire_node(*r);   // (no CPU caches it any more; the grace period waits for the software walks)
    else drop(r);                                      // some CPU may still use it: keep the table
    r = next;
  }
  return ok && flushed;
}

bool PageTables::map(uint64_t address, uint64_t bytes, const PageAttributes& attributes) { return apply(address, bytes, attributes, Mode::Replace); }
bool PageTables::map_missing(uint64_t address, uint64_t bytes, const PageAttributes& attributes) { return apply(address, bytes, attributes, Mode::Missing); }
bool PageTables::unmap(uint64_t address, uint64_t bytes) { return apply(address, bytes, PageAttributes(), Mode::Remove); }

bool PageTables::query(uint64_t address, PageMapping* out) const {
  *out = PageMapping();
  if (!__atomic_load_n(&ready_, __ATOMIC_ACQUIRE) || address >= kMaxAddress) return false;
  RcuReader reader(rcu_);
  const uint64_t* table = as_table(host_.root());
  for (int level = 4; level >= 1; --level) {
    const uint64_t e = load_entry(&table[index_at(address, level)]);
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
