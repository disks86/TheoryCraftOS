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

// Host-side tests for the MMIO layer: MmioRegion's accesses, MmioMapper over a pretend CPU's page tables, and PCI BAR sizing and mapping against a pretend
// PCI function whose BARs behave the way the specification describes (reading back the size mask after ones were written).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "Hardware/Bus/Pci/PciBar.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/MmioMapper.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct FakeCpu final : kernel::IPagingHost {
  uint64_t* root_table;
  std::vector<void*> owned;
  uint64_t pat_msr = 0x0007040600070406ull;
  FakeCpu(int gigabytes) {
    root_table = static_cast<uint64_t*>(aligned_alloc(4096, 4096));
    memset(root_table, 0, 4096);
    uint64_t* pdpt = static_cast<uint64_t*>(aligned_alloc(4096, 4096));
    owned.push_back(pdpt);
    memset(pdpt, 0, 4096);
    for (int i = 0; i < gigabytes; ++i) pdpt[i] = (static_cast<uint64_t>(i) << 30) | 3 | 0x80;
    root_table[0] = reinterpret_cast<uint64_t>(pdpt) | 3;
  }
  ~FakeCpu() { free(root_table); for (void* p : owned) free(p); }
  uint64_t root() override { return reinterpret_cast<uint64_t>(root_table); }
  bool five_level() override { return false; }
  bool nx_enabled() override { return true; }
  bool gigabyte_pages() override { return true; }
  bool pat_available() override { return true; }
  uint64_t read_pat() override { return pat_msr; }
  void write_pat(uint64_t v) override { pat_msr = v; }
  void invalidate_page(uint64_t) override {}
  void invalidate_all() override {}
};

// Memory below 4 GiB, as the firmware lists it.
struct FakeRam final : IRandomAccessMemory {
  const char* name() const override { return "ram"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t total_bytes() const override { return 0; }
  uint64_t usable_bytes() const override { return 0; }
  void* base() const override { return nullptr; }
  bool overlaps_memory(uint64_t address, uint64_t bytes) const override { return address < 0x80000000ull && address + bytes > 0x100000 ; }
};

// A PCI bus with a few functions whose config space is a plain array, and BARs that size themselves.
struct FakePci final : IPciBus {
  struct Bar { uint32_t flags; uint64_t size; uint64_t base; };   // flags: the low bits of the register (type, prefetchable); size 0: not implemented
  struct Function {
    PciAddress address;
    uint32_t header_type;
    Bar bars[6];
    uint32_t command = 0x0007;   // I/O, memory, bus master
    uint32_t status = 0x0210;    // some status bits that are write-1-to-clear
    bool latched[6] = {};
    bool decode_was_on_during_sizing = false;
  };
  std::vector<Function> functions;
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(functions.size()); }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override {
    if (i >= functions.size()) return false;
    *out = PciDeviceInfo{functions[i].address, 0x1234, static_cast<uint16_t>(i), 0, 0, 0};
    return true;
  }
  Function* find(PciAddress a) const {
    for (const Function& f : functions)
      if (f.address.bus == a.bus && f.address.device == a.device && f.address.function == a.function) return const_cast<Function*>(&f);
    return nullptr;
  }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    Function* f = find(a);
    if (!f) return 0xFFFFFFFF;
    if (offset == 0x04) return f->command | (f->status << 16);
    if (offset == 0x0C) return f->header_type << 16;
    if (offset >= 0x10 && offset < 0x28) {
      const int i = (offset - 0x10) / 4;
      const Bar& b = f->bars[i];
      const bool upper = i > 0 && (f->bars[i - 1].flags & 6) == 4 && f->bars[i - 1].size != 0;
      if (upper) {   // the upper half of the previous (64-bit) BAR
        const Bar& low = f->bars[i - 1];
        return f->latched[i] ? static_cast<uint32_t>(~(low.size - 1) >> 32) : static_cast<uint32_t>(low.base >> 32);
      }
      if (b.size == 0) return 0;
      if (f->latched[i]) return (static_cast<uint32_t>(~(b.size - 1)) & ~0xFu) | b.flags;
      return static_cast<uint32_t>(b.base & ~0xFull) | b.flags;
    }
    return 0;
  }
  void write32(PciAddress a, uint16_t offset, uint32_t value) const override {
    Function* f = find(a);
    if (!f) return;
    if (offset == 0x04) {
      f->command = value & 0xFFFF;
      f->status &= ~(value >> 16);   // write-1-to-clear
      return;
    }
    if (offset >= 0x10 && offset < 0x28) {
      const int i = (offset - 0x10) / 4;
      if (value == 0xFFFFFFFF) {
        if (f->command & 3) f->decode_was_on_during_sizing = true;
        f->latched[i] = true;
        return;
      }
      f->latched[i] = false;
      const bool upper = i > 0 && (f->bars[i - 1].flags & 6) == 4 && f->bars[i - 1].size != 0;
      if (upper) f->bars[i - 1].base = (f->bars[i - 1].base & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      else f->bars[i].base = (f->bars[i].base & ~0xFFFFFFFFull) | (value & ~0xFu);
    }
  }
  uint64_t memory_bar(PciAddress a, uint8_t index) const override {
    Function* f = find(a);
    if (!f || index > 5 || f->bars[index].size == 0 || (f->bars[index].flags & 1)) return 0;
    return f->bars[index].base;
  }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

constexpr uint64_t kMiB = 1 << 20, kGiB = 1ull << 30;

int main() {
  // ---- register accesses
  {
    alignas(64) uint8_t block[64];
    memset(block, 0, sizeof block);
    MmioRegion r(reinterpret_cast<uint64_t>(block), sizeof block);
    CHECK(r.valid() && r.length() == 64 && !MmioRegion().valid());
    r.write32(0, 0x11223344);
    CHECK(r.read32(0) == 0x11223344 && r.read16(0) == 0x3344 && r.read16(2) == 0x1122 && r.read8(3) == 0x11 && block[0] == 0x44);
    r.write8(4, 0xAB);
    r.write16(6, 0xCDEF);
    CHECK(r.read32(4) == 0xCDEF00AB);
    r.write64(8, 0x0123456789ABCDEFull);
    CHECK(r.read64(8) == 0x0123456789ABCDEFull && r.read32(8) == 0x89ABCDEF && r.read32(12) == 0x01234567);
    r.write64_split(16, 0xAABBCCDD11223344ull);
    CHECK(r.read64_split(16) == 0xAABBCCDD11223344ull && r.read32(16) == 0x11223344 && r.read32(20) == 0xAABBCCDD);
    r.write32(24, 0xFF00FF00);
    r.update32(24, 0x0000FFFF, 0x12345678);
    CHECK(r.read32(24) == 0xFF005678);
    r.set_bits32(24, 0x1);
    r.clear_bits32(24, 0xFF000000);
    CHECK(r.read32(24) == 0x00005679);
    CHECK(r.wait32(24, 0xFF, 0x79, 3) && !r.wait32(24, 0xFF, 0x00, 5) && !r.wait32(24, 0xFF, 0x79, 0));
    MmioRegion s = r.subregion(32, 16);
    s.write32(0, 0xCAFEBABE);
    CHECK(s.length() == 16 && s.address() == r.address() + 32 && r.read32(32) == 0xCAFEBABE && r.address_of(32) == s.address());
    // an access outside the window, a misaligned one, or a bad subregion stops the OS: here, a child that aborts
    auto aborts = [&](void (*body)(const MmioRegion&)) {
      fflush(stdout);
      const pid_t pid = fork();
      if (pid == 0) {
        if (!freopen("/dev/null", "w", stderr)) _exit(2);
        body(MmioRegion(reinterpret_cast<uint64_t>(block), sizeof block));
        _exit(0);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    };
    CHECK(aborts([](const MmioRegion& m) { (void)m.read32(64); }));
    CHECK(aborts([](const MmioRegion& m) { (void)m.read32(62); }));
    CHECK(aborts([](const MmioRegion& m) { m.write8(64, 0); }));
    CHECK(aborts([](const MmioRegion& m) { (void)m.read32(2); }));       // misaligned
    CHECK(aborts([](const MmioRegion& m) { (void)m.read64(4); }));
    CHECK(aborts([](const MmioRegion& m) { (void)m.subregion(60, 8); }));
    CHECK(aborts([](const MmioRegion& m) { (void)m.subregion(8, 0); }));
    CHECK(!aborts([](const MmioRegion& m) { (void)m.read32(60); (void)m.read8(63); }));
  }

  // ---- the mapper
  {
    FakeCpu cpu(4);
    TestAllocator alloc;
    kernel::PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    FakeRam ram;
    MmioMapper mapper(pt, ram);
    MmioRegion r;
    CHECK(mapper.map(0x280000000ull, 256 * kMiB, MmioKind::Registers, &r) && r.address() == 0x280000000ull && r.length() == 256 * kMiB);
    kernel::PageMapping m;
    CHECK(pt.query(r.address(), &m) && m.present && m.attributes.cache == kernel::CacheMode::Uncached && !m.attributes.executable && m.attributes.writable);
    CHECK(mapper.is_mapped(r.address(), r.length()) && mapper.mapping_count() == 1);
    // the same window again: one more use
    MmioRegion again;
    CHECK(mapper.map(0x280000000ull, 256 * kMiB, MmioKind::Registers, &again) && again.address() == r.address() && mapper.mapping_count() == 1);
    mapper.release(again);
    CHECK(mapper.mapping_count() == 1);
    mapper.release(r);
    CHECK(mapper.mapping_count() == 0);
    // a small window that does not start on a page
    MmioRegion s;
    CHECK(mapper.map(0xFED00100, 0x400, MmioKind::Registers, &s) && s.address() == 0xFED00100 && s.length() == 0x400);
    CHECK(pt.query(0xFED00000, &m) && m.page_size == 4096 && m.attributes.cache == kernel::CacheMode::Uncached);
    // RAM is refused, an empty range too
    MmioRegion bad;
    CHECK(!mapper.map(0x1000000, 4096, MmioKind::Registers, &bad) && !mapper.map(0x7FFFF000, 8192, MmioKind::Registers, &bad) && !mapper.map(0xFEC00000, 0, MmioKind::Registers, &bad));
    CHECK(pt.query(0x1000000, &m) && m.attributes.cache == kernel::CacheMode::WriteBack);   // untouched
    // different kinds over the same pages clash; a frame buffer becomes write-combining
    CHECK(!mapper.map(0xFED00000, 4096, MmioKind::FrameBuffer, &bad));
    MmioRegion fb;
    CHECK(mapper.map(0xE0000000, 16 * kMiB, MmioKind::FrameBuffer, &fb));
    CHECK(pt.query(0xE0000000, &m) && m.attributes.cache == kernel::CacheMode::WriteCombining && cpu.pat_msr == 0x0007040600070406ull + (0x01ull << 32) - (0x06ull << 32));
    // ensure_mapped keeps what the firmware set up and fills only holes
    MmioRegion e;
    CHECK(mapper.ensure_mapped(0xA0000000, 4096, &e) && pt.query(0xA0000000, &m) && m.page_size == kGiB && m.attributes.cache == kernel::CacheMode::WriteBack);
    CHECK(mapper.ensure_mapped(0x500000000ull, 8192, &e) && pt.query(0x500001000ull, &m) && m.present && m.attributes.cache == kernel::CacheMode::Uncached);
    // the table of mappings is finite
    int ok = 0;
    for (uint64_t i = 0; i < 200; ++i) ok += mapper.map(0x1000000000ull + i * 0x10000, 4096, MmioKind::Registers, &bad);
    CHECK(ok > 100 && ok < 200 && mapper.mapping_count() == MmioMapper::kMaxMappings);
  }

  // ---- BARs
  {
    FakeCpu cpu(4);
    TestAllocator alloc;
    kernel::PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    FakeRam ram;
    MmioMapper mapper(pt, ram);
    FakePci pci;
    FakePci::Function nic{};
    nic.address = {0, 3, 0};
    nic.header_type = 0;
    nic.bars[0] = {0x0, 0x4000, 0xC0000000};                       // 32-bit, inside the firmware's map
    nic.bars[1] = {0x1, 0x20, 0xC000};                             // I/O
    nic.bars[2] = {0xC, 0x1000000, 0x600000000ull};              // 64-bit prefetchable, 16 MiB, above 4 GiB, not mapped
    nic.bars[3] = {0x0, 0, 0};                                     // (upper half of BAR 2)
    nic.bars[4] = {0x0, 0, 0};                                     // not implemented
    nic.bars[5] = {0x0, 0x2000, 0};                                // implemented, not assigned
    pci.functions.push_back(nic);
    FakePci::Function bridge{};
    bridge.address = {0, 4, 0};
    bridge.header_type = 1;
    bridge.bars[0] = {0x4, 0x100000, 0x700000000ull};            // 64-bit non-prefetchable, 1 MiB
    pci.functions.push_back(bridge);
    FakePci::Function other{};
    other.address = {1, 0, 0};
    other.header_type = 0x80;                                       // multifunction flag, header type 0
    other.bars[1] = {0x0, 0x100, 0xFEBD0000};
    pci.functions.push_back(other);

    PciBarInfo info;
    CHECK(pci_read_bar(pci, nic.address, 0, &info) && info.valid && !info.io && !info.is64 && !info.prefetchable && info.base == 0xC0000000 && info.size == 0x4000);
    CHECK(pci_read_bar(pci, nic.address, 1, &info) && info.valid && info.io && info.base == 0xC000);
    CHECK(pci_read_bar(pci, nic.address, 2, &info) && info.valid && info.is64 && info.prefetchable && info.base == 0x600000000ull && info.size == 16 * kMiB);
    CHECK(pci_read_bar(pci, nic.address, 4, &info) && !info.valid);
    CHECK(!pci_read_bar(pci, nic.address, 6, &info));
    CHECK(pci_read_bar(pci, bridge.address, 0, &info) && info.valid && info.is64 && !info.prefetchable && info.size == 1 * kMiB && info.base == 0x700000000ull);
    CHECK(!pci_read_bar(pci, bridge.address, 2, &info));    // a bridge has two BARs
    // sizing left everything as it was and switched decoding off while it ran
    FakePci::Function* f = pci.find(nic.address);
    CHECK(f->command == 0x0007 && f->status == 0x0210 && f->bars[2].base == 0x600000000ull && f->bars[0].base == 0xC0000000 && !f->decode_was_on_during_sizing);
    CHECK(pci.find(bridge.address)->bars[0].base == 0x700000000ull);

    MmioRegion region;
    CHECK(pci_map_bar(pci, mapper, nic.address, 0, MmioKind::Registers, &region) && region.address() == 0xC0000000 && region.length() == 0x4000);
    kernel::PageMapping m;
    CHECK(pt.query(0xC0000000, &m) && m.attributes.cache == kernel::CacheMode::Uncached && m.page_size == 4096);
    CHECK(!pci_map_bar(pci, mapper, nic.address, 1, MmioKind::Registers, &region));   // I/O
    CHECK(!pci_map_bar(pci, mapper, nic.address, 4, MmioKind::Registers, &region));   // not there
    CHECK(!pci_map_bar(pci, mapper, nic.address, 5, MmioKind::Registers, &region));   // not assigned
    CHECK(pci_map_bar(pci, mapper, nic.address, 2, MmioKind::FrameBuffer, &region) && region.address() == 0x600000000ull && region.length() == 16 * kMiB);
    CHECK(pt.query(0x600000000ull, &m) && m.present && m.attributes.cache == kernel::CacheMode::WriteCombining);

    // the whole machine at boot: only BARs that are not mapped get sized and mapped
    FakeCpu cpu2(4);
    kernel::PageTables pt2(cpu2, alloc);
    CHECK(pt2.initialize());
    MmioMapper mapper2(pt2, ram);
    pci.find(nic.address)->decode_was_on_during_sizing = false;
    CHECK(pci_map_all_bars(pci, mapper2) == 2);   // BAR 2 of the NIC and BAR 0 of the bridge; the others lie in the firmware's map or are I/O, unimplemented or unassigned
    CHECK(pt2.is_mapped(0x600000000ull, 16 * kMiB) && pt2.is_mapped(0x700000000ull, kMiB));
    CHECK(pt2.query(0x600000000ull, &m) && m.attributes.cache == kernel::CacheMode::WriteCombining);   // prefetchable and big
    CHECK(pt2.query(0x700000000ull, &m) && m.attributes.cache == kernel::CacheMode::Uncached);
    CHECK(pt2.query(0xC0000000, &m) && m.page_size == kGiB);                                           // the firmware's page stayed as it was
    CHECK(!pci.find(nic.address)->latched[0] && pci.find(nic.address)->command == 0x0007);
    CHECK(pci_map_all_bars(pci, mapper2) == 0);   // everything is mapped now
  }
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("mmio tests passed\n");
  return 0;
}
