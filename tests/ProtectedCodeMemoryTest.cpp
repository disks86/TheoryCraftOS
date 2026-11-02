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
// Host tests for the code memory of the images without page-table code: the RISC-V page protector against Sv39 / Sv48 tables built in this process (read back
// with an independent walk), and jit::ProtectedCodeMemory against a protector that records what it is asked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Arch/RiscV64/PageProtector.hpp"
#include "Jit/CodePageSource.hpp"
#include "Jit/ProtectedCodeMemory.hpp"

using namespace arch_riscv64;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct Alloc final : allocators::IAllocator {
  std::vector<void*> blocks;
  long fail_after = -1;
  long live = 0;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (size == 0 || fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    const uint64_t a = alignment < 64 ? 64 : alignment;
    void* p = aligned_alloc(a, (size + a - 1) / a * a);
    blocks.push_back(p);
    ++live;
    return p;
  }
  void deallocate(void* p) override { if (p) --live; }
  ~Alloc() { for (void* p : blocks) free(p); }
};

constexpr uint64_t kV = 1, kR = 2, kW = 4, kX = 8, kA = 0x40, kD = 0x80;
constexpr uint64_t kPpn = 0x003FFFFFFFFFFC00ull;

struct FakeHost final : IPagingHost {
  uint64_t satp_value = 0;
  int flushes = 0;
  bool flush_ok = true;
  uint64_t satp() override { return satp_value; }
  bool flush_tlb_all_harts() override { ++flushes; return flush_ok; }
};

// The page tables of a pretend machine: `levels` 3 or 4, one big leaf at the top that maps [va, va + size of an entry) with R W X.
struct Tables {
  std::vector<uint64_t*> owned;
  uint64_t* root = nullptr;
  int levels;
  explicit Tables(int l, uint64_t leaf_va, int leaf_level) : levels(l) {
    root = table();
    // path of tables down to leaf_level, then the leaf: identity mapped (ppn = va >> 12)
    uint64_t* t = root;
    for (int level = l - 1; level > leaf_level; --level) {
      uint64_t* next = table();
      t[(leaf_va >> (12 + 9 * level)) & 511] = ((reinterpret_cast<uint64_t>(next) >> 12) << 10) | kV;
      t = next;
    }
    t[(leaf_va >> (12 + 9 * leaf_level)) & 511] = ((leaf_va >> 12) << 10) | kV | kR | kW | kX | kA | kD;
  }
  uint64_t* table() {
    auto* t = static_cast<uint64_t*>(aligned_alloc(4096, 4096));
    memset(t, 0, 4096);
    owned.push_back(t);
    return t;
  }
  ~Tables() { for (uint64_t* t : owned) free(t); }
  uint64_t satp() const { return (uint64_t(levels == 3 ? 8 : levels == 4 ? 9 : 10) << 60) | (reinterpret_cast<uint64_t>(root) >> 12); }

  // An independent reading: the leaf entry that translates va, and its level.
  bool walk(uint64_t va, uint64_t* entry, int* leaf_level) const {
    const uint64_t* t = root;
    for (int level = levels - 1; level >= 0; --level) {
      const uint64_t e = t[(va >> (12 + 9 * level)) & 511];
      if (!(e & kV)) return false;
      if (e & (kR | kW | kX)) { *entry = e; *leaf_level = level; return true; }
      t = reinterpret_cast<const uint64_t*>((e & kPpn) << 2);
    }
    return false;
  }
  // The physical page va translates to.
  bool physical(uint64_t va, uint64_t* phys) const {
    uint64_t e; int level;
    if (!walk(va, &e, &level)) return false;
    const uint64_t span = uint64_t{1} << (12 + 9 * level);
    *phys = (((e & kPpn) >> 10) << 12 & ~(span - 1)) | (va & (span - 1));
    return true;
  }
};

bool access_is(const Tables& t, uint64_t va, uint64_t pages, bool writable, bool executable) {
  for (uint64_t i = 0; i < pages; ++i) {
    uint64_t e; int level;
    if (!t.walk(va + i * 4096, &e, &level)) return false;
    if (!(e & kR) || !!(e & kW) != writable || !!(e & kX) != executable) return false;
  }
  return true;
}

struct RecordingProtector final : jit::IPageProtector {
  struct Call { uint64_t base, length; jit::PageAccess access; };
  std::vector<Call> calls;
  bool is_enforced = true;
  long fail_after = -1;   // successful calls left (-1: all)
  bool enforced() const override { return is_enforced; }
  bool protect(uint64_t base, uint64_t length, jit::PageAccess access) override {
    if (fail_after == 0) return false;
    if (fail_after > 0) --fail_after;
    calls.push_back({base, length, access});
    return true;
  }
};

}  // namespace

int main() {
  constexpr uint64_t kGiB = 1ull << 30, kMiB = 1ull << 20;
  // ---- Sv39: a 1 GiB leaf at 2 GiB (like the firmware's RAM mapping) is split down to the pages asked for
  {
    Tables t(3, 2 * kGiB, 2);
    Alloc a;
    FakeHost host;
    host.satp_value = t.satp();
    PageProtector pp(host, a);
    CHECK(pp.enforced());
    const uint64_t va = 2 * kGiB + 0x2000000 + 0x5000;   // inside the gigabyte, not 2 MiB aligned
    CHECK(access_is(t, va, 4, true, true));
    CHECK(pp.protect(va, 4 * 4096, jit::PageAccess::ReadExecute) && host.flushes == 1);
    CHECK(access_is(t, va, 4, false, true));
    CHECK(access_is(t, va - 4096, 1, true, true) && access_is(t, va + 4 * 4096, 1, true, true));   // the neighbours keep what they had
    // the mapping itself did not change anywhere: sample the whole gigabyte
    for (uint64_t off = 0; off < kGiB; off += 0x1F3000 + 0x123) {
      uint64_t phys = 0;
      CHECK(t.physical(2 * kGiB + off, &phys) && phys == 2 * kGiB + off);
    }
    uint64_t e; int level;
    CHECK(t.walk(2 * kGiB, &e, &level) && level == 1);                  // the rest of the gigabyte is 2 MiB pages now
    CHECK(t.walk(2 * kGiB + 0x2000000 + 0x5000 - 4096, &e, &level) && level == 0);
    CHECK(pp.protect(va, 4 * 4096, jit::PageAccess::ReadWrite) && access_is(t, va, 4, true, false));   // and back: writable, not executable
    CHECK(pp.protect(va, 4 * 4096, jit::PageAccess::ReadExecute) && access_is(t, va, 4, false, true));
    const size_t tables_before = a.blocks.size();
    CHECK(pp.protect(va, 4096, jit::PageAccess::ReadWrite) && a.blocks.size() == tables_before);        // no more splitting
    // arguments
    CHECK(!pp.protect(va + 1, 4096, jit::PageAccess::ReadWrite) && !pp.protect(va, 100, jit::PageAccess::ReadWrite));
    CHECK(!pp.protect(8 * kGiB, 4096, jit::PageAccess::ReadWrite));   // unmapped
    // a hart that does not answer the TLB flush
    host.flush_ok = false;
    CHECK(!pp.protect(va, 4096, jit::PageAccess::ReadExecute));
  }
  // ---- Sv48 with a 2 MiB leaf, and the table pages running out
  {
    Tables t(4, 4 * kGiB + 6 * kMiB, 1);
    Alloc a;
    FakeHost host;
    host.satp_value = t.satp();
    PageProtector pp(host, a);
    const uint64_t va = 4 * kGiB + 6 * kMiB + 0x3000;
    a.fail_after = 0;
    CHECK(!pp.protect(va, 4096, jit::PageAccess::ReadExecute));
    CHECK(access_is(t, va, 1, true, true));   // untouched
    uint64_t e; int level;
    CHECK(t.walk(va, &e, &level) && level == 1);
    a.fail_after = -1;
    CHECK(pp.protect(va, 3 * 4096, jit::PageAccess::ReadExecute) && access_is(t, va, 3, false, true));
    CHECK(t.walk(va, &e, &level) && level == 0);
  }
  // ---- no translation: nothing to protect; a napot leaf is refused
  {
    Alloc a;
    FakeHost host;
    host.satp_value = 0;
    PageProtector off(host, a);
    CHECK(!off.enforced() && off.protect(0x1000, 4096, jit::PageAccess::ReadExecute) && host.flushes == 0);
    Tables t(3, 2 * kGiB, 2);
    uint64_t* leaf = &t.root[(2 * kGiB >> 30) & 511];
    *leaf |= uint64_t{1} << 63;
    host.satp_value = t.satp();
    PageProtector pp(host, a);
    CHECK(!pp.protect(2 * kGiB, 4096, jit::PageAccess::ReadExecute));
  }

  // ---- ProtectedCodeMemory: what it asks of the protector, in what order, and how it rolls back
  {
    Alloc backing;
    jit::CodePool pool(backing, 1);
    RecordingProtector prot;
    struct Sync final : jit::IInstructionCacheSync {
      RecordingProtector* p = nullptr;
      int calls = 0, protect_calls_at_sync = -1;
      bool ok = true;
      bool sync(ConstByteView) override { ++calls; protect_calls_at_sync = static_cast<int>(p->calls.size()); return ok; }
    } sync;
    sync.p = &prot;
    jit::ProtectedCodeMemory mem(pool, prot, &sync);
    CHECK(mem.can_execute() && mem.protections_enforced() && mem.page_size() == 4096);
    jit::CodeRegion r;
    CHECK(!mem.allocate(0, &r));
    CHECK(mem.allocate(5000, &r) && r.bytes.size() == 8192 && prot.calls.size() == 1 && prot.calls[0].access == jit::PageAccess::ReadWrite &&
          prot.calls[0].base == reinterpret_cast<uint64_t>(r.bytes.data()) && prot.calls[0].length == 8192);
    for (uint8_t b : r.bytes) CHECK(b == 0);
    CHECK(mem.make_executable(r) && prot.calls.size() == 2 && prot.calls[1].access == jit::PageAccess::ReadExecute);
    CHECK(sync.calls == 1 && sync.protect_calls_at_sync == 1);   // synchronised while the pages were still writable, before they became executable
    CHECK(mem.make_writable(r) && prot.calls.back().access == jit::PageAccess::ReadWrite);
    sync.ok = false;
    const size_t before = prot.calls.size();
    CHECK(!mem.make_executable(r) && prot.calls.size() == before);   // refused by the sync: never made executable
    sync.ok = true;
    prot.fail_after = 0;
    CHECK(!mem.make_executable(r));                                    // the protector refuses: stays data (the roll back also fails, harmlessly)
    prot.fail_after = -1;
    const uint8_t* first = r.bytes.data();
    mem.release(r);
    CHECK(prot.calls.back().access == jit::PageAccess::ReadWrite);
    jit::CodeRegion again;
    CHECK(mem.allocate(100, &again) && again.bytes.data() == first);  // the pool took the pages back
    // allocation: the protector refusing leaks nothing it could not restore, and an unenforced protector still hands out memory
    prot.fail_after = 0;
    jit::CodeRegion denied;
    CHECK(!mem.allocate(4096, &denied));
    prot.fail_after = -1;
    prot.is_enforced = false;
    CHECK(!mem.protections_enforced() && mem.allocate(4096, &denied));
    mem.release(denied);
    mem.release(jit::CodeRegion());
    CHECK(!mem.make_executable(jit::CodeRegion()) && !mem.make_writable(jit::CodeRegion()));
  }

  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("ProtectedCodeMemoryTest: all passed\n");
  return 0;
}
