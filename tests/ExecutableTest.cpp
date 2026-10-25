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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Executable/ElfFile.hpp"
#include "Executable/PeFile.hpp"

using namespace exe;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;
typedef std::vector<uint8_t> Bytes;

static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }
static std::string str(StringView v) { return std::string(v.data(), v.size()); }

// Writes little- or big-endian values at an offset of the buffer (growing it).
struct Put {
  Bytes& b;
  bool big;
  void at(uint64_t off, uint64_t v, int n) {
    if (b.size() < off + n) b.resize(off + n);
    for (int i = 0; i < n; ++i) b[off + i] = uint8_t(v >> (8 * (big ? n - 1 - i : i)));
  }
  void u8(uint64_t o, uint64_t v) { at(o, v, 1); }
  void u16(uint64_t o, uint64_t v) { at(o, v, 2); }
  void u32(uint64_t o, uint64_t v) { at(o, v, 4); }
  void u64(uint64_t o, uint64_t v) { at(o, v, 8); }
  void text(uint64_t o, const char* s, size_t n) {
    if (b.size() < o + n) b.resize(o + n);
    for (size_t i = 0; i < n; ++i) b[o + i] = uint8_t(s[i]);
  }
};
#define TEXT(p, off, lit) (p).text(off, lit, sizeof(lit))      /* with the NUL */

// An x86-64 shared object with an interpreter, three dynamic strings, one symbol and five sections (800 bytes).
static Bytes make_elf64() {
  Bytes b(800, 0);
  Put p{b, false};
  p.text(0, "\x7F" "ELF", 4);
  p.u8(4, 2); p.u8(5, 1); p.u8(6, 1);
  p.u16(16, 3); p.u16(18, 62); p.u32(20, 1); p.u64(24, 0x401000); p.u64(32, 64); p.u64(40, 480);
  p.u32(48, 0); p.u16(52, 64); p.u16(54, 56); p.u16(56, 3); p.u16(58, 64); p.u16(60, 5); p.u16(62, 4);
  // program headers: INTERP, LOAD, DYNAMIC
  auto ph = [&](uint64_t i, uint32_t type, uint32_t flags, uint64_t off, uint64_t vaddr, uint64_t size) {
    const uint64_t o = 64 + i * 56;
    p.u32(o, type); p.u32(o + 4, flags); p.u64(o + 8, off); p.u64(o + 16, vaddr); p.u64(o + 24, vaddr); p.u64(o + 32, size); p.u64(o + 40, size); p.u64(o + 48, 8);
  };
  ph(0, kPtInterp, kPfRead, 232, 0x400000 + 232, 28);
  ph(1, kPtLoad, kPfRead | kPfExecute, 0, 0x400000, 800);
  ph(2, kPtDynamic, kPfRead | kPfWrite, 296, 0x400000 + 296, 96);
  TEXT(p, 232, "/lib64/ld-linux-x86-64.so.2");
  const char dynstr[] = "\0libc.so.6\0libm.so.6\0libx.so.1\0main";
  p.text(260, dynstr, sizeof(dynstr));      // 36 bytes with the final NUL
  auto dyn = [&](uint64_t i, uint64_t tag, uint64_t value) { p.u64(296 + i * 16, tag); p.u64(296 + i * 16 + 8, value); };
  dyn(0, kDtNeeded, 1); dyn(1, kDtNeeded, 11); dyn(2, kDtSoname, 21); dyn(3, kDtStrtab, 0x400000 + 260); dyn(4, 10, 36); dyn(5, kDtNull, 0);
  // .symtab: null + "main"
  p.u32(392 + 24, 31); p.u8(392 + 24 + 4, 0x12); p.u8(392 + 24 + 5, 0); p.u16(392 + 24 + 6, 1); p.u64(392 + 24 + 8, 0x401000); p.u64(392 + 24 + 16, 16);
  const char shstr[] = "\0.text\0.symtab\0.strtab\0.shstrtab";
  p.text(440, shstr, sizeof(shstr));      // 33 bytes
  auto sh = [&](uint64_t i, uint32_t name, uint32_t type, uint64_t flags, uint64_t addr, uint64_t off, uint64_t size, uint32_t link, uint64_t entsize) {
    const uint64_t o = 480 + i * 64;
    p.u32(o, name); p.u32(o + 4, type); p.u64(o + 8, flags); p.u64(o + 16, addr); p.u64(o + 24, off); p.u64(o + 32, size); p.u32(o + 40, link); p.u64(o + 56, entsize);
  };
  sh(1, 1, kShtProgbits, kShfAlloc | kShfExecInstr, 0x400000 + 232, 232, 4, 0, 0);
  sh(2, 7, kShtSymtab, 0, 0, 392, 48, 3, 24);
  sh(3, 15, kShtStrtab, 0, 0, 260, 36, 0, 0);
  sh(4, 23, kShtStrtab, 0, 0, 440, 33, 0, 0);
  return b;
}

// A 32-bit big-endian (PowerPC) executable with one segment and no sections (84 bytes).
static Bytes make_elf32_be() {
  Bytes b(84, 0);
  Put p{b, true};
  p.text(0, "\x7F" "ELF", 4);
  p.u8(4, 1); p.u8(5, 2); p.u8(6, 1);
  p.u16(16, 2); p.u16(18, 20); p.u32(20, 1); p.u32(24, 0x10000074); p.u32(28, 52); p.u32(32, 0);
  p.u16(40, 52); p.u16(42, 32); p.u16(44, 1); p.u16(46, 40);
  p.u32(52, kPtLoad); p.u32(56, 0); p.u32(60, 0x10000000); p.u32(64, 0x10000000); p.u32(68, 84); p.u32(72, 4096); p.u32(76, kPfRead | kPfExecute); p.u32(80, 4096);
  return b;
}

static void test_elf64() {
  Bytes file = make_elf64();
  CHECK(ElfFile::looks_like_elf(view(file)));
  ElfFile e(A);
  CHECK(e.parse(view(file)));
  CHECK(e.elf_class() == ElfClass::Elf64 && e.data_encoding() == ElfData::Little && e.is_64());
  CHECK(e.type() == kElfShared && e.machine() == 62 && e.entry() == 0x401000);
  CHECK(e.segments().count() == 3);
  if (e.segments().count() == 3) {
    CHECK(e.segments()[1].type == kPtLoad && e.segments()[1].file_size == 800 && e.segments()[1].vaddr == 0x400000);
    CHECK(e.segments()[1].flags == (kPfRead | kPfExecute));
  }
  CHECK(str(e.interpreter()) == "/lib64/ld-linux-x86-64.so.2");
  CHECK(e.needed().count() == 2);
  if (e.needed().count() == 2) CHECK(str(e.needed()[0].view()) == "libc.so.6" && str(e.needed()[1].view()) == "libm.so.6");
  CHECK(str(e.soname()) == "libx.so.1" && e.run_path().empty());
  CHECK(e.sections().count() == 5);
  const ElfSection* text = e.find_section(".text");
  CHECK(text && text->type == kShtProgbits && (text->flags & kShfExecInstr) && text->offset == 232 && text->size == 4);
  CHECK(e.find_section(".shstrtab") != nullptr && e.find_section(".nope") == nullptr);
  ConstByteView bytes;
  CHECK(text && ElfFile::section_data(view(file), *text, &bytes) && bytes.size() == 4);
  const ElfSymbol* main_symbol = e.find_symbol("main");
  CHECK(main_symbol && main_symbol->value == 0x401000 && main_symbol->size == 16 && main_symbol->type() == 2 && main_symbol->bind() == 1 && !main_symbol->dynamic);
  CHECK(e.find_symbol("missing") == nullptr);

  // Parsing again replaces everything.
  CHECK(e.parse(view(file)) && e.segments().count() == 3 && e.needed().count() == 2);
  // A wrong file does not leave half a parse behind.
  Bytes bad = file;
  bad[0] = 'X';
  CHECK(!e.parse(view(bad)) && e.sections().count() == 0 && e.segments().count() == 0);
  CHECK(!e.parse(ConstByteView()));
}

static void test_elf32_be() {
  Bytes file = make_elf32_be();
  ElfFile e(A);
  CHECK(e.parse(view(file)));
  CHECK(e.elf_class() == ElfClass::Elf32 && e.data_encoding() == ElfData::Big && !e.is_64());
  CHECK(e.type() == kElfExecutable && e.machine() == 20 && e.entry() == 0x10000074);
  CHECK(e.segments().count() == 1 && e.sections().count() == 0);
  if (e.segments().count() == 1) {
    const ElfSegment& s = e.segments()[0];
    CHECK(s.type == kPtLoad && s.vaddr == 0x10000000 && s.file_size == 84 && s.mem_size == 4096 && s.flags == (kPfRead | kPfExecute) && s.align == 4096);
  }
}

static void test_elf_hostile() {
  Bytes file = make_elf64();
  ElfFile e(A);
  // Every cut-off copy fails (the section header table is the last thing in the file) ...
  for (size_t len = 0; len < file.size(); ++len) CHECK(!e.parse(ConstByteView(file.data(), len)));
  // ... and no single flipped bit makes the parser read outside the file (ASan/UBSan watch; the result may be either).
  for (size_t i = 0; i < file.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes copy = file;
      copy[i] ^= uint8_t(1 << bit);
      e.parse(view(copy));
    }
  // Huge counts and offsets do not wrap.
  Bytes huge = file;
  Put p{huge, false};
  p.u64(32, ~uint64_t{0} - 8);      // e_phoff
  CHECK(!e.parse(view(huge)));
  huge = file;
  p.u64(40, ~uint64_t{0} - 8);      // e_shoff
  CHECK(!e.parse(view(huge)));
  huge = file;
  p.u16(60, 0xFFFF);                // e_shnum beyond the file
  e.parse(view(huge));
}

static const uint64_t kPeBase = 0x180000000ull;

// A PE32+ DLL: .text and .rdata, exports (two named, one forwarded, one unused slot) and imports (two from kernel32, one by ordinal from user32).
static Bytes make_pe64() {
  Bytes b(0x800, 0);
  Put p{b, true};
  p.big = false;
  p.text(0, "MZ", 2);
  p.u32(0x3C, 0x40);
  p.text(0x40, "PE\0\0", 4);
  p.u16(0x44, 0x8664); p.u16(0x46, 2); p.u32(0x48, 0x12345678); p.u16(0x54, 240); p.u16(0x56, kPeExecutableImage | kPeDll);
  const uint64_t o = 0x58;
  p.u16(o, 0x20B); p.u32(o + 16, 0x1000); p.u32(o + 20, 0x1000); p.u64(o + 24, kPeBase); p.u32(o + 32, 0x1000); p.u32(o + 36, 0x200);
  p.u32(o + 56, 0x3000); p.u32(o + 60, 0x200); p.u16(o + 68, 3); p.u32(o + 108, 16);
  // directories: exports and imports
  p.u32(o + 112, 0x2000); p.u32(o + 116, 0x140);
  p.u32(o + 120, 0x2200); p.u32(o + 124, 60);
  auto section = [&](uint64_t i, const char* name, uint32_t vsize, uint32_t va, uint32_t rawsize, uint32_t raw, uint32_t flags) {
    const uint64_t s = 0x148 + i * 40;
    p.text(s, name, 8);
    p.u32(s + 8, vsize); p.u32(s + 12, va); p.u32(s + 16, rawsize); p.u32(s + 20, raw); p.u32(s + 36, flags);
  };
  section(0, ".text\0\0", 0x100, 0x1000, 0x200, 0x200, kPeSectionCode | kPeSectionExecute | kPeSectionRead);
  section(1, ".rdata\0", 0x400, 0x2000, 0x400, 0x400, kPeSectionRead);
  // .rdata at file offset 0x400 = rva 0x2000
  auto at = [](uint32_t rva) { return uint64_t{0x400} + (rva - 0x2000); };
  p.u32(at(0x2000) + 12, 0x2100); p.u32(at(0x2000) + 16, 1); p.u32(at(0x2000) + 20, 3); p.u32(at(0x2000) + 24, 2);
  p.u32(at(0x2000) + 28, 0x2040); p.u32(at(0x2000) + 32, 0x2050); p.u32(at(0x2000) + 36, 0x2060);
  p.u32(at(0x2040), 0x1000); p.u32(at(0x2040) + 4, 0); p.u32(at(0x2040) + 8, 0x2110);
  p.u32(at(0x2050), 0x2120); p.u32(at(0x2050) + 4, 0x2130);
  p.u16(at(0x2060), 0); p.u16(at(0x2060) + 2, 2);
  TEXT(p, at(0x2100), "test.dll"); TEXT(p, at(0x2110), "other.Func"); TEXT(p, at(0x2120), "Alpha"); TEXT(p, at(0x2130), "Gamma");
  // imports
  p.u32(at(0x2200), 0x2280); p.u32(at(0x2200) + 12, 0x2300); p.u32(at(0x2200) + 16, 0x22A0);
  p.u32(at(0x2214), 0x22C0); p.u32(at(0x2214) + 12, 0x2320); p.u32(at(0x2214) + 16, 0x22C0);
  p.u64(at(0x2280), 0x2340); p.u64(at(0x2288), 0x2360);
  p.u64(at(0x22A0), 0x2340); p.u64(at(0x22A8), 0x2360);
  p.u64(at(0x22C0), 0x8000000000000005ull);
  TEXT(p, at(0x2300), "kernel32.dll"); TEXT(p, at(0x2320), "user32.dll");
  p.u16(at(0x2340), 7); TEXT(p, at(0x2342), "ExitProcess");
  p.u16(at(0x2360), 1); TEXT(p, at(0x2362), "GetLastError");
  return b;
}

static void test_pe64() {
  Bytes file = make_pe64();
  CHECK(PeFile::looks_like_pe(view(file)));
  PeFile pe(A);
  CHECK(pe.parse(view(file)));
  CHECK(pe.is_pe32_plus() && pe.machine() == kPeMachineAmd64 && pe.is_dll() && pe.timestamp() == 0x12345678);
  CHECK(pe.entry_rva() == 0x1000 && pe.image_base() == kPeBase && pe.section_alignment() == 0x1000 && pe.file_alignment() == 0x200);
  CHECK(pe.size_of_image() == 0x3000 && pe.size_of_headers() == 0x200 && pe.subsystem() == kPeSubsystemConsole && pe.directory_count() == 16);
  uint32_t rva = 0, size = 0;
  CHECK(pe.directory(1, &rva, &size) && rva == 0x2200 && size == 60);
  CHECK(!pe.directory(16, &rva, &size));
  CHECK(pe.sections().count() == 2 && pe.directories_ok());
  const PeSection* text = pe.find_section(".text");
  CHECK(text && text->virtual_address == 0x1000 && text->raw_offset == 0x200 && (text->characteristics & kPeSectionExecute));
  CHECK(pe.find_section(".rdata") != nullptr && pe.find_section(".data") == nullptr);

  uint64_t offset = 0;
  CHECK(pe.rva_to_offset(0x1010, &offset) && offset == 0x210);
  CHECK(pe.rva_to_offset(0x10, &offset) && offset == 0x10);      // in the headers
  CHECK(!pe.rva_to_offset(0x1200, &offset));                     // after .text's raw data, before .rdata
  CHECK(!pe.rva_to_offset(0x9000, &offset));

  CHECK(str(pe.export_dll_name()) == "test.dll");
  CHECK(pe.exports().count() == 2);
  if (pe.exports().count() == 2) {
    const PeExport& a = pe.exports()[0];
    CHECK(a.ordinal == 1 && a.rva == 0x1000 && str(a.name.view()) == "Alpha" && a.forwarder.empty());
    const PeExport& g = pe.exports()[1];
    CHECK(g.ordinal == 3 && str(g.name.view()) == "Gamma" && str(g.forwarder.view()) == "other.Func");
  }
  CHECK(pe.find_export("Gamma") != nullptr && pe.find_export("Beta") == nullptr);

  CHECK(pe.imports().count() == 3);
  if (pe.imports().count() == 3) {
    CHECK(str(pe.imports()[0].dll.view()) == "kernel32.dll" && str(pe.imports()[0].name.view()) == "ExitProcess" && pe.imports()[0].hint == 7);
    CHECK(str(pe.imports()[1].name.view()) == "GetLastError" && pe.imports()[1].hint == 1 && !pe.imports()[1].by_ordinal);
    CHECK(str(pe.imports()[2].dll.view()) == "user32.dll" && pe.imports()[2].by_ordinal && pe.imports()[2].ordinal == 5 && pe.imports()[2].name.empty());
  }
}

static void test_pe32() {
  // Same header scheme with a 32-bit optional header (96 bytes + 16 directories) and no sections.
  Bytes b(0x200, 0);
  Put p{b, false};
  p.text(0, "MZ", 2); p.u32(0x3C, 0x40); p.text(0x40, "PE\0\0", 4);
  p.u16(0x44, kPeMachineI386); p.u16(0x46, 0); p.u16(0x54, 224); p.u16(0x56, kPeExecutableImage);
  const uint64_t o = 0x58;
  p.u16(o, 0x10B); p.u32(o + 16, 0x1234); p.u32(o + 28, 0x400000); p.u32(o + 32, 0x1000); p.u32(o + 36, 0x200);
  p.u32(o + 56, 0x2000); p.u32(o + 60, 0x200); p.u16(o + 68, 2); p.u32(o + 92, 16);
  PeFile pe(A);
  CHECK(pe.parse(view(b)));
  CHECK(!pe.is_pe32_plus() && pe.machine() == kPeMachineI386 && !pe.is_dll() && pe.entry_rva() == 0x1234 && pe.image_base() == 0x400000);
  CHECK(pe.subsystem() == kPeSubsystemGui && pe.directory_count() == 16 && pe.sections().count() == 0);
  b[0x58] = 0x07; b[0x59] = 0x01;      // ROM image
  CHECK(!pe.parse(view(b)));
}

static void test_pe_hostile() {
  Bytes file = make_pe64();
  PeFile pe(A);
  for (size_t len = 0; len < file.size(); ++len) CHECK(!pe.parse(ConstByteView(file.data(), len)));
  for (size_t i = 0; i < file.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes copy = file;
      copy[i] ^= uint8_t(1 << bit);
      pe.parse(view(copy));
    }
  // A lying export count, an unterminated import list and huge RVAs are reported through directories_ok(), not read.
  Bytes bad = file;
  Put p{bad, false};
  p.u32(0x400 + 20, 0xFFFFFFFFu);      // NumberOfFunctions
  CHECK(pe.parse(view(bad)) && !pe.directories_ok() && pe.exports().count() == 0);
  bad = file;
  p.u32(0x400 + 0x200 + 12, 0xFFFFFFF0u);      // first DLL's name RVA
  CHECK(pe.parse(view(bad)) && !pe.directories_ok() && pe.imports().count() == 0);
  bad = file;
  p.u32(0x3C, 0xFFFFFFFFu);      // e_lfanew
  CHECK(!PeFile::looks_like_pe(view(bad)) && !pe.parse(view(bad)));
  bad = file;
  p.u16(0x46, 0xFFFF);      // too many sections
  CHECK(!pe.parse(view(bad)));
}

int main() {
  test_elf64();
  test_elf32_be();
  test_elf_hostile();
  test_pe64();
  test_pe32();
  test_pe_hostile();
  CHECK(A.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("ExecutableTest: all passed\n");
  return 0;
}
