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
#include "Hardware/Acpi/AmlInterpreter.hpp"
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
  uint64_t only_at = 0;             // if not 0, the only physical address that can be mapped
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    asked_physical = physical;
    asked_length = length;
    if (only_at && physical != only_at) return false;
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

// --- The ATRM method: tables assembled byte by byte (a small AML assembler, as AmlTest has) ---------------------------------------------------------

namespace aml_asm {
typedef std::vector<uint8_t> Bytes;
static Bytes cat(std::initializer_list<Bytes> parts) { Bytes r; for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end()); return r; }
static Bytes b(std::initializer_list<uint8_t> v) { return Bytes(v); }
static Bytes path(const char* s) {
  Bytes r;
  if (*s == '\\') { r.push_back(0x5C); ++s; }
  std::vector<Bytes> segs;
  while (*s) {
    Bytes sg(4, '_');
    int n = 0;
    while (*s && *s != '.') sg[n++] = static_cast<uint8_t>(*s++);
    segs.push_back(sg);
    if (*s == '.') ++s;
  }
  if (segs.size() == 2) r.push_back(0x2E);
  else if (segs.size() > 2) { r.push_back(0x2F); r.push_back(static_cast<uint8_t>(segs.size())); }
  for (const Bytes& sg : segs) r.insert(r.end(), sg.begin(), sg.end());
  return r;
}
static Bytes pkg(const Bytes& body) {
  Bytes r;
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (n + 1 < 64) r.push_back(static_cast<uint8_t>(n + 1));
  else if (n + 2 < 4096) { const uint32_t t = n + 2; r.push_back(static_cast<uint8_t>(0x40 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); }
  else { const uint32_t t = n + 3; r.push_back(static_cast<uint8_t>(0x80 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); r.push_back(static_cast<uint8_t>(t >> 12)); }
  r.insert(r.end(), body.begin(), body.end());
  return r;
}
static Bytes num(uint64_t v) {
  if (v == 0) return b({0x00});
  if (v == 1) return b({0x01});
  if (v <= 0xFF) return b({0x0A, static_cast<uint8_t>(v)});
  if (v <= 0xFFFF) return b({0x0B, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
  return b({0x0C, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
}
static Bytes name_def(const char* n, const Bytes& value) { return cat({b({0x08}), path(n), value}); }
static Bytes method(const char* n, int argc, const Bytes& body) { return cat({b({0x14}), pkg(cat({path(n), b({static_cast<uint8_t>(argc)}), body}))}); }
static Bytes device(const char* n, const Bytes& body) { return cat({b({0x5B, 0x82}), pkg(cat({path(n), body}))}); }
static Bytes scope(const char* n, const Bytes& body) { return cat({b({0x10}), pkg(cat({path(n), body}))}); }
static Bytes buffer(const Bytes& size, const Bytes& init) { return cat({b({0x11}), pkg(cat({size, init}))}); }
static Bytes ret(const Bytes& v) { return cat({b({0xA4}), v}); }
static Bytes if_(const Bytes& cond, const Bytes& body) { return cat({b({0xA0}), pkg(cat({cond, body}))}); }
static Bytes lequal(const Bytes& a, const Bytes& c) { return cat({b({0x93}), a, c}); }
static const Bytes kArg0 = b({0x68});
static Bytes make_table(const char* signature, const Bytes& aml) {
  Bytes t(36, 0);
  memcpy(&t[0], signature, 4);
  const uint32_t length = static_cast<uint32_t>(36 + aml.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  t[8] = 2;
  t.insert(t.end(), aml.begin(), aml.end());
  return t;
}
}  // namespace aml_asm

struct NoRegions final : acpi::IAmlHost {
  bool region_read(const acpi::RegionAccess&, uint64_t*) override { return false; }
  bool region_write(const acpi::RegionAccess&, uint64_t) override { return false; }
};

// The contents of a device that serves `image` through its ATRM method in pages of 4096 bytes (the first two pages are all the test images need; any other offset gives an empty buffer,
// and so does a request for other than 4096 bytes). `second` replaces the second page.
static aml_asm::Bytes atrm_body(const std::vector<uint8_t>& image, const std::vector<uint8_t>* second = nullptr) {
  using namespace aml_asm;
  const size_t first = image.size() < 4096 ? image.size() : 4096;
  const Bytes p0(image.begin(), image.begin() + first), p1 = second ? *second : Bytes(image.begin() + first, image.end());
  return cat({
      name_def("P0__", buffer(num(first), p0)),
      name_def("P1__", buffer(num(p1.size()), p1)),
      method("ATRM", 2, if_(lequal(b({0x69}), num(4096)),
                            cat({if_(lequal(kArg0, num(0)), ret(path("P0__"))), if_(lequal(kArg0, num(4096)), ret(path("P1__")))}))),
  });
}

static aml_asm::Bytes atrm_device(const char* name, uint32_t adr, const std::vector<uint8_t>& image, bool with_adr = true, const std::vector<uint8_t>* second = nullptr) {
  using namespace aml_asm;
  return device(name, cat({with_adr ? name_def("_ADR", num(adr)) : Bytes(), atrm_body(image, second)}));
}

static aml_asm::Bytes dsdt_of(std::initializer_list<aml_asm::Bytes> devices) {
  using namespace aml_asm;
  Bytes inner;
  for (const Bytes& d : devices) inner.insert(inner.end(), d.begin(), d.end());
  return make_table("DSDT", scope("\\_SB", device("PCI0", inner)));
}

static void test_atrm() {
  TestAllocator alloc;
  using namespace aml_asm;
  const PciAddress card{1, 1, 0};   // _ADR 0x00010000
  std::vector<uint8_t> small = atom_image(4);          // 2048 bytes: one short page
  std::vector<uint8_t> big = atom_image(12);           // 6144 bytes: a page and a short one
  big[0x1800 - 5] = 0x77;                              // (in the second page)
  small[0x300] = 0x33;
  big[0x300] = 0x44;
  NoRegions host;
  ByteBuffer out(alloc);
  auto expect_image = [&](const std::vector<uint8_t>& image) {
    if (out.size() != AmdVbios::kAtrmBiosBytes) return false;
    for (size_t i = 0; i < image.size(); ++i) if (out.view()[i] != image[i]) return false;
    for (size_t i = image.size(); i < out.size(); ++i) if (out.view()[i] != 0) return false;
    return true;
  };
  {
    // a short page, then a page and a short one; padded with zeros to the 256 KiB the driver allocates
    for (const std::vector<uint8_t>* image : {&small, &big}) {
      const Bytes table = dsdt_of({atrm_device("GFX0", 0x10000, *image)});
      acpi::AmlInterpreter vm(alloc, host);
      CHECK(vm.load_table(ConstByteView(table.data(), table.size())));
      out.clear();
      CHECK(AmdVbios::read_from_atrm(vm, card, alloc, &out) && expect_image(*image));
    }
  }
  {
    // two devices have the method: the one whose _ADR is the card's wins, whatever the order; a card with no match takes the first
    std::vector<uint8_t> other = atom_image(4);
    other[0x300] = 0x55;
    const Bytes table = dsdt_of({atrm_device("IGP0", 0x20000, other), atrm_device("DGP0", 0x10000, small)});
    acpi::AmlInterpreter vm(alloc, host);
    CHECK(vm.load_table(ConstByteView(table.data(), table.size())));
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm, card, alloc, &out) && expect_image(small));
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm, PciAddress{1, 2, 0}, alloc, &out) && expect_image(other));
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm, PciAddress{1, 3, 0}, alloc, &out) && expect_image(other));   // (no match: the first)
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm, PciAddress{1, 1, 1}, alloc, &out) && expect_image(other));   // (the function counts)
    // a device without an _ADR is only taken when nothing matches
    const Bytes table2 = dsdt_of({atrm_device("NOAD", 0, other, false), atrm_device("DGP0", 0x10000, small)});
    acpi::AmlInterpreter vm2(alloc, host);
    CHECK(vm2.load_table(ConstByteView(table2.data(), table2.size())));
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm2, card, alloc, &out) && expect_image(small));
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm2, PciAddress{1, 5, 0}, alloc, &out) && expect_image(other));
  }
  {
    // a name called ATRM in front of the real method is passed over; a device that has no _ADR of its own does not take its parent's; the second page is not read after a short one
    std::vector<uint8_t> other = atom_image(4);
    other[0x300] = 0x55;
    const std::vector<uint8_t> extra(4, 0xEE);
    const Bytes tables[] = {
        dsdt_of({device("NAMD", cat({name_def("_ADR", num(0x10000)), name_def("ATRM", num(5))})), atrm_device("GFX0", 0x10000, small)}),
        dsdt_of({device("PAR0", cat({name_def("_ADR", num(0x10000)), atrm_device("NOAD", 0, other, false)})), atrm_device("DGP0", 0x10000, small)}),
        dsdt_of({atrm_device("GFX0", 0x10000, small, true, &extra)}),
    };
    for (const Bytes& table : tables) {
      acpi::AmlInterpreter vm(alloc, host);
      CHECK(vm.load_table(ConstByteView(table.data(), table.size())));
      out.clear();
      CHECK(AmdVbios::read_from_atrm(vm, card, alloc, &out) && expect_image(small));
    }
  }
  {
    // nothing to call: no method (a name called ATRM is not one), a method that fails, one that returns what is not a buffer, one that returns too much, a BIOS that is not one
    using namespace aml_asm;
    const std::vector<uint8_t> junk(2048, 0x5A);
    const Bytes tables[] = {
        dsdt_of({device("GFX0", cat({name_def("_ADR", num(0x10000)), name_def("ATRM", num(5))}))}),
        dsdt_of({device("GFX0", cat({name_def("_ADR", num(0x10000)), method("ATRM", 2, ret(path("NOPE")))}))}),
        dsdt_of({device("GFX0", cat({name_def("_ADR", num(0x10000)), method("ATRM", 2, ret(num(7)))}))}),
        dsdt_of({device("GFX0", cat({name_def("_ADR", num(0x10000)), method("ATRM", 2, ret(buffer(num(8192), Bytes())))}))}),
        dsdt_of({atrm_device("GFX0", 0x10000, junk)}),
        make_table("DSDT", scope("\\_SB", device("PCI0", Bytes()))),
        // (the method on something that is not a device: not found)
        make_table("DSDT", scope("\\_SB", cat({device("PCI0", Bytes()), atrm_body(small)}))),
    };
    for (const Bytes& table : tables) {
      acpi::AmlInterpreter vm(alloc, host);
      CHECK(vm.load_table(ConstByteView(table.data(), table.size())));
      out.clear();
      CHECK(!AmdVbios::read_from_atrm(vm, card, alloc, &out) && out.size() == 0);
    }
    // a method that gives full pages for ever: 64 of them, and an image
    std::vector<uint8_t> full = atom_image(8);   // 4096 bytes
    const Bytes forever = make_table("DSDT", scope("\\_SB", device("PCI0", device("GFX0", cat({name_def("_ADR", num(0x10000)), name_def("P0__", buffer(num(4096), Bytes(full.begin(), full.end()))),
                                                                                           method("ATRM", 2, ret(path("P0__")))})))));
    acpi::AmlInterpreter vm(alloc, host);
    CHECK(vm.load_table(ConstByteView(forever.data(), forever.size())));
    out.clear();
    CHECK(AmdVbios::read_from_atrm(vm, card, alloc, &out) && out.size() == AmdVbios::kAtrmBiosBytes);
    for (size_t i = 0; i < full.size(); ++i) if (out.view()[i] != full[i]) { CHECK(false); break; }
    for (size_t i = full.size(); i < out.size(); ++i) if (out.view()[i] != full[i % full.size()]) { CHECK(false); break; }
    // no table at all
    acpi::AmlInterpreter empty(alloc, host);
    out.clear();
    CHECK(!AmdVbios::read_from_atrm(empty, card, alloc, &out));
  }
  {
    // damage: every truncation and flipped bit of the table that holds the method, and nothing is read out of bounds (and an image that does come out is one)
    const Bytes table = dsdt_of({atrm_device("GFX0", 0x10000, small)});
    for (size_t len = 0; len <= table.size(); len += 3) {
      acpi::AmlInterpreter vm(alloc, host);
      vm.load_table(ConstByteView(table.data(), len));
      out.clear();
      if (AmdVbios::read_from_atrm(vm, card, alloc, &out)) CHECK(AmdVbios::is_atom_bios(out.view()));
    }
    Bytes flipped = table;
    for (size_t at = 0; at < 160 && at < flipped.size(); ++at) {
      for (int bit = 0; bit < 8; ++bit) {
        flipped[at] ^= static_cast<uint8_t>(1 << bit);
        acpi::AmlInterpreter vm(alloc, host);
        vm.load_table(ConstByteView(flipped.data(), flipped.size()));
        out.clear();
        if (AmdVbios::read_from_atrm(vm, card, alloc, &out)) CHECK(AmdVbios::is_atom_bios(out.view()));
        flipped[at] ^= static_cast<uint8_t>(1 << bit);
      }
    }
  }
}

// An image of the shape a shadowed legacy ROM has: 128 KiB, the video BIOS first, its PCI data structure naming the card.
static std::vector<uint8_t> shadow_memory(uint16_t vendor, uint16_t device, uint8_t marker = 0) {
  std::vector<uint8_t> memory(0x20000, 0);
  const std::vector<uint8_t> image = atom_image(8);
  memcpy(memory.data(), image.data(), image.size());
  put16(memory, 0x18, 0x40);
  memcpy(&memory[0x40], "PCIR", 4);
  put16(memory, 0x44, vendor);
  put16(memory, 0x46, device);
  put16(memory, 0x40 + 16, 8);
  memory[0x40 + 21] = 0x80;
  memory[0x300] = marker;
  return memory;
}

static void test_legacy_shadow() {
  TestAllocator alloc;
  ByteBuffer out(alloc);
  RomMapper mapper;
  mapper.only_at = 0xC0000;
  mapper.memory = shadow_memory(0x1002, 0x694C, 0x99);
  // The card's own image: the whole 128 KiB, as the driver copies romlen bytes.
  CHECK(AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1002, 0x694C, &out) && out.size() == 0x20000 && out.view()[0x300] == 0x99);
  CHECK(mapper.asked_physical == 0xC0000 && mapper.asked_length == 0x20000);
  CHECK(out.view().equals(ConstByteView(mapper.memory.data(), mapper.memory.size())));
  // Someone else's: the integrated graphics' image is no use, whichever of the two IDs differs.
  out.clear();
  CHECK(!AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1002, 0x6860, &out) && out.size() == 0);
  CHECK(!AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x8086, 0x694C, &out) && out.size() == 0);
  CHECK(!AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1003, 0x694D, &out) && out.size() == 0);
  // A range that cannot be mapped, or that is smaller.
  mapper.refuse = true;
  CHECK(!AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1002, 0x694C, &out));
  mapper.refuse = false;
  mapper.only_at = 0xD0000;
  CHECK(!AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1002, 0x694C, &out));
  mapper.only_at = 0xC0000;
  // No shadow at all (all ones, as a bus with nothing answers; zeros), no PCI data structure, one that points out of the window, one that is not "PCIR", not an AtomBIOS image.
  const std::vector<uint8_t> good = shadow_memory(0x1002, 0x694C);
  for (int variant = 0; variant < 7; ++variant) {
    mapper.memory = good;
    switch (variant) {
      case 0: mapper.memory.assign(0x20000, 0xFF); break;
      case 1: mapper.memory.assign(0x20000, 0); break;
      case 2: put16(mapper.memory, 0x18, 0); break;
      case 3: put16(mapper.memory, 0x18, 0xFFFF); break;
      case 4: mapper.memory[0x40] = 'X'; break;
      case 5: mapper.memory[0x48] = 0; mapper.memory[0x49] = 0; break;   // (the BIOS header pointer)
      case 6: mapper.memory[0] = 0x54; break;
    }
    out.clear();
    CHECK(!AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1002, 0x694C, &out) && out.size() == 0);
  }
  // rom_ids: the two IDs, and the refusals
  uint16_t vendor = 0, device = 0;
  CHECK(AmdVbios::rom_ids(ConstByteView(good.data(), good.size()), &vendor, &device) && vendor == 0x1002 && device == 0x694C);
  CHECK(!AmdVbios::rom_ids(ConstByteView(good.data(), 25), &vendor, &device) && !AmdVbios::rom_ids(ConstByteView(), &vendor, &device));
  CHECK(!AmdVbios::rom_ids(ConstByteView(good.data(), 0x47), &vendor, &device) && AmdVbios::rom_ids(ConstByteView(good.data(), 0x48), &vendor, &device));
  std::vector<uint8_t> unsigned_rom = good;
  unsigned_rom[1] = 0xAB;
  CHECK(!AmdVbios::rom_ids(ConstByteView(unsigned_rom.data(), unsigned_rom.size()), &vendor, &device));
  // Damage to the first bytes, every bit: no crash under the sanitizers, and what is accepted still is an image of this card.
  std::vector<uint8_t> flipped = good;
  for (size_t at = 0; at < 0x60; ++at) {
    for (int bit = 0; bit < 8; ++bit) {
      flipped[at] ^= static_cast<uint8_t>(1 << bit);
      mapper.memory = flipped;
      out.clear();
      if (AmdVbios::read_from_legacy_shadow(mapper, alloc, 0x1002, 0x694C, &out)) CHECK(AmdVbios::is_atom_bios(out.view()));
      flipped[at] ^= static_cast<uint8_t>(1 << bit);
    }
  }
}

// --- The disabled ROM path ---------------------------------------------------------------------------------------------------------------------

// A card whose ROM answers only when the registers the driver clears are cleared (BUS_CNTL bit 1, the VGA modes and timing select of the first two controllers, the VGA render status
// select, and ROM_CNTL bit 1 set through the SMC index and data pair). All numbers are literal here.
struct LockedCard final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;    // by dword, in order (the SMC's ROM_CNTL as 0xc0600000)
  uint32_t smc_index = 0;
  uint32_t get(uint32_t r) const { auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }
  bool unlocked(bool display_engine) const {
    if (get(0x1508) & 2) return false;
    if (!(get(0xc0600000) & 2)) return false;
    if (display_engine && ((get(0xcc) & 0x101) || (get(0xce) & 0x101) || (get(0xc0) & 0x30000))) return false;
    return true;
  }
  uint32_t read(uint32_t r) override {
    if (r == 0x1AD) return get(smc_index);
    return get(r);
  }
  void write(uint32_t r, uint32_t v) override {
    if (r == 0x1AC) { smc_index = v; return; }
    if (r == 0x1AD) { writes.push_back({smc_index, v}); regs[smc_index] = v; return; }
    writes.push_back({r, v});
    regs[r] = v;
  }
};

// The ROM window as the card shows it: the image while the card is unlocked at the time of the map and of every read, ones otherwise.
struct LockedWindow final : IRegisterMapper {
  LockedCard* card = nullptr;
  bool display_engine = true;
  std::vector<uint8_t> image, locked_view;
  uint32_t maps = 0;
  bool unlocked_at_map = false;
  bool map_registers(uint64_t, uint64_t length, MmioRegion* region) override {
    ++maps;
    unlocked_at_map = card->unlocked(display_engine);
    std::vector<uint8_t>& view = unlocked_at_map ? image : locked_view;
    if (length > view.size()) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(view.data()), length);
    return true;
  }
};

static void test_disabled_rom() {
  TestAllocator alloc;
  const PciAddress fn{1, 0, 0};
  std::vector<uint8_t> image = atom_image(8);
  put16(image, 0x18, 0x40);
  memcpy(&image[0x40], "PCIR", 4);
  put16(image, 0x40 + 16, 8);
  image[0x40 + 21] = 0x80;
  for (int display = 0; display < 2; ++display) {
    LockedCard card;
    card.regs[0x1508] = 0xA3;          // ROM disabled (bit 1), and other bits
    card.regs[0xcc] = 0x00010101;      // VGA mode and timing select on, a bit beside them
    card.regs[0xce] = 0x00000101;
    card.regs[0xc0] = 0x00030007;
    card.regs[0xc0600000] = 0x00F0;    // (ROM_CNTL: bit 1 clear)
    LockedWindow window;
    window.card = &card;
    window.display_engine = display != 0;
    window.image.assign(128 * 1024, 0);
    memcpy(window.image.data(), image.data(), image.size());
    window.locked_view.assign(128 * 1024, 0xFF);
    RomPci pci;
    pci.rom_register = 0xE0000000;
    pci.size_mask = 0xFFFE0000;
    ByteBuffer out(alloc);
    // Locked, the plain ROM read finds nothing; the path unlocks it, reads it, and puts every register back.
    CHECK(!AmdVbios::read_from_rom_bar(pci, fn, window, alloc, &out));
    const std::map<uint32_t, uint32_t> before = card.regs;
    card.writes.clear();
    CHECK(AmdVbios::read_from_disabled_rom(card, display != 0, pci, fn, window, alloc, &out));
    CHECK(window.unlocked_at_map && out.view().equals(ConstByteView(image.data(), image.size())));
    CHECK(card.regs == before);
    // The writes, in order: the unlock (BUS_CNTL, the three display registers if the card has them, ROM_CNTL), then the restore in the same order.
    std::vector<std::pair<uint32_t, uint32_t>> expect;
    expect.push_back({0x1508, 0xA1});
    if (display) { expect.push_back({0xcc, 0x00010000}); expect.push_back({0xce, 0}); expect.push_back({0xc0, 0x00000007}); }
    expect.push_back({0xc0600000, 0x00F2});
    expect.push_back({0x1508, 0xA3});
    if (display) { expect.push_back({0xcc, 0x00010101}); expect.push_back({0xce, 0x101}); expect.push_back({0xc0, 0x00030007}); }
    expect.push_back({0xc0600000, 0x00F0});
    CHECK(card.writes == expect);
    // A card without display controllers: their registers are neither read nor written.
    if (!display) for (const auto& w : card.writes) CHECK(w.first != 0xcc && w.first != 0xce && w.first != 0xc0);
    // The ROM does not come even then (no base assigned): nothing is found, and the registers are still put back.
    RomPci unassigned;
    unassigned.size_mask = 0xFFFE0000;
    out.clear();
    card.writes.clear();
    CHECK(!AmdVbios::read_from_disabled_rom(card, display != 0, unassigned, fn, window, alloc, &out) && out.size() == 0 && card.regs == before);
    CHECK(card.writes.size() == expect.size());
  }
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
  // The ATRM method comes first of all (the driver's order), when the machine has the tables for it.
  {
    NoRegions host;
    using namespace aml_asm;
    std::vector<uint8_t> via_atrm = atom_image(4);
    via_atrm[0x300] = 0x66;
    const Bytes dsdt = dsdt_of({atrm_device("DGP0", 0x00000000, via_atrm)});
    acpi::AmlInterpreter vm(alloc, host);
    CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
    s.aml = &vm;
    s.pci = nullptr;
    s.mapper = nullptr;
    s.device_id = 0x694C;
    regs.writes.clear();
    CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::Atrm && out.view()[0x300] == 0x66 && regs.writes.empty());
    // a machine whose ATRM gives nothing falls through to the next source
    acpi::AmlInterpreter none_vm(alloc, host);
    s.aml = &none_vm;
    CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::Vfct && out.view()[0x300] == 0x11);
    s.aml = nullptr;
    s.pci = &pci;
    s.mapper = &mapper;
    s.device_id = 0x6860;
  }
  // The legacy shadow comes after the VFCT and before the expansion ROM: a card the VFCT does not know, with a shadow that names it, is read from the shadow; a shadow that names
  // another card is not read.
  {
    RomMapper shadow;
    shadow.only_at = 0xC0000;
    shadow.memory = shadow_memory(0x1002, 0x6860, 0x88);
    s.mapper = &shadow;
    s.pci = nullptr;
    s.device_id = 0x6860;
    regs.writes.clear();
    CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::LegacyShadow && out.view()[0x300] == 0x88 && regs.writes.empty());
    s.device_id = 0x694C;   // (the VFCT has this one)
    CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::Vfct);
    s.device_id = 0x6861;   // (the shadow is of another card: the ROM registers)
    CHECK(AmdVbios::find(s, &out) == AmdVbios::Source::RomRegisters);
    s.mapper = &mapper;
    s.pci = &pci;
    s.device_id = 0x6860;
  }
  // The disabled ROM is the last resort: only when the other sources give nothing, the ROM base is assigned, and the ROM answers once unlocked.
  {
    std::vector<uint8_t> rom_image = atom_image(8);
    put16(rom_image, 0x18, 0x40);
    memcpy(&rom_image[0x40], "PCIR", 4);
    put16(rom_image, 0x40 + 16, 8);
    rom_image[0x40 + 21] = 0x80;
    rom_image[0x300] = 0x77;
    LockedCard card;
    card.regs[0x1508] = 2;
    LockedWindow window;
    window.card = &card;
    window.image.assign(128 * 1024, 0);
    memcpy(window.image.data(), rom_image.data(), rom_image.size());
    window.locked_view.assign(128 * 1024, 0xFF);
    RomPci locked_pci;
    locked_pci.rom_register = 0xE0000000;
    locked_pci.size_mask = 0xFFFE0000;
    AmdVbios::Sources last;
    last.allocator = &alloc;
    last.pci = &locked_pci;
    last.mapper = &window;
    last.registers = &card;
    last.function = fn;
    last.vendor_id = 0x1002;
    last.device_id = 0x6860;
    CHECK(AmdVbios::find(last, &out) == AmdVbios::Source::DisabledRom && out.view()[0x300] == 0x77);
    CHECK(card.regs[0x1508] == 2);   // (put back)
    last.registers = nullptr;
    out.clear();
    CHECK(AmdVbios::find(last, &out) == AmdVbios::Source::None && out.size() == 0);
    last.registers = &card;
    last.pci = nullptr;
    CHECK(AmdVbios::find(last, &out) == AmdVbios::Source::None);
    last.pci = &locked_pci;
    last.mapper = nullptr;
    CHECK(AmdVbios::find(last, &out) == AmdVbios::Source::None);
  }
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
  CHECK(AmdVbios::source_name(AmdVbios::Source::None) && AmdVbios::source_name(AmdVbios::Source::Atrm) && AmdVbios::source_name(AmdVbios::Source::LegacyShadow) && AmdVbios::source_name(AmdVbios::Source::DisabledRom) && AmdVbios::source_name(AmdVbios::Source::Vfct) && AmdVbios::source_name(AmdVbios::Source::RomBar) && AmdVbios::source_name(AmdVbios::Source::RomRegisters));
}


int main() {
  test_is_atom_bios();
  test_vfct();
  test_rom_size();
  test_rom_registers();
  test_rom_bar();
  test_legacy_shadow();
  test_disabled_rom();
  test_find_order();
  test_atrm();
  printf(g_failed ? "AmdVbiosTest: %d FAILED\n" : "AmdVbiosTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
