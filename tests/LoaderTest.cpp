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
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/UserPageTables.hpp"
#include "Process/UserProgram.hpp"
#include "Process/WindowsStartup.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { ++live; return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;
typedef std::vector<uint8_t> Bytes;
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

static void put(Bytes& b, uint64_t off, uint64_t v, int n) {
  if (b.size() < off + n) b.resize(off + n);
  for (int i = 0; i < n; ++i) b[off + i] = uint8_t(v >> (8 * i));
}
static void put_text(Bytes& b, uint64_t off, const std::string& s) {
  if (b.size() < off + s.size()) b.resize(off + s.size());
  for (size_t i = 0; i < s.size(); ++i) b[off + i] = uint8_t(s[i]);
}

static void test_address_space() {
  AddressSpace s(A, 64 * 1024);
  CHECK(s.map(0x10000, 100, kProtRead | kProtWrite));
  CHECK(s.mapped_bytes() == 4096 && s.region_count() == 1);
  CHECK(!s.map(0x10000, 4096, kProtRead));              // overlap
  CHECK(!s.map(0x10800, 4096, kProtRead));              // not page-aligned
  CHECK(!s.map(0x20000, 0, kProtRead));                 // empty
  CHECK(!s.map(AddressSpace::kUserTop, 4096, kProtRead));
  CHECK(!s.map(AddressSpace::kUserTop - 4096, 8192, kProtRead));
  CHECK(!s.map(0x20000, ~uint64_t{0}, kProtRead));      // size overflow
  CHECK(!s.map(0x20000, 128 * 1024, kProtRead));        // over the budget
  CHECK(s.map(0x11000, 4096, kProtRead));               // adjacent
  CHECK(s.map(0xF000, 4096, kProtRead | kProtWrite));   // adjacent below: kept sorted

  uint8_t out[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  CHECK(s.read(0x10000, ByteView(out, 8)) && out[0] == 0 && out[7] == 0);
  const uint8_t data[4] = {1, 2, 3, 4};
  CHECK(s.write(0x10FFE, ConstByteView(data, 4)) == false);      // runs into the read-only region
  uint8_t back[4] = {};
  CHECK(s.read(0x10FFE, ByteView(back, 4)) && back[0] == 0);       // and nothing was written
  CHECK(s.write(0xFFFE, ConstByteView(data, 4)));                  // crosses two writable regions
  CHECK(s.read(0xFFFE, ByteView(back, 4)) && back[0] == 1 && back[3] == 4);
  CHECK(s.read(0x10FFE, ByteView(back, 4)) && back[2] == 0);       // read crosses the read-only boundary
  CHECK(s.write_privileged(0x11000, ConstByteView(data, 4)) && s.read(0x11000, ByteView(back, 4)) && back[3] == 4);
  CHECK(!s.write(0x11000, ConstByteView(data, 4)));
  CHECK(!s.read(0x12000, ByteView(back, 1)));                      // unmapped
  CHECK(!s.read(0, ByteView(back, 1)));                            // page zero
  CHECK(!s.read(~uint64_t{0} - 1, ByteView(back, 4)));             // wraps
  CHECK(!s.read(AddressSpace::kUserTop - 1, ByteView(back, 2)));
  CHECK(s.read(0x99999, ByteView(back, 0)));                       // empty access never faults
  uint8_t prot = 0;
  CHECK(s.protection_at(0x11000, &prot) && prot == kProtRead && !s.protection_at(0x12000, &prot));

  CHECK(s.protect(0x11000, 4096, kProtRead | kProtWrite) && s.write(0x11000, ConstByteView(data, 4)));
  CHECK(!s.protect(0x10800, 4096, kProtRead));                     // misaligned
  CHECK(!s.protect(0x10000, 0x4000, kProtRead));                   // spans a gap

  uint64_t at = 0;
  CHECK(s.find_free(4096, 0xF000, &at) && at == 0x12000);          // the first hole above the contiguous run
  CHECK(s.find_free(4096, 0, &at) && at == 0x1000);                // never page zero
  CHECK(s.find_free(0x8000, 0x1000, &at) && at == 0x1000);         // fits below 0xF000
  CHECK(s.find_free(0x10000, 0x1000, &at) && at == 0x12000);       // too big for the low hole
  CHECK(!s.find_free(AddressSpace::kUserTop, 0, &at));
  CHECK(!s.find_free(0, 0, &at));

  CHECK(s.unmap(0xF000, 0x3000));                                  // three whole regions
  CHECK(s.region_count() == 0 && s.mapped_bytes() == 0);

  // Cutting regions: unmap the middle of one, protect part of another.
  CHECK(s.map(0x10000, 4096 * 4, kProtRead | kProtWrite));
  uint8_t mark[2] = {7, 9};
  CHECK(s.write(0x10FFF, ConstByteView(mark, 2)));                  // straddles pages 0 and 1
  CHECK(s.unmap(0x11000, 4096));
  CHECK(s.region_count() == 2 && s.mapped_bytes() == 3 * 4096);
  uint8_t one = 0;
  CHECK(s.read(0x10FFF, ByteView(&one, 1)) && one == 7 && !s.read(0x11000, ByteView(&one, 1)) && !s.read(0x10FFF, ByteView(back, 2)));
  CHECK(s.read(0x12000, ByteView(&one, 1)) && one == 0);
  CHECK(s.protect(0x12000, 4096, kProtRead) && !s.write(0x12000, ConstByteView(&one, 1)) && s.write(0x13000, ConstByteView(&one, 1)));
  CHECK(!s.protect(0x10000, 0x4000, kProtRead));                    // the hole is not protectable
  CHECK(s.unmap(0x30000, 4096 * 3));                                // nothing there: fine
  CHECK(s.unmap(0x10000, 0x10000) && s.region_count() == 0 && s.mapped_bytes() == 0);      // several regions and gaps at once
  CHECK(!s.unmap(0x10001, 4096) && !s.unmap(0x10000, 0) && !s.unmap(AddressSpace::kUserTop, 4096));

  // resize: a heap grows and shrinks, and new pages are zero.
  CHECK(s.map(0x20000, 4096, kProtRead | kProtWrite) && s.map(0x24000, 4096, kProtRead));
  CHECK(s.write(0x20000, ConstByteView(mark, 2)));
  CHECK(s.resize(0x20000, 4096 * 4) && s.mapped_bytes() == 5 * 4096);
  CHECK(!s.resize(0x20000, 4096 * 5));                              // would hit the region at 0x24000
  CHECK(s.read(0x20000, ByteView(&one, 1)) && one == 7 && s.read(0x23FFF, ByteView(&one, 1)) && one == 0);
  CHECK(s.resize(0x20000, 1) && s.mapped_bytes() == 2 * 4096 && !s.read(0x21000, ByteView(&one, 1)));
  CHECK(s.resize(0x20000, 4096 * 2) && s.read(0x20001, ByteView(&one, 1)) && one == 9 && s.read(0x21000, ByteView(&one, 1)) && one == 0);      // regrown pages are zero
  CHECK(!s.resize(0x20800, 4096) && !s.resize(0x50000, 4096));
  CHECK(!s.resize(0x20000, 128 * 1024));                            // over the budget
  CHECK(s.resize(0x20000, 0) && s.region_count() == 1);
  uint64_t rb = 0, rs = 0;
  CHECK(s.region_extent(0x24800, &rb, &rs) && rb == 0x24000 && rs == 4096 && !s.region_extent(0x25000, &rb, &rs));
}

// A static x86-64 executable: a code segment holding the program headers, and a data segment with a string and a .bss tail.
static Bytes make_elf(uint16_t type = 2, uint64_t entry = 0x400100) {
  Bytes b(0x1010, 0);
  put_text(b, 0, "\x7F" "ELF");
  b[4] = 2; b[5] = 1; b[6] = 1;
  put(b, 16, type, 2); put(b, 18, 62, 2); put(b, 20, 1, 4); put(b, 24, entry, 8); put(b, 32, 64, 8);
  put(b, 52, 64, 2); put(b, 54, 56, 2); put(b, 56, 2, 2); put(b, 58, 64, 2);
  auto ph = [&](int i, uint32_t type, uint32_t flags, uint64_t off, uint64_t vaddr, uint64_t filesz, uint64_t memsz) {
    const uint64_t o = 64 + 56 * i;
    put(b, o, type, 4); put(b, o + 4, flags, 4); put(b, o + 8, off, 8); put(b, o + 16, vaddr, 8); put(b, o + 24, vaddr, 8);
    put(b, o + 32, filesz, 8); put(b, o + 40, memsz, 8); put(b, o + 48, 0x1000, 8);
  };
  ph(0, 1, 5, 0, type == 3 ? 0 : 0x400000, 0x1000, 0x1000);
  ph(1, 1, 6, 0x1000, type == 3 ? 0x201000 : 0x601000, 6, 0x2000);
  put_text(b, 0x1000, "hello\n");
  put_text(b, 0x100, "\xC3");
  return b;
}

struct Parsed {
  exe::ElfFile elf{A};
  Bytes file;
  explicit Parsed(Bytes f) : file(static_cast<Bytes&&>(f)) {}
  bool parse() { return elf.parse(view(file)); }
};

struct FakeHost final : IProcessHost {
  std::string out;
  bool write_console(uint32_t, ConstByteView d) override { for (uint8_t c : d) out.push_back(char(c)); return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
};

static void test_load_elf() {
  Parsed p(make_elf());
  CHECK(p.parse());
  AddressSpace s(A);
  LoadedImage image;
  CHECK(load_elf(p.elf, view(p.file), s, &image) == LoadStatus::Ok);
  CHECK(image.base == 0x400000 && image.end == 0x603000 && image.entry == 0x400100 && image.phdr == 0x400040 && image.phnum == 2 && image.phent == 56);
  uint8_t prot = 0;
  CHECK(s.protection_at(0x400000, &prot) && prot == (kProtRead | kProtExecute));
  CHECK(s.protection_at(0x601000, &prot) && prot == (kProtRead | kProtWrite));
  uint8_t text[6] = {}, tail = 1, code = 0;
  CHECK(s.read(0x601000, ByteView(text, 6)) && std::string((char*)text, 6) == "hello\n");
  CHECK(s.read(0x602FFF, ByteView(&tail, 1)) && tail == 0);                      // .bss
  CHECK(s.read(0x400100, ByteView(&code, 1)) && code == 0xC3);
  CHECK(!s.write(0x400100, ConstByteView(&code, 1)));                            // code is not writable
  CHECK(!s.read(0x1000, ByteView(&code, 1)));

  // End to end: a Linux process writes a string out of the loaded image.
  ProcessBinding binding;
  CHECK(bind_executable(p.elf, &binding));
  FakeHost host;
  Process proc(1, binding, s, host);
  SyscallRequest r;
  r.number = 1; r.args[0] = 1; r.args[1] = 0x601000; r.args[2] = 6;
  CHECK(proc.syscall(r) == 6 && host.out == "hello\n");

  // Position independent: moved to the PIE base.
  Parsed pie(make_elf(3, 0x100));
  CHECK(pie.parse());
  AddressSpace s2(A);
  CHECK(load_elf(pie.elf, view(pie.file), s2, &image) == LoadStatus::Ok && image.entry == kElfPieBase + 0x100 && image.base == kElfPieBase);
  CHECK(s2.read(kElfPieBase + 0x201000, ByteView(text, 6)) && text[0] == 'h');

  // A neighbouring segment on the same page shares one region with merged permissions.
  Bytes shared = make_elf();
  put(shared, 64 + 56 + 16, 0x400800, 8); put(shared, 64 + 56 + 24, 0x400800, 8);       // second segment now starts inside the first page
  put(shared, 64 + 56 + 8, 0x800, 8);
  put(shared, 64 + 32, 0x700, 8); put(shared, 64 + 40, 0x700, 8);                        // and the first ends before it
  Parsed sh(shared);
  CHECK(sh.parse());
  AddressSpace s3(A);
  CHECK(load_elf(sh.elf, view(sh.file), s3, &image) == LoadStatus::Ok && s3.region_count() == 1 && s3.protection_at(0x400000, &prot) && prot == 7);
}

static void test_load_elf_rejects() {
  LoadedImage image;
  auto status = [&](const Bytes& b) {
    exe::ElfFile elf(A);
    if (!elf.parse(view(b))) return LoadStatus::Malformed;
    AddressSpace s(A, 1 << 20);
    return load_elf(elf, view(b), s, &image);
  };
  CHECK(status(make_elf()) == LoadStatus::Ok);
  Bytes b = make_elf(); put(b, 64 + 32, 0x2000, 8);                   // file size runs past the end of the file
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 64 + 56 + 32, 0x3000, 8);                    // file size larger than the memory size
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 64 + 56 + 16, 0x400800, 8);                  // overlaps the first segment
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 24, 0x601000, 8);                            // entry in non-executable memory
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 24, 0x10, 8);                                // entry unmapped
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 64 + 16, 0, 8);                              // segment at page zero
  CHECK(status(b) != LoadStatus::Ok);
  b = make_elf(); put(b, 64 + 56 + 16, 0x7FFFFFFFF000ull, 8); put(b, 64 + 56 + 40, 0x4000, 8);       // runs past the top of user space
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 64 + 56 + 16, ~uint64_t{0} - 100, 8);        // address near 2^64
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 64 + 40, 0x10000000, 8); put(b, 64 + 56 + 16, 0x20000000, 8);       // 256 MiB: over the 1 MiB budget
  CHECK(status(b) == LoadStatus::OutOfMemory);
  b = make_elf(); put(b, 64, 4 /*PT_NOTE*/, 4); put(b, 64 + 56, 4, 4);   // no loadable segment
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_elf(); put(b, 64, 3 /*PT_INTERP*/, 4); put(b, 64 + 8, 0x1000, 8); put(b, 64 + 32, 6, 8);
  CHECK(status(b) != LoadStatus::Ok);                                  // a program that wants a dynamic loader

  // Flip every bit of the headers: whatever happens, nothing is read or written out of bounds.
  Bytes base = make_elf();
  for (size_t i = 0; i < 64 + 56 * 2; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes f = base; f[i] ^= uint8_t(1 << bit);
      (void)status(f);
    }
  }
  // Truncated files.
  for (size_t n : {size_t(0), size_t(10), size_t(63), size_t(64), size_t(120), size_t(0x100), size_t(0x1000), size_t(0x1005)}) {
    Bytes f = make_elf(); f.resize(n);
    (void)status(f);
  }
}

static Bytes make_pe(uint64_t base_high = 1, bool with_tls = false) {
  Bytes b(0x400, 0);
  put_text(b, 0, "MZ"); put(b, 0x3C, 0x40, 4);
  put_text(b, 0x40, std::string("PE\0\0", 4));
  put(b, 0x44, 0x8664, 2); put(b, 0x46, 1, 2); put(b, 0x54, 240, 2); put(b, 0x56, 0x0002, 2);
  const uint64_t o = 0x58;
  put(b, o, 0x20B, 2); put(b, o + 16, 0x1000, 4); put(b, o + 24, base_high << 32, 8); put(b, o + 32, 0x1000, 4); put(b, o + 36, 0x200, 4);
  put(b, o + 56, 0x2000, 4); put(b, o + 60, 0x200, 4); put(b, o + 68, 3, 2); put(b, o + 108, 16, 4);
  const uint64_t s = 0x148;
  put_text(b, s, ".text"); put(b, s + 8, 0x100, 4); put(b, s + 12, 0x1000, 4); put(b, s + 16, 0x200, 4); put(b, s + 20, 0x200, 4); put(b, s + 36, 0x60000020, 4);
  put_text(b, 0x200, "\xC3");
  if (with_tls) {
    // IMAGE_TLS_DIRECTORY64 at rva 0x1080: template [0x10C0, 0x10C8) = "TLSDATA!", index variable at 0x10D0, 8 bytes of zero fill.
    const uint64_t image_base = base_high << 32;
    put(b, o + 112 + 9 * 8, 0x1080, 4); put(b, o + 112 + 9 * 8 + 4, 0x28, 4);
    put(b, 0x280, image_base + 0x10C0, 8); put(b, 0x288, image_base + 0x10C8, 8); put(b, 0x290, image_base + 0x10D0, 8); put(b, 0x298, 0, 8); put(b, 0x2A0, 8, 4);
    put_text(b, 0x2C0, "TLSDATA!");
    put(b, 0x2D0, 0xFFFFFFFF, 4);
  }
  return b;
}

static void test_load_pe() {
  Bytes file = make_pe();
  exe::PeFile pe(A);
  CHECK(pe.parse(view(file)));
  AddressSpace s(A);
  LoadedImage image;
  CHECK(load_pe(pe, view(file), s, &image) == LoadStatus::Ok);
  CHECK(image.base == 0x100000000ull && image.entry == 0x100001000ull && image.end == 0x100002000ull);
  uint8_t prot = 0, byte = 0;
  CHECK(s.protection_at(0x100000000ull, &prot) && prot == kProtRead);
  CHECK(s.protection_at(0x100001000ull, &prot) && prot == (kProtRead | kProtExecute));
  CHECK(s.read(0x100001000ull, ByteView(&byte, 1)) && byte == 0xC3);
  CHECK(s.read(0x100000000ull, ByteView(&byte, 1)) && byte == 'M');
  CHECK(s.read(0x100001080ull, ByteView(&byte, 1)) && byte == 0);       // virtual size beyond the raw data is zero

  ProcessBinding binding;
  CHECK(bind_executable(pe, &binding) && binding.abi == SyscallAbi::Windows);

  auto status = [&](const Bytes& b) {
    exe::PeFile p(A);
    if (!p.parse(view(b))) return LoadStatus::Malformed;
    AddressSpace sp(A, 1 << 20);
    return load_pe(p, view(b), sp, &image);
  };
  Bytes b = make_pe(); put(b, 0x58 + 16, 0, 4);                          // no entry point
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_pe(); put(b, 0x58 + 16, 0x1800, 4);                           // entry in the zero tail of the section's last page: mapped, so accepted
  CHECK(status(b) == LoadStatus::Ok);
  b = make_pe(); put(b, 0x58 + 16, 0x3000, 4);                           // entry beyond the image
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_pe(); put(b, 0x148 + 12, 0x1800, 4);                          // section not page aligned
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_pe(); put(b, 0x148 + 12, 0x0, 4);                             // section over the headers
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_pe(); put(b, 0x148 + 8, 0x5000, 4);                           // bigger than SizeOfImage
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_pe(0x8000); put(b, 0x58 + 24, 0xFFFFFFFFFFFFF000ull, 8);     // base beyond user space
  CHECK(status(b) == LoadStatus::Malformed);
  b = make_pe(); put(b, 0x58 + 32, 0x200, 4);                            // 512-byte section alignment
  CHECK(status(b) == LoadStatus::Unsupported);
  Bytes base = make_pe();
  for (size_t i = 0; i < 0x148 + 40; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes f = base; f[i] ^= uint8_t(1 << bit);
      (void)status(f);
    }
  }
  for (size_t n : {size_t(0), size_t(0x40), size_t(0x100), size_t(0x150), size_t(0x200), size_t(0x300)}) {
    Bytes f = make_pe(); f.resize(n);
    (void)status(f);
  }
}

static uint64_t word(AddressSpace& s, uint64_t at) {
  uint8_t raw[8] = {};
  CHECK(s.read(at, ByteView(raw, 8)));
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i);
  return v;
}
static std::string cstr(AddressSpace& s, uint64_t at) {
  std::string out;
  for (;;) {
    uint8_t c = 0;
    CHECK(s.read(at++, ByteView(&c, 1)));
    if (!c || out.size() > 200) return out;
    out.push_back(char(c));
  }
}

static void test_stacks() {
  AddressSpace s(A);
  uint64_t top = 0;
  CHECK(map_stack(s, kDefaultStackBytes, &top) == LoadStatus::Ok && top == AddressSpace::kUserTop - 4096);
  uint8_t byte = 0;
  CHECK(s.read(top - 1, ByteView(&byte, 1)) && !s.read(top, ByteView(&byte, 1)));       // guard page above
  CHECK(map_stack(s, kDefaultStackBytes, &top) == LoadStatus::OutOfMemory);              // already there
  AddressSpace tiny(A, 8192);
  CHECK(map_stack(tiny, 1 << 20, &top) == LoadStatus::OutOfMemory);
  CHECK(map_stack(tiny, 0, &top) == LoadStatus::Malformed);
  CHECK(map_stack(tiny, ~uint64_t{0}, &top) == LoadStatus::Malformed);

  LoadedImage image;
  image.entry = 0x400100; image.phdr = 0x400040; image.phent = 56; image.phnum = 2;
  const StringView args[] = {"prog", "-x"};
  const StringView env[] = {"HOME=/", "A=b"};
  uint8_t random[16];
  for (int i = 0; i < 16; ++i) random[i] = uint8_t(i + 1);
  CHECK(top == AddressSpace::kUserTop - 4096);
  uint64_t sp = 0;
  CHECK(build_linux_stack(s, top, image, ArrayView<const StringView>(args, 2), ArrayView<const StringView>(env, 2), "/bin/prog", ConstByteView(random, 16), &sp) == LoadStatus::Ok);
  CHECK(sp % 16 == 0 && sp < top);
  CHECK(word(s, sp) == 2);
  CHECK(cstr(s, word(s, sp + 8)) == "prog" && cstr(s, word(s, sp + 16)) == "-x" && word(s, sp + 24) == 0);
  CHECK(cstr(s, word(s, sp + 32)) == "HOME=/" && cstr(s, word(s, sp + 40)) == "A=b" && word(s, sp + 48) == 0);
  uint64_t at = sp + 56, entry = 0, phdr = 0, rnd = 0, pagesz = 0, phnum = 0, execfn = 0, platform = 0, clktck = 0;
  for (int i = 0; i < 40; ++i, at += 16) {
    const uint64_t key = word(s, at), value = word(s, at + 8);
    if (key == 0) break;
    if (key == 9) entry = value; else if (key == 3) phdr = value; else if (key == 25) rnd = value; else if (key == 6) pagesz = value; else if (key == 5) phnum = value; else if (key == 31) execfn = value; else if (key == 15) platform = value; else if (key == 17) clktck = value;
  }
  CHECK(entry == 0x400100 && phdr == 0x400040 && pagesz == 4096 && phnum == 2 && clktck == 100);
  CHECK(cstr(s, execfn) == "/bin/prog" && cstr(s, platform) == "x86_64");
  uint8_t got[16] = {};
  CHECK(s.read(rnd, ByteView(got, 16)) && got[0] == 1 && got[15] == 16);

  // Limits.
  CHECK(build_linux_stack(s, top, image, ArrayView<const StringView>(args, 2), ArrayView<const StringView>(env, 2), "/bin/prog", ConstByteView(random, 8), &sp) == LoadStatus::Malformed);
  std::string huge(kMaxStartupStrings + 1, 'x');
  const StringView big[] = {StringView(huge.data(), huge.size())};
  CHECK(build_linux_stack(s, top, image, ArrayView<const StringView>(big, 1), ArrayView<const StringView>(), "/bin/prog", ConstByteView(random, 16), &sp) == LoadStatus::Malformed);
  AddressSpace small(A);
  CHECK(small.map(0x10000, 4096, kProtRead | kProtWrite));
  CHECK(build_linux_stack(small, 0x11000, image, ArrayView<const StringView>(args, 2), ArrayView<const StringView>(env, 2), "/bin/prog", ConstByteView(random, 16), &sp) == LoadStatus::Ok);
  CHECK(build_linux_stack(small, 0x20000, image, ArrayView<const StringView>(args, 2), ArrayView<const StringView>(), "/bin/prog", ConstByteView(random, 16), &sp) == LoadStatus::OutOfMemory);

  CHECK(windows_initial_stack_pointer(top) % 16 == 8 && top - windows_initial_stack_pointer(top) >= 0x28);
}

static SyscallRequest lin(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0) {
  SyscallRequest r;
  r.number = n; r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4;
  return r;
}
static int64_t sres(uint64_t v) { return int64_t(v); }

static void test_linux_memory() {
  AddressSpace s(A, 16 << 20);
  FakeHost host;
  ProcessBinding binding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)};
  Process p(1, binding, s, host);
  // Without an attached space the calls fail cleanly.
  CHECK(sres(p.syscall(lin(9, 0, 4096, 3, 0x22, ~uint64_t{0}))) == -12);
  p.attach_address_space(s, 0x700000);

  // brk
  CHECK(p.syscall(lin(12, 0)) == 0x700000);
  CHECK(p.syscall(lin(12, 0x700000 + 100)) == 0x700000 + 100);
  uint8_t b = 5;
  CHECK(s.write(0x700000, ConstByteView(&b, 1)));
  CHECK(p.syscall(lin(12, 0x700000 + 3 * 4096 + 1)) == 0x700000 + 3 * 4096 + 1);
  CHECK(s.read(0x700000 + 4 * 4096 - 1, ByteView(&b, 1)) && b == 0);
  CHECK(p.syscall(lin(12, 0x700000 + 5)) == 0x700000 + 5 && !s.read(0x701000, ByteView(&b, 1)));      // shrinks
  CHECK(p.syscall(lin(12, 0x1000)) == 0x700000 + 5);                    // below the start: unchanged
  CHECK(p.syscall(lin(12, 0x700000 + (1ull << 40))) == 0x700000 + 5);   // beyond the budget: unchanged
  CHECK(p.syscall(lin(12, 0x700000)) == 0x700000 && s.region_count() == 0);
  CHECK(p.syscall(lin(12, ~uint64_t{0})) == 0x700000);

  // mmap / munmap / mprotect
  const uint64_t anon = 0x22;      // MAP_PRIVATE | MAP_ANONYMOUS
  uint64_t a = p.syscall(lin(9, 0, 5000, 3, anon, ~uint64_t{0}));
  CHECK(int64_t(a) > 0 && a % 4096 == 0 && s.mapped_bytes() == 8192);
  CHECK(s.write(a + 5000 - 1, ConstByteView(&b, 1)));
  uint64_t a2 = p.syscall(lin(9, 0, 4096, 1, anon, ~uint64_t{0}));
  CHECK(a2 == a + 8192 && !s.write(a2, ConstByteView(&b, 1)));
  CHECK(p.syscall(lin(10, a2, 4096, 3)) == 0 && s.write(a2, ConstByteView(&b, 1)));
  CHECK(p.syscall(lin(11, a + 4096, 4096)) == 0 && s.mapped_bytes() == 8192 && !s.read(a + 4096, ByteView(&b, 1)));      // the middle goes
  CHECK(sres(p.syscall(lin(10, a, 3 * 4096, 1))) == -12);                // hole in the range
  CHECK(sres(p.syscall(lin(11, a + 1, 4096))) == -22);
  CHECK(sres(p.syscall(lin(11, a, 0))) == -22);
  CHECK(sres(p.syscall(lin(10, a, 4096, 0x100))) == -22);                // unknown protection bit
  CHECK(sres(p.syscall(lin(9, 0, 0, 3, anon, ~uint64_t{0}))) == -22);   // empty
  CHECK(sres(p.syscall(lin(9, 0, 4096, 3, 0x20, ~uint64_t{0}))) == -22); // neither shared nor private
  CHECK(sres(p.syscall(lin(9, 0, 4096, 3, 0x2, 3))) == -19);            // a file: not supported
  CHECK(sres(p.syscall(lin(9, 0, 4096, 0x80, anon, ~uint64_t{0}))) == -22);
  CHECK(sres(p.syscall(lin(9, 0, ~uint64_t{0}, 3, anon, ~uint64_t{0}))) == -22);    // length overflows the page rounding
  CHECK(sres(p.syscall(lin(9, 0, 1ull << 40, 3, anon, ~uint64_t{0}))) == -12);       // over the budget
  // MAP_FIXED replaces what is there.
  CHECK(p.syscall(lin(9, a, 4096, 1, anon | 0x10, ~uint64_t{0})) == a && !s.write(a, ConstByteView(&b, 1)));
  CHECK(sres(p.syscall(lin(9, a + 1, 4096, 1, anon | 0x10, ~uint64_t{0}))) == -22);
  CHECK(sres(p.syscall(lin(9, AddressSpace::kUserTop, 4096, 1, anon | 0x10, ~uint64_t{0}))) == -12);
  CHECK(sres(p.syscall(lin(11, ~uint64_t{0} - 4095, 4096))) == -22);
  // Position independent code from the loader keeps working next to the mappings.
  uint64_t a3 = p.syscall(lin(9, 0x700000, 4096, 3, anon, ~uint64_t{0}));
  CHECK(a3 == 0x700000);      // a hint at a free spot is honoured
  uint64_t a4 = p.syscall(lin(9, 0x700000, 4096, 3, anon, ~uint64_t{0}));
  CHECK(a4 == 0x701000);      // a taken hint moves up to the next free page
}

// The Windows virtual-memory services; five or six arguments, the rest on the stack.
static void test_windows_memory() {
  AddressSpace s(A, 16 << 20);
  FakeHost host;
  ProcessBinding binding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)};
  Process p(1, binding, s, host);
  CHECK(s.map(0x10000, 4096 * 4, kProtRead | kProtWrite));              // scratch for the arguments (and stack at 0x12000)
  const uint64_t kBaseSlot = 0x10000, kSizeSlot = 0x10008, kStack = 0x12000, kOld = 0x10020;
  auto rd = [&](uint64_t at) { uint8_t raw[8] = {}; s.read(at, ByteView(raw, 8)); uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i); return v; };
  auto wr = [&](uint64_t at, uint64_t v) { uint8_t raw[8]; for (int i = 0; i < 8; ++i) raw[i] = uint8_t(v >> (8 * i)); s.write(at, ConstByteView(raw, 8)); };
  auto call = [&](WindowsSyscallNumber n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t s4 = 0, uint64_t s5 = 0) {
    wr(kStack + 0x28, s4); wr(kStack + 0x30, s5);
    SyscallRequest r;
    r.number = uint64_t(n); r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.user_stack = kStack;
    return uint32_t(p.syscall(r));
  };
  const uint64_t self = ~uint64_t{0};
  // Allocate: nothing attached yet.
  wr(kBaseSlot, 0); wr(kSizeSlot, 5000);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x3000, 4) == 0xC0000017u);
  p.attach_address_space(s, 0);

  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x3000, 4) == 0);
  const uint64_t base = rd(kBaseSlot);
  CHECK(base % 0x10000 == 0 && rd(kSizeSlot) == 8192);
  uint8_t byte = 3;
  CHECK(s.write(base + 8191, ConstByteView(&byte, 1)));
  // Reserve, then commit part of it.
  wr(kBaseSlot, 0); wr(kSizeSlot, 0x4000);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x2000, 1) == 0);
  const uint64_t reserved = rd(kBaseSlot);
  CHECK(!s.write(reserved, ConstByteView(&byte, 1)));
  wr(kBaseSlot, reserved); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x1000, 4) == 0 && s.write(reserved, ConstByteView(&byte, 1)));
  CHECK(!s.write(reserved + 4096, ConstByteView(&byte, 1)));
  // An address that is taken by something that was not reserved by us in the same way.
  wr(kBaseSlot, 0x10000); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x2000, 4) == 0xC0000018u);      // reserve on top of mapped pages
  // Bad parameters.
  wr(kBaseSlot, 0); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, 5, kBaseSlot, 0, kSizeSlot, 0x3000, 4) == 0xC0000008u);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x3000, 0x1000) == 0xC0000045u);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0, 4) == 0xC000000Du);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, 0xDEAD0000, 0, kSizeSlot, 0x3000, 4) == 0xC0000005u);
  wr(kSizeSlot, 0);
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x3000, 4) == 0xC000000Du);
  wr(kSizeSlot, ~uint64_t{0});
  CHECK(call(WindowsSyscallNumber::NtAllocateVirtualMemory, self, kBaseSlot, 0, kSizeSlot, 0x3000, 4) == 0xC000000Du);

  // Protect: read-only, and the old protection is reported.
  wr(kBaseSlot, base); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtProtectVirtualMemory, self, kBaseSlot, kSizeSlot, 0x02, kOld) == 0);
  uint8_t oldp[4] = {};
  CHECK(s.read(kOld, ByteView(oldp, 4)) && oldp[0] == 0x04);
  CHECK(!s.write(base, ConstByteView(&byte, 1)) && s.write(base + 4096, ConstByteView(&byte, 1)));
  wr(kBaseSlot, 0x900000); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtProtectVirtualMemory, self, kBaseSlot, kSizeSlot, 0x02, kOld) == 0xC0000018u);      // unmapped
  wr(kBaseSlot, base);
  CHECK(call(WindowsSyscallNumber::NtProtectVirtualMemory, self, kBaseSlot, kSizeSlot, 0x7777, kOld) == 0xC0000045u);

  // NtQueryVirtualMemory (the service behind VirtualQuery): a committed region, a protected one, a reserved one, and free space.
  {
    const uint64_t info = 0x10100;
    const uint64_t junk = 0xABCD000000000000ull;
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, base + 5, 0 | junk, info, 48, 0) == 0);
    CHECK(rd(info) == base && rd(info + 8) == base && rd(info + 24) == 4096 && (rd(info + 32) & 0xFFFFFFFFu) == 0x1000 && (rd(info + 36) & 0xFFFFFFFFu) == 0x02);      // MEM_COMMIT, PAGE_READONLY: the page protect() made read-only
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, base + 4096, 0, info, 48, 0) == 0 && rd(info) == base + 4096 && (rd(info + 36) & 0xFFFFFFFFu) == 0x04 && rd(info + 24) == 4096);      // the rest of the allocation, read-write
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, reserved + 4096, 0, info, 48, 0) == 0 && (rd(info + 32) & 0xFFFFFFFFu) == 0x2000);      // reserved only
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, 0x7FFFFFF0000ull, 0, info, 48, 0) == 0 && (rd(info + 32) & 0xFFFFFFFFu) == 0x10000 && rd(info + 24) > 4096);      // MEM_FREE
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, base, 5, info, 48, 0) == 0xC0000003u);      // a class this does not have
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, base, 0, info, 47, 0) == 0xC0000004u);      // a buffer that is too small
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, self, ~uint64_t{0} - 5, 0, info, 48, 0) == 0xC000000Du);      // beyond user space
    CHECK(call(WindowsSyscallNumber::NtQueryVirtualMemory, 5, base, 0, info, 48, 0) == 0xC0000008u);
  }
  // Free: decommit, then release (size must be 0, base must be a region start).
  wr(kBaseSlot, base + 4096); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtFreeVirtualMemory, self, kBaseSlot, kSizeSlot, 0x4000) == 0 && !s.write(base + 4096, ConstByteView(&byte, 1)));
  wr(kBaseSlot, base); wr(kSizeSlot, 4096);
  CHECK(call(WindowsSyscallNumber::NtFreeVirtualMemory, self, kBaseSlot, kSizeSlot, 0x8000) == 0xC000000Du);      // release needs size 0
  wr(kBaseSlot, base + 4096); wr(kSizeSlot, 0);
  CHECK(call(WindowsSyscallNumber::NtFreeVirtualMemory, self, kBaseSlot, kSizeSlot, 0x8000) == 0xC000001Au);      // not the start of an allocation
  const uint64_t before = s.mapped_bytes();
  wr(kBaseSlot, base); wr(kSizeSlot, 0);
  CHECK(call(WindowsSyscallNumber::NtFreeVirtualMemory, self, kBaseSlot, kSizeSlot, 0x8000) == 0);                // both pieces, though protect() cut them apart
  CHECK(s.mapped_bytes() == before - 8192 && rd(kSizeSlot) == 8192 && !s.read(base, ByteView(&byte, 1)) && !s.read(base + 4096, ByteView(&byte, 1)));
  wr(kBaseSlot, 0x900000); wr(kSizeSlot, 0);
  CHECK(call(WindowsSyscallNumber::NtFreeVirtualMemory, self, kBaseSlot, kSizeSlot, 0x8000) == 0xC000001Au);
  CHECK(call(WindowsSyscallNumber::NtFreeVirtualMemory, self, kBaseSlot, kSizeSlot, 0x1234) == 0xC000000Du);
}

static void test_user_program() {
  FakeHost host;
  uint8_t random[16] = {1, 2, 3};
  const StringView args[] = {"prog"};
  auto load = [&](UserProgram& up, const Bytes& file) {
    return up.load(view(file), ArrayView<const StringView>(args, 1), ArrayView<const StringView>(), ConstByteView(random, 16));
  };

  UserProgram lin(A, 5, host);
  Bytes elf = make_elf();
  CHECK(!lin.loaded() && lin.process() == nullptr);
  CHECK(load(lin, elf) == LoadStatus::Ok && lin.loaded());
  CHECK(lin.process()->abi() == SyscallAbi::Linux && lin.process()->arch() == UserArch::X86_64);
  CHECK(lin.start().entry == 0x400100 && lin.start().arch == UserArch::X86_64 && lin.start().stack_pointer % 16 == 0);
  CHECK(lin.start().stack_pointer < AddressSpace::kUserTop && lin.address_space().check(lin.start().stack_pointer, 8, kProtRead | kProtWrite));
  CHECK(lin.process()->heap_start() == 0x603000);
  CHECK(load(lin, elf) == LoadStatus::Malformed);                          // only once
  SyscallRequest r;
  r.number = 12; r.args[0] = 0;
  CHECK(lin.process()->syscall(r) == 0x603000);                            // brk is live
  r.args[0] = 0x603000 + 100;
  CHECK(lin.process()->syscall(r) == 0x603000 + 100);
  r.number = 1; r.args[0] = 1; r.args[1] = 0x601000; r.args[2] = 6;
  CHECK(lin.process()->syscall(r) == 6 && host.out == "hello\n");
  r.number = 60; r.args[0] = 3;
  lin.process()->syscall(r);
  CHECK(lin.process()->exited() && lin.process()->exit_code() == 3);

  UserProgram win(A, 6, host);
  CHECK(load(win, make_pe()) == LoadStatus::Ok && win.process()->abi() == SyscallAbi::Windows);
  CHECK(win.start().entry == 0x100001000ull && win.start().stack_pointer % 16 == 8 && win.address_space().check(win.start().stack_pointer, 8, kProtRead | kProtWrite));

  // Failures leave nothing runnable.
  UserProgram bad(A, 7, host);
  Bytes text(100, 'x');
  CHECK(load(bad, text) == LoadStatus::NotExecutable && !bad.loaded());
  CHECK(load(bad, Bytes()) == LoadStatus::NotExecutable);
  UserProgram bsd(A, 8, host);
  Bytes freebsd = make_elf(); freebsd[7] = 9;
  CHECK(load(bsd, freebsd) == LoadStatus::Unsupported && !bsd.loaded());
  UserProgram trunc(A, 9, host);
  Bytes cut = make_elf(); cut.resize(100);
  CHECK(load(trunc, cut) != LoadStatus::Ok && !trunc.loaded());
  UserProgram dll(A, 10, host);
  Bytes d = make_pe(); put(d, 0x56, 0x2002, 2);
  CHECK(load(dll, d) == LoadStatus::Unsupported && !dll.loaded());
  UserProgram tiny(A, 11, host, 64 * 1024);                                 // the stack alone does not fit
  CHECK(load(tiny, make_elf()) == LoadStatus::OutOfMemory && !tiny.loaded());
  // Bit flips and truncation of the whole pipeline.
  Bytes base = make_elf();
  for (size_t i = 0; i < 64 + 56 * 2; ++i) for (int bit = 0; bit < 8; ++bit) {
    Bytes f = base; f[i] ^= uint8_t(1 << bit);
    UserProgram u(A, 12, host);
    (void)load(u, f);
  }
}

// Walks the tables the way the CPU would (the host test's pointers stand in for physical addresses). 0 if the page is not present.
static uint64_t translate(uint64_t root, uint64_t address, uint64_t* flags) {
  uint64_t table = root, accumulated = UserPageTables::kUser | UserPageTables::kWritable;
  const int shifts[4] = {39, 30, 21, 12};
  for (int level = 0; level < 4; ++level) {
    const uint64_t entry = reinterpret_cast<const uint64_t*>(table)[(address >> shifts[level]) & 511];
    if (!(entry & UserPageTables::kPresent)) return 0;
    accumulated &= entry;
    if (level == 3) {
      *flags = (accumulated & (UserPageTables::kUser | UserPageTables::kWritable)) | (entry & UserPageTables::kNoExecute);
      return (entry & 0x000FFFFFFFFFF000ull) | (address & 0xFFF);
    }
    table = entry & 0x000FFFFFFFFFF000ull;
  }
  return 0;
}

// With deferred freeing, the memory of what is unmapped, cut or moved stays allocated until reclaim() (other CPUs may still run on the old page tables).
static void test_deferred_free() {
  const int base = A.live;
  {
    AddressSpace s(A);
    s.set_deferred_free(true);
    CHECK(s.map(0x400000, 0x4000, kProtRead | kProtWrite));
    const int mapped = A.live;
    CHECK(s.unmap(0x401000, 0x1000));      // a hole in the middle: the tail is copied to new memory, the old memory stays whole
    CHECK(s.unmap(0x400000, 0x1000));
    CHECK(A.live > mapped);                // nothing was given back yet
    s.reclaim();
    CHECK(A.live < mapped + 4);
    CHECK(s.resize(0x402000, 0x8000));     // growing moves the region to bigger memory
    const int before = A.live;
    s.reclaim();
    CHECK(A.live < before);
    CHECK(s.unmap(0x402000, 0x10000));
    CHECK(A.live > base);
    s.set_deferred_free(false);
    s.reclaim();
  }
  CHECK(A.live == base);
  {
    AddressSpace s(A);      // destroyed with memory still waiting
    s.set_deferred_free(true);
    CHECK(s.map(0x400000, 0x2000, kProtRead));
    CHECK(s.unmap(0x400000, 0x2000));
  }
  CHECK(A.live == base);
}

static void test_user_page_tables() {
  AddressSpace s(A);
  CHECK(s.map(0x400000, 0x2000, kProtRead | kProtExecute));
  CHECK(s.map(0x601000, 0x1000, kProtRead | kProtWrite));
  CHECK(s.map(0x7F0000000000ull, 0x1000, 0));                      // no access
  const uint8_t mark = 0x5A;
  CHECK(s.write_privileged(0x400010, ConstByteView(&mark, 1)) && s.write_privileged(0x601000, ConstByteView(&mark, 1)));

  alignas(4096) static uint8_t kernel_page[8192];
  UserPageTables tables(A, true);
  CHECK(!tables.add_identity({0x1001, 4096, false, false}));      // not page aligned
  CHECK(!tables.add_identity({0x1000, 0, false, false}));
  CHECK(tables.add_identity({reinterpret_cast<uint64_t>(kernel_page), 4096, false, true}));
  CHECK(tables.add_identity({reinterpret_cast<uint64_t>(kernel_page) + 4096, 4096, true, false}));
  CHECK(!tables.built() && tables.root() == 0);
  CHECK(tables.sync(s) && tables.built() && tables.root() % 4096 == 0);

  uint64_t flags = 0;
  AddressSpace::RegionInfo info;
  CHECK(s.region_at(0, &info));
  uint64_t pa = translate(tables.root(), 0x400010, &flags);
  CHECK(pa == reinterpret_cast<uint64_t>(info.storage) + 0x10 && *reinterpret_cast<const uint8_t*>(pa) == 0x5A);
  CHECK((flags & UserPageTables::kUser) && !(flags & UserPageTables::kWritable) && !(flags & UserPageTables::kNoExecute));     // code: user, read-only, executable
  CHECK(translate(tables.root(), 0x401FFF, &flags) != 0 && translate(tables.root(), 0x402000, &flags) == 0);
  CHECK(translate(tables.root(), 0x601000, &flags) != 0 && (flags & UserPageTables::kWritable) && (flags & UserPageTables::kNoExecute));       // data: writable, no-execute
  CHECK(translate(tables.root(), 0x7F0000000000ull, &flags) == 0);                      // PROT_NONE: nothing mapped
  CHECK(translate(tables.root(), 0x1000, &flags) == 0 && translate(tables.root(), 0, &flags) == 0);
  const uint64_t k = reinterpret_cast<uint64_t>(kernel_page);
  CHECK(translate(tables.root(), k + 5, &flags) == k + 5 && !(flags & UserPageTables::kUser) && !(flags & UserPageTables::kNoExecute) && !(flags & UserPageTables::kWritable));
  CHECK(translate(tables.root(), k + 4096, &flags) == k + 4096 && !(flags & UserPageTables::kUser) && (flags & UserPageTables::kWritable) && (flags & UserPageTables::kNoExecute));

  // Unchanged space: no rebuild. A change: rebuilt, and the new layout shows.
  const uint64_t root = tables.root();
  CHECK(tables.sync(s) && tables.root() == root);
  CHECK(s.unmap(0x601000, 0x1000) && s.map(0x800000, 0x1000, kProtRead | kProtWrite));
  CHECK(tables.sync(s) && translate(tables.root(), 0x601000, &flags) == 0 && translate(tables.root(), 0x800000, &flags) != 0);
  CHECK(translate(tables.root(), k + 5, &flags) == k + 5);      // the kernel pages survive a rebuild
  // The heap moving to bigger memory is noticed.
  CHECK(s.map(0x900000, 0x1000, kProtRead | kProtWrite) && tables.sync(s));
  const uint64_t before = translate(tables.root(), 0x900000, &flags);
  CHECK(s.resize(0x900000, 0x10000) && tables.sync(s));
  CHECK(translate(tables.root(), 0x900000, &flags) != 0 && translate(tables.root(), 0x90F000, &flags) != 0 && before != 0);

  // Without NX the bit is never set.
  UserPageTables plain(A, false);
  CHECK(plain.sync(s) && translate(plain.root(), 0x800000, &flags) != 0 && !(flags & UserPageTables::kNoExecute));
  // Out of memory while building leaves nothing half-built.
  struct Limited final : allocators::IAllocator {
    int left = 3;
    void* allocate(uint64_t size) override { return left-- > 0 ? malloc(size) : nullptr; }
    void* allocate_aligned(uint64_t size, uint64_t a) override { return left-- > 0 ? aligned_alloc(a, size) : nullptr; }
    void deallocate(void* p) override { free(p); }
  } limited;
  UserPageTables starved(limited, true);
  CHECK(!starved.rebuild(s) && !starved.built() && starved.root() == 0);
}

namespace {
struct RandomHost final : IProcessHost {
  std::string out;
  uint64_t unhandled = 0, seed = 7;
  bool give_random = true;
  bool write_console(uint32_t, ConstByteView d) override { for (uint8_t c : d) out.push_back(char(c)); return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull + 5000; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
  bool random_bytes(ByteView o) override {
    if (!give_random) return false;
    for (uint8_t& b : o) { seed = seed * 6364136223846793005ull + 1; b = uint8_t(seed >> 56); }
    return true;
  }
  void unhandled_syscall(SyscallAbi, uint64_t number) override { unhandled = number; }
};
}  // namespace

static void test_linux_thread_pointer_and_startup_calls() {
  AddressSpace s(A);
  RandomHost host;
  ProcessBinding binding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)};
  Process p(1, binding, s, host);
  CHECK(s.map(0x10000, 0x2000, kProtRead | kProtWrite));
  uint64_t v = 0;
  // arch_prctl
  CHECK(!p.take_fs_change(&v) && v == 0);
  CHECK(p.syscall(lin(158, 0x1002, 0x7000)) == 0 && p.fs_base() == 0x7000 && p.take_fs_change(&v) && v == 0x7000 && !p.take_fs_change(&v));
  CHECK(!p.take_gs_change(&v));
  CHECK(p.syscall(lin(158, 0x1001, 0x9000)) == 0 && p.take_gs_change(&v) && v == 0x9000);
  CHECK(p.syscall(lin(158, 0x1003, 0x10000)) == 0 && s.read(0x10000, ByteView(reinterpret_cast<uint8_t*>(&v), 8)) && v == 0x7000);
  CHECK(p.syscall(lin(158, 0x1004, 0x10000)) == 0 && s.read(0x10000, ByteView(reinterpret_cast<uint8_t*>(&v), 8)) && v == 0x9000);
  CHECK(sres(p.syscall(lin(158, 0x1002, AddressSpace::kUserTop))) == -22);       // not a user address
  CHECK(sres(p.syscall(lin(158, 0x1002, ~uint64_t{0}))) == -22);
  CHECK(sres(p.syscall(lin(158, 0x1003, 0xDEAD0000))) == -14);
  CHECK(sres(p.syscall(lin(158, 0x9999, 0))) == -22);
  p.set_initial_thread_pointers(5, 6);
  CHECK(p.fs_base() == 5 && p.gs_base() == 6 && !p.take_fs_change(&v) && !p.take_gs_change(&v));

  // The calls a C library makes before main().
  CHECK(sres(p.syscall(lin(16, 1, 0x5413, 0x10000))) == -25);              // ioctl on a console: not a terminal
  CHECK(sres(p.syscall(lin(16, 9, 0x5413, 0))) == -9);
  CHECK(p.syscall(lin(5, 1, 0x10100)) == 0);                               // fstat(1)
  uint8_t mode[4] = {};
  CHECK(s.read(0x10100 + 24, ByteView(mode, 4)) && mode[1] == 0x21 && mode[0] == 0x90);      // S_IFCHR | 0620 = 0020620
  CHECK(sres(p.syscall(lin(5, 9, 0x10100))) == -9 && sres(p.syscall(lin(5, 1, 0xDEAD0000))) == -14);
  uint8_t zero = 0;
  CHECK(s.write(0x10200, ConstByteView(&zero, 1)));
  CHECK(p.syscall(lin(262, 1, 0x10200, 0x10100, 0x1000)) == 0);            // newfstatat(1, "", buf, AT_EMPTY_PATH)
  CHECK(sres(p.syscall(lin(262, 1, 0x10200, 0x10100, 0))) == -2);          // a path lookup: no file system
  CHECK(sres(p.syscall(lin(257, ~uint64_t{99}, 0x10200, 0))) == -2 && sres(p.syscall(lin(267, ~uint64_t{99}, 0x10200, 0x10100, 10))) == -2);
  CHECK(sres(p.syscall(lin(8, 1, 0, 0))) == -29 && sres(p.syscall(lin(8, 9, 0, 0))) == -9);
  uint8_t junk[32]; for (uint8_t& b : junk) b = 0xFF;
  CHECK(s.write(0x10300, ConstByteView(junk, 32)));
  CHECK(p.syscall(lin(13, 2, 0, 0x10300, 8)) == 0 && s.read(0x10300, ByteView(junk, 32)) && junk[0] == 0 && junk[31] == 0);      // rt_sigaction fills the old action
  CHECK(p.syscall(lin(14, 0, 0, 0x10300, 8)) == 0 && p.syscall(lin(131, 0, 0x10300)) == 0 && p.syscall(lin(273, 0, 24)) == 0 && p.syscall(lin(28, 0x10000, 4096, 4)) == 0);
  CHECK(sres(p.syscall(lin(334, 0, 0, 0, 0))) == -38);                     // rseq: not supported
  CHECK(p.syscall(lin(302, 0, 3, 0, 0x10400)) == 0 && p.read_user_u64(0x10400, &v) && v == (8ull << 20));      // prlimit64(RLIMIT_STACK)
  CHECK(p.syscall(lin(302, 0, 7, 0, 0x10400)) == 0 && p.read_user_u64(0x10400, &v) && v == ~uint64_t{0});
  CHECK(p.syscall(lin(110)) == 0);
  CHECK(p.syscall(lin(96, 0x10500, 0)) == 0 && p.read_user_u64(0x10500, &v) && v == 1700000000 && p.read_user_u64(0x10508, &v) && v == 5);
  // getrandom: the host's bytes, spread over the request; not supported without a source.
  CHECK(p.syscall(lin(318, 0x10600, 600, 0)) == 600);
  uint8_t r1[16], r2[16];
  CHECK(s.read(0x10600, ByteView(r1, 16)) && s.read(0x10600 + 584, ByteView(r2, 16)));
  bool all_zero = true; for (uint8_t b : r1) all_zero = all_zero && b == 0;
  CHECK(!all_zero);
  CHECK(sres(p.syscall(lin(318, 0xDEAD0000, 16, 0))) == -14);
  host.give_random = false;
  CHECK(sres(p.syscall(lin(318, 0x10600, 16, 0))) == -38);
  // A call this OS lacks is reported to the host.
  CHECK(sres(p.syscall(lin(202, 0, 0, 0))) == -38 && host.unhandled == 202);
  // The generic table has its own numbers and no arch_prctl.
  Process g(2, ProcessBinding{SyscallAbi::Linux, UserArch::AArch64, syscall_interface(SyscallAbi::Linux, UserArch::AArch64)}, s, host);
  CHECK(sres(g.syscall(lin(158, 0x1002, 0x7000))) == -38 && g.syscall(lin(173)) == 0 && g.syscall(lin(169, 0x10500, 0)) == 0);
  CHECK(g.syscall(lin(80, 1, 0x10100)) == 0 && s.read(0x10100 + 16, ByteView(mode, 4)) && mode[1] == 0x21);      // generic struct stat keeps st_mode at 16
}

static std::string wide_to_narrow(AddressSpace& s, uint64_t at, uint64_t bytes) {
  std::string out;
  for (uint64_t i = 0; i + 1 < bytes + 1 && i < bytes; i += 2) { uint8_t c[2]; CHECK(s.read(at + i, ByteView(c, 2))); out.push_back(char(c[0])); }
  return out;
}

static void test_windows_startup() {
  RandomHost host;
  UserProgram win(A, 33, host);
  const StringView args[] = {"C:\\tools\\app.exe", "-v", "two words"};
  const StringView env[] = {"PATH=C:\\bin", "A=b"};
  Bytes pe = make_pe(1, true);
  CHECK(win.load(view(pe), ArrayView<const StringView>(args, 3), ArrayView<const StringView>(env, 2), ConstByteView(nullptr, 0)) == LoadStatus::Ok);
  AddressSpace& s = win.address_space();
  Process& p = *win.process();
  const uint64_t teb = win.start().gs_base, peb = win.start().argument;
  CHECK(teb != 0 && peb != 0 && p.gs_base() == teb && p.fs_base() == 0 && win.start().fs_base == 0);
  uint64_t v = 0;
  CHECK(p.read_user_u64(teb + 0x30, &v) && v == teb);                       // gs:[0x30] = Self
  CHECK(p.read_user_u64(teb + 0x60, &v) && v == peb);                       // gs:[0x60] = PEB
  CHECK(p.read_user_u64(teb + 0x40, &v) && v == 33 && p.read_user_u64(teb + 0x48, &v) && v == 33);
  uint64_t stack_base = 0, stack_limit = 0;
  CHECK(p.read_user_u64(teb + 0x08, &stack_base) && p.read_user_u64(teb + 0x10, &stack_limit) && stack_base - stack_limit == kDefaultStackBytes);
  CHECK(win.start().stack_pointer < stack_base && win.start().stack_pointer > stack_limit);
  CHECK(p.read_user_u64(peb + 0x10, &v) && v == 0x100000000ull);            // ImageBaseAddress
  uint8_t b4[4] = {};
  CHECK(s.read(peb + 0x118, ByteView(b4, 4)) && b4[0] == 10);                // OSMajorVersion
  uint64_t params = 0;
  CHECK(p.read_user_u64(peb + 0x20, &params) && params != 0);
  CHECK(p.read_user_u64(params + 0x20, &v) && v == 4 && p.read_user_u64(params + 0x28, &v) && v == 8 && p.read_user_u64(params + 0x30, &v) && v == 12);
  uint8_t us[16];
  CHECK(s.read(params + 0x70, ByteView(us, 16)));                           // CommandLine: UNICODE_STRING
  uint64_t length = us[0] | (us[1] << 8), buffer = 0;
  for (int i = 0; i < 8; ++i) buffer |= uint64_t{us[8 + i]} << (8 * i);
  CHECK(wide_to_narrow(s, buffer, length) == "C:\\tools\\app.exe -v \"two words\"");
  CHECK(s.read(params + 0x60, ByteView(us, 16)));                           // ImagePathName
  length = us[0] | (us[1] << 8); buffer = 0;
  for (int i = 0; i < 8; ++i) buffer |= uint64_t{us[8 + i]} << (8 * i);
  CHECK(wide_to_narrow(s, buffer, length) == "C:\\tools\\app.exe");
  uint64_t environment = 0, environment_size = 0;
  CHECK(p.read_user_u64(params + 0x80, &environment) && p.read_user_u64(params + 0x3F0, &environment_size));
  CHECK(environment_size == 17 * 2 && wide_to_narrow(s, environment, 11 * 2) == "PATH=C:\\bin");      // "PATH=C:\bin\0" "A=b\0" "\0"
  // KUSER_SHARED_DATA
  CHECK(s.read(kSharedUserDataAddress + 0x26C, ByteView(b4, 4)) && b4[0] == 10);
  uint8_t prot = 0;
  CHECK(s.protection_at(kSharedUserDataAddress, &prot) && prot == kProtRead);
  // Static TLS: the TEB points at an array whose first entry is this thread's copy of the template, with the zero fill after it.
  uint64_t array = 0, block = 0;
  CHECK(p.read_user_u64(teb + 0x58, &array) && array != 0 && p.read_user_u64(array, &block) && block != 0);
  uint8_t tls[16];
  CHECK(s.read(block, ByteView(tls, 16)) && std::string((char*)tls, 8) == "TLSDATA!" && tls[8] == 0 && tls[15] == 0);
  CHECK(s.read(0x100000000ull + 0x10D0, ByteView(b4, 4)) && b4[0] == 0 && b4[1] == 0);      // the index variable was set to 0
  CHECK(s.write(block, ConstByteView(tls + 8, 1)));                          // the block is writable

  // No TLS directory: the pointer array is there (libraries loaded later put their blocks in it) and holds nothing.
  UserProgram plain(A, 34, host);
  Bytes pe2 = make_pe();
  CHECK(plain.load(view(pe2), ArrayView<const StringView>(), ArrayView<const StringView>(), ConstByteView(nullptr, 0)) == LoadStatus::Ok);
  uint64_t first_block = 1;
  CHECK(plain.process()->read_user_u64(plain.start().gs_base + 0x58, &v) && v != 0 && plain.process()->read_user_u64(v, &first_block) && first_block == 0);
  // Damaged TLS directories are refused, not followed.
  Bytes bad = make_pe(1, true); put(bad, 0x288, 0x100000000ull + 0x10C0 - 8, 8);        // end before start
  UserProgram u1(A, 35, host);
  CHECK(u1.load(view(bad), ArrayView<const StringView>(), ArrayView<const StringView>(), ConstByteView(nullptr, 0)) == LoadStatus::Malformed);
  bad = make_pe(1, true); put(bad, 0x280, 0x100000000ull + 0x10F0000, 8); put(bad, 0x288, 0x100000000ull + 0x10F0008, 8);      // template outside the image
  UserProgram u2(A, 36, host);
  CHECK(u2.load(view(bad), ArrayView<const StringView>(), ArrayView<const StringView>(), ConstByteView(nullptr, 0)) == LoadStatus::Malformed);
  bad = make_pe(1, true); put(bad, 0x2A0, 0x7FFFFFFF, 4);                                // absurd zero fill
  UserProgram u3(A, 37, host);
  CHECK(u3.load(view(bad), ArrayView<const StringView>(), ArrayView<const StringView>(), ConstByteView(nullptr, 0)) == LoadStatus::Malformed);
  // Bit flips in the TLS directory and the startup text never read or write out of bounds.
  Bytes base = make_pe(1, true);
  for (size_t i = 0x280; i < 0x2A8; ++i) for (int bit = 0; bit < 8; ++bit) {
    Bytes f = base; f[i] ^= uint8_t(1 << bit);
    UserProgram u(A, 38, host);
    (void)u.load(view(f), ArrayView<const StringView>(args, 3), ArrayView<const StringView>(env, 2), ConstByteView(nullptr, 0));
  }
}

int main() {
  test_address_space();
  test_load_elf();
  test_load_elf_rejects();
  test_load_pe();
  test_stacks();
  test_linux_memory();
  test_windows_memory();
  test_user_program();
  test_deferred_free();
  test_user_page_tables();
  test_linux_thread_pointer_and_startup_calls();
  test_windows_startup();
  CHECK(A.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("LoaderTest: all passed\n");
  return 0;
}
