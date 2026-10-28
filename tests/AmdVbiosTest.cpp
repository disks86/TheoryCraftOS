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

// AmdVbios against simulated sources: a VFCT table with images of several cards, an expansion ROM behind a ROM base register, and the SMC's ROM index/data registers. Checks the
// AtomBIOS image test, the VFCT walk (a match among several, truncation, a broken table), the expansion ROM chain size, the ROM base register protocol (probe, enable, read,
// disable), the register read (the header first, then the length it names), the order of the sources, and damaged input under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVbios.hpp"
#include "AtomBiosImage.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// A video BIOS image: the AtomBIOS image of the interpreter's tests, with the length in 512 byte units at byte 2.
static std::vector<uint8_t> atom_image(uint32_t units = 8) {
  atomimage::Image img;
  img.tables.push_back({0, 0, 0, atomimage::Code().eot()});
  std::vector<uint8_t> bytes = img.build();
  bytes.resize(units * 512, 0);
  bytes[2] = static_cast<uint8_t>(units);
  return bytes;
}

static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i)); }
static void put16(std::vector<uint8_t>& v, size_t at, uint32_t x) { v[at] = static_cast<uint8_t>(x); v[at + 1] = static_cast<uint8_t>(x >> 8); }

static void test_is_atom_bios() {
  const std::vector<uint8_t> good = atom_image();
  CHECK(AmdVbios::is_atom_bios(ConstByteView(good.data(), good.size())));
  CHECK(!AmdVbios::is_atom_bios(ConstByteView()) && !AmdVbios::is_atom_bios(ConstByteView(good.data(), 0x48)));
  std::vector<uint8_t> v = good;
  v[0] = 0x56;
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; v[1] = 0xAB;
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; put16(v, 0x48, 0);                                    // no header
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; put16(v, 0x48, 0); memcpy(&v[4], "ATOM", 4);          // no header, whatever is at offset 4
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; put16(v, 0x48, 0xFFF0);                               // header beyond the image
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; put16(v, 0x48, static_cast<uint32_t>(v.size() - 7));  // the four bytes at +4 do not fit
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; v[0x100 + 4] = 'X';
  CHECK(!AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  v = good; memcpy(&v[0x100 + 4], "MOTA", 4);                      // the byte swapped signature is accepted too
  CHECK(AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
  // the header at the very end: exactly four bytes left
  v = good; put16(v, 0x48, static_cast<uint32_t>(v.size() - 8)); memcpy(&v[v.size() - 4], "ATOM", 4);
  CHECK(AmdVbios::is_atom_bios(ConstByteView(v.data(), v.size())));
}

// A VFCT table: the ACPI header, the UUID, the offset of the first image at 0x34, then the images with their headers.
struct Entry { uint32_t bus, device, function; uint16_t vendor, dev_id; std::vector<uint8_t> image; uint32_t length_override = ~0u; };
static std::vector<uint8_t> make_vfct(const std::vector<Entry>& entries) {
  std::vector<uint8_t> t(0x4C, 0);
  memcpy(&t[0], "VFCT", 4);
  put32(t, 0x34, 0x4C);
  for (const Entry& e : entries) {
    std::vector<uint8_t> h(28, 0);
    put32(h, 0, e.bus); put32(h, 4, e.device); put32(h, 8, e.function);
    put16(h, 12, e.vendor); put16(h, 14, e.dev_id);
    put32(h, 24, e.length_override != ~0u ? e.length_override : static_cast<uint32_t>(e.image.size()));
    t.insert(t.end(), h.begin(), h.end());
    t.insert(t.end(), e.image.begin(), e.image.end());
  }
  put32(t, 4, static_cast<uint32_t>(t.size()));
  return t;
}

static void test_vfct() {
  const std::vector<uint8_t> a = atom_image(8), b = atom_image(4);
  std::vector<uint8_t> marked = b;
  marked[0x200] = 0x77;   // tells the two images apart
  const PciAddress vega{1, 0, 0};
  const std::vector<uint8_t> table = make_vfct({{0, 2, 0, 0x8086, 0x591B, a}, {1, 0, 0, 0x1002, 0x694C, marked}, {3, 0, 0, 0x1002, 0x6860, a}});
  ConstByteView tv(table.data(), table.size());
  ConstByteView image = AmdVbios::find_in_vfct(tv, vega, 0x1002, 0x694C);
  CHECK(image.size() == marked.size() && image.data() >= table.data() && image.data() + image.size() <= table.data() + table.size() && image[0x200] == 0x77);
  // The function and IDs must all match.
  CHECK(AmdVbios::find_in_vfct(tv, PciAddress{1, 0, 1}, 0x1002, 0x694C).size() == 0);
  CHECK(AmdVbios::find_in_vfct(tv, PciAddress{1, 1, 0}, 0x1002, 0x694C).size() == 0);
  CHECK(AmdVbios::find_in_vfct(tv, PciAddress{2, 0, 0}, 0x1002, 0x694C).size() == 0);
  CHECK(AmdVbios::find_in_vfct(tv, vega, 0x1003, 0x694C).size() == 0 && AmdVbios::find_in_vfct(tv, vega, 0x1002, 0x694D).size() == 0);
  CHECK(AmdVbios::find_in_vfct(tv, PciAddress{3, 0, 0}, 0x1002, 0x6860).size() == a.size());   // (the third)
  CHECK(AmdVbios::find_in_vfct(tv, PciAddress{0, 2, 0}, 0x8086, 0x591B).size() == a.size());   // (the first)
  // An entry with no image is skipped; one that matches but is no AtomBIOS image gives nothing (and does not fall through to a later one).
  const std::vector<uint8_t> empty_entry = make_vfct({{1, 0, 0, 0x1002, 0x694C, {}}, {1, 0, 0, 0x1002, 0x694C, marked}});
  CHECK(AmdVbios::find_in_vfct(ConstByteView(empty_entry.data(), empty_entry.size()), vega, 0x1002, 0x694C).size() == marked.size());
  std::vector<uint8_t> junk(2048, 0x5A);
  const std::vector<uint8_t> junk_table = make_vfct({{1, 0, 0, 0x1002, 0x694C, junk}, {1, 0, 0, 0x1002, 0x694C, marked}});
  CHECK(AmdVbios::find_in_vfct(ConstByteView(junk_table.data(), junk_table.size()), vega, 0x1002, 0x694C).size() == 0);
  // Broken tables: too short, an offset past the end, a header cut off, an image cut off, a length that wraps.
  CHECK(AmdVbios::find_in_vfct(ConstByteView(table.data(), 0x4B), vega, 0x1002, 0x694C).size() == 0 && AmdVbios::find_in_vfct(ConstByteView(), vega, 0x1002, 0x694C).size() == 0);
  std::vector<uint8_t> far = table;
  put32(far, 0x34, 0xFFFFFFF0);
  CHECK(AmdVbios::find_in_vfct(ConstByteView(far.data(), far.size()), vega, 0x1002, 0x694C).size() == 0);
  CHECK(AmdVbios::find_in_vfct(ConstByteView(table.data(), 0x4C + 10), vega, 0x1002, 0x694C).size() == 0);
  CHECK(AmdVbios::find_in_vfct(ConstByteView(table.data(), table.size() - 1), vega, 0x1002, 0x6860).size() == 0);
  const std::vector<uint8_t> wrap = make_vfct({{1, 0, 0, 0x1002, 0x694C, a, 0xFFFFFFF0}});
  CHECK(AmdVbios::find_in_vfct(ConstByteView(wrap.data(), wrap.size()), vega, 0x1002, 0x694C).size() == 0);
  // Every truncation and a bit flip in every byte of the headers: no read outside the table.
  for (size_t n = 0; n <= table.size(); n += 13) AmdVbios::find_in_vfct(ConstByteView(table.data(), n), vega, 0x1002, 0x694C);
  for (size_t at = 0; at < 0x4C + 28 * 2 + 64; ++at) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> bad = table;
      bad[at] ^= static_cast<uint8_t>(1u << bit);
      const ConstByteView r = AmdVbios::find_in_vfct(ConstByteView(bad.data(), bad.size()), vega, 0x1002, 0x694C);
      CHECK(r.size() == 0 || (r.data() >= bad.data() && r.data() + r.size() <= bad.data() + bad.size()));
    }
  }
}

static void test_rom_size() {
  // Images chained: each has a PCI data structure ("PCIR") at the offset in the word at 0x18, with the length in 512 byte units at +16 and the last-image flag in bit 7 of the byte at +21.
  auto image_at = [](std::vector<uint8_t>& w, size_t at, uint32_t units, bool last) {
    put16(w, at, 0xAA55);
    put16(w, at + 0x18, 0x40);
    memcpy(&w[at + 0x40], "PCIR", 4);
    put16(w, at + 0x40 + 16, units);
    w[at + 0x40 + 21] = last ? 0x80 : 0;
  };
  std::vector<uint8_t> w(8192, 0);
  image_at(w, 0, 2, false);
  image_at(w, 1024, 1, true);
  CHECK(AmdVbios::rom_size(ConstByteView(w.data(), w.size())) == 1536);
  std::vector<uint8_t> one(4096, 0);
  image_at(one, 0, 3, true);
  CHECK(AmdVbios::rom_size(ConstByteView(one.data(), one.size())) == 1536);
  // A window shorter than the images say: no more than the window. A first image that is not one: 0. A broken data structure: the images before it.
  CHECK(AmdVbios::rom_size(ConstByteView(one.data(), 1000)) <= 1000);
  CHECK(AmdVbios::rom_size(ConstByteView(w.data() + 2, 4000)) == 0);
  std::vector<uint8_t> bad_pcir = w;
  bad_pcir[1024 + 0x40] = 'X';
  CHECK(AmdVbios::rom_size(ConstByteView(bad_pcir.data(), bad_pcir.size())) == 1024);
  std::vector<uint8_t> zero_len = w;
  put16(zero_len, 0x40 + 16, 0);
  CHECK(AmdVbios::rom_size(ConstByteView(zero_len.data(), zero_len.size())) == 0);
  CHECK(AmdVbios::rom_size(ConstByteView()) == 0);
  // A first image without the signature is no image even if its data structure is fine; after the image flagged as the last nothing counts, however much it looks like another.
  std::vector<uint8_t> unsigned_first = one;
  put16(unsigned_first, 0, 0x1234);
  CHECK(AmdVbios::rom_size(ConstByteView(unsigned_first.data(), unsigned_first.size())) == 0);
  std::vector<uint8_t> trailing(8192, 0);
  image_at(trailing, 0, 2, true);
  image_at(trailing, 1024, 2, true);
  CHECK(AmdVbios::rom_size(ConstByteView(trailing.data(), trailing.size())) == 1024);
  std::vector<uint8_t> far_pcir = w;
  put16(far_pcir, 0x18, 0xFFF0);
  CHECK(AmdVbios::rom_size(ConstByteView(far_pcir.data(), far_pcir.size())) == 0);
  // an endless chain (a length of 0xFFFF units and no last flag) stops at the window
  std::vector<uint8_t> endless = w;
  put16(endless, 0x40 + 16, 0xFFFF);
  CHECK(AmdVbios::rom_size(ConstByteView(endless.data(), endless.size())) == endless.size());
}

// The card's registers as the SMC's ROM index/data pair: writing the index register selects a register, and the data register then reads the ROM a dword at a time.
struct RomRegs final : IVcnRegisters {
  std::vector<uint8_t> rom;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint32_t index = 0, position = 0;
  bool rom_selected = false;
  uint32_t reads = 0;
  uint32_t read(uint32_t r) override {
    if (r != AmdVbios::kSmcData) return 0;
    ++reads;
    if (index != AmdVbios::kRomData) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t{position + i < rom.size() ? rom[position + i] : uint8_t{0xFF}} << (8 * i);
    position += 4;
    return v;
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r == AmdVbios::kSmcIndex) index = v;
    if (r == AmdVbios::kSmcData && index == AmdVbios::kRomIndex) position = v;   // ROM_INDEX: the byte position
  }
};

static void test_rom_registers() {
  TestAllocator alloc;
  const std::vector<uint8_t> image = atom_image(8);
  RomRegs regs;
  regs.rom = image;
  regs.rom.resize(image.size() + 4096, 0xEE);   // (more ROM behind it: not read)
  ByteBuffer out(alloc);
  CHECK(AmdVbios::read_from_rom_registers(regs, alloc, &out) && out.view().equals(ConstByteView(image.data(), image.size())));
  // The protocol: index <- ROM_INDEX, data <- 0, index <- ROM_DATA, then reads; twice (the header, then the image).
  const std::vector<std::pair<uint32_t, uint32_t>> expected = {{0x1AC, 0xc0600010}, {0x1AD, 0}, {0x1AC, 0xc0600014}, {0x1AC, 0xc0600010}, {0x1AD, 0}, {0x1AC, 0xc0600014}};
  CHECK(regs.writes == expected);
  CHECK(regs.reads == 15 + image.size() / 4);   // the header (0x3C bytes: 15 dwords), then the whole image
  // No ROM (all ones), a wrong signature, no AMD string, a length that is zero or too small.
  RomRegs none;
  none.rom.assign(8192, 0xFF);
  ByteBuffer o1(alloc);
  CHECK(!AmdVbios::read_from_rom_registers(none, alloc, &o1));
  for (int variant = 0; variant < 4; ++variant) {
    RomRegs r;
    r.rom = image;
    if (variant == 0) r.rom[0] = 0x00;
    if (variant == 1) r.rom[0x30] = 'X';
    if (variant == 2) r.rom[2] = 0;
    if (variant == 3) r.rom[2] = 1;     // 512 bytes: too short to hold the header the check needs? (it is long enough to read, but the ATOM header is at 0x100: fine) -> not an error by length
    ByteBuffer o(alloc);
    const bool ok = AmdVbios::read_from_rom_registers(r, alloc, &o);
    CHECK(variant == 3 ? ok == AmdVbios::is_atom_bios(ConstByteView(r.rom.data(), 512)) : !ok);
  }
  // The image is no AtomBIOS image after all.
  RomRegs plain;
  plain.rom = image;
  plain.rom[0x100 + 4] = 'X';
  ByteBuffer o2(alloc);
  CHECK(!AmdVbios::read_from_rom_registers(plain, alloc, &o2) && o2.size() == 0);
}

// The expansion ROM: a PCI function whose ROM base register works like a BAR (writing ones reads back the size mask), and a mapper that hands out host memory.
struct RomPci final : IPciBus {
  uint32_t rom_register = 0;
  uint32_t size_mask = 0;           // the writable bits of the base: ~(size - 1) & 0xFFFFF800
  std::vector<uint32_t> writes;     // every write to the ROM base register, in order
  uint32_t other_accesses = 0;
  const char* name() const override { return "pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override {
    if (offset == 0x30) return rom_register;
    const_cast<RomPci*>(this)->other_accesses++;
    return 0;
  }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    RomPci& self = const_cast<RomPci&>(*this);
    if (offset != 0x30) { self.other_accesses++; return; }
    self.writes.push_back(value);
    self.rom_register = (value & (size_mask | 1)) | (value & 0);
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct RomMapper final : IRegisterMapper {
  std::vector<uint8_t> memory;      // the ROM as the window shows it
  bool refuse = false;
  uint64_t asked_physical = 0, asked_length = 0;
  RomPci* pci = nullptr;
  bool enabled_when_mapped = false;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    asked_physical = physical;
    asked_length = length;
    enabled_when_mapped = pci && (pci->rom_register & 1);
    if (refuse || length > memory.size()) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(memory.data()), length);
    return true;
  }
};

static void test_rom_bar() {
  TestAllocator alloc;
  const PciAddress fn{1, 0, 0};
  // A 128 KiB window at 0xE0000000 holding the video BIOS image first and zeros behind it; the image is chained as a PCI expansion ROM image of 8 units.
  std::vector<uint8_t> image = atom_image(8);
  put16(image, 0x18, 0x40);
  memcpy(&image[0x40], "PCIR", 4);
  put16(image, 0x40 + 16, 8);
  image[0x40 + 21] = 0x80;
  // (the first bytes are shared with the AtomBIOS image: 0x18 and 0x40 are inside its ROM header area, the test image does not use them)
  CHECK(AmdVbios::is_atom_bios(ConstByteView(image.data(), image.size())));
  RomMapper mapper;
  mapper.memory.assign(128 * 1024, 0);
  memcpy(mapper.memory.data(), image.data(), image.size());
  RomPci pci;
  mapper.pci = &pci;
  pci.rom_register = 0xE0000000;
  pci.size_mask = 0xFFFE0000;      // 128 KiB
  ByteBuffer out(alloc);
  CHECK(AmdVbios::read_from_rom_bar(pci, fn, mapper, alloc, &out));
  CHECK(out.view().equals(ConstByteView(image.data(), image.size())));    // the images only, not the 128 KiB
  // The protocol: probe with ones, restore (disabled), enable at the base, disable at the base; mapped the window of the probed size at the base; enabled while the data was read.
  const std::vector<uint32_t> expected = {0xFFFFF800, 0xE0000000, 0xE0000001, 0xE0000000};
  CHECK(pci.writes == expected && pci.other_accesses == 0);
  CHECK(mapper.asked_physical == 0xE0000000 && mapper.asked_length == 128 * 1024);
  CHECK(pci.rom_register == 0xE0000000);
  // The firmware left the ROM switched on: the probe and the restore switch it off (only the base is written back), and it is off at the end.
  RomPci left_on;
  left_on.rom_register = 0xE0000001;
  left_on.size_mask = 0xFFFE0000;
  RomMapper mapper2 = mapper;
  mapper2.pci = &left_on;
  CHECK(AmdVbios::read_from_rom_bar(left_on, fn, mapper2, alloc, &out));
  CHECK(left_on.writes == expected && left_on.rom_register == 0xE0000000);
  // No base assigned: nothing is touched.
  RomPci unassigned;
  unassigned.size_mask = 0xFFFE0000;
  CHECK(!AmdVbios::read_from_rom_bar(unassigned, fn, mapper, alloc, &out) && unassigned.writes.empty());
  // A register that does not take a size (reads back zero), a mapper that refuses, a window that is too big, an image that is no good.
  RomPci no_size;
  no_size.rom_register = 0xE0000000;
  no_size.size_mask = 0;
  CHECK(!AmdVbios::read_from_rom_bar(no_size, fn, mapper, alloc, &out));
  RomMapper refuse = mapper;
  refuse.refuse = true;
  RomPci p2;
  p2.rom_register = 0xE0000000; p2.size_mask = 0xFFFE0000;
  CHECK(!AmdVbios::read_from_rom_bar(p2, fn, refuse, alloc, &out));
  RomPci huge;
  huge.rom_register = 0xE0000000; huge.size_mask = 0xF0000000;   // a 256 MiB window
  CHECK(!AmdVbios::read_from_rom_bar(huge, fn, mapper, alloc, &out));
  RomMapper big_mapper;
  big_mapper.memory.assign(16u << 20, 0);   // a 16 MiB window (over the limit of 8 MiB) that the mapper would give
  RomPci sixteen;
  sixteen.rom_register = 0xE0000000; sixteen.size_mask = 0xFF000000;
  CHECK(!AmdVbios::read_from_rom_bar(sixteen, fn, big_mapper, alloc, &out) && big_mapper.asked_length == 0);
  RomMapper blank = mapper;
  std::fill(blank.memory.begin(), blank.memory.end(), 0xFF);
  RomPci p3;
  p3.rom_register = 0xE0000000; p3.size_mask = 0xFFFE0000;
  CHECK(!AmdVbios::read_from_rom_bar(p3, fn, blank, alloc, &out));
  CHECK((p3.rom_register & 1) == 0);   // (the ROM is switched off again whatever was read)
  RomMapper notatom = mapper;
  notatom.memory[0x100 + 4] = 'X';
  RomPci p4;
  p4.rom_register = 0xE0000000; p4.size_mask = 0xFFFE0000;
  CHECK(!AmdVbios::read_from_rom_bar(p4, fn, notatom, alloc, &out));
}

static void test_find_order() {
  TestAllocator alloc;
  const PciAddress fn{1, 0, 0};
  const std::vector<uint8_t> from_vfct = atom_image(8), from_regs = atom_image(4);
  std::vector<uint8_t> marked = from_vfct;
  marked[0x300] = 0x11;
  const std::vector<uint8_t> table = make_vfct({{1, 0, 0, 0x1002, 0x694C, marked}});
  RomRegs regs;
  regs.rom = from_regs;
  AmdVbios::Sources s;
  s.vfct = ConstByteView(table.data(), table.size());
  s.registers = &regs;
  s.allocator = &alloc;
  s.function = fn;
  s.vendor_id = 0x1002;
  s.device_id = 0x694C;
  ByteBuffer out(alloc);
  // The VFCT first; nothing else is touched.
  CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::Vfct && out.size() == marked.size() && out.view()[0x300] == 0x11 && regs.writes.empty());
  // No entry for this card: the ROM registers.
  s.device_id = 0x6860;
  CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::RomRegisters && out.size() == from_regs.size());
  // The expansion ROM comes before the registers.
  RomMapper mapper;
  mapper.memory.assign(128 * 1024, 0);
  std::vector<uint8_t> bar_image = atom_image(8);
  put16(bar_image, 0x18, 0x40);
  memcpy(&bar_image[0x40], "PCIR", 4);
  put16(bar_image, 0x40 + 16, 8);
  bar_image[0x40 + 21] = 0x80;
  bar_image[0x300] = 0x22;
  memcpy(mapper.memory.data(), bar_image.data(), bar_image.size());
  RomPci pci;
  pci.rom_register = 0xE0000000; pci.size_mask = 0xFFFE0000;
  s.pci = &pci;
  s.mapper = &mapper;
  regs.writes.clear();
  CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::RomBar && out.view()[0x300] == 0x22 && regs.writes.empty());
  // Nothing: none, and the output is left empty-handed.
  AmdVbios::Sources none;
  none.allocator = &alloc;
  ByteBuffer o2(alloc);
  CHECK(AmdVbios::find(none, &o2) == AmdVbios::Source::None && o2.size() == 0);
  AmdVbios::Sources no_alloc;
  no_alloc.vfct = ConstByteView(table.data(), table.size());
  no_alloc.function = fn; no_alloc.vendor_id = 0x1002; no_alloc.device_id = 0x694C;
  // (the VFCT needs no allocator for the copy beyond the output's own)
  CHECK(AmdVbios::find(no_alloc, &o2) == AmdVbios::Source::Vfct);
  CHECK(AmdVbios::source_name(AmdVbios::Source::None) && AmdVbios::source_name(AmdVbios::Source::Vfct) && AmdVbios::source_name(AmdVbios::Source::RomBar) && AmdVbios::source_name(AmdVbios::Source::RomRegisters));
}

int main() {
  test_is_atom_bios();
  test_vfct();
  test_rom_size();
  test_rom_registers();
  test_rom_bar();
  test_find_order();
  printf(g_failed ? "AmdVbiosTest: %d FAILED\n" : "AmdVbiosTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
