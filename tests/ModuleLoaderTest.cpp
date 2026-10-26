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
#include <map>
#include <string>
#include <vector>
#include "Process/AddressSpace.hpp"
#include "Process/ModuleLoader.hpp"

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

static void put(Bytes& b, uint64_t off, uint64_t v, int n) {
  if (b.size() < off + n) b.resize(off + n);
  for (int i = 0; i < n; ++i) b[off + i] = uint8_t(v >> (8 * i));
}
static void put_text(Bytes& b, uint64_t off, const std::string& s) {
  if (b.size() < off + s.size() + 1) b.resize(off + s.size() + 1);
  for (size_t i = 0; i < s.size(); ++i) b[off + i] = uint8_t(s[i]);
  b[off + s.size()] = 0;
}

struct Import { std::string dll, name; uint16_t ordinal = 0; bool by_ordinal = false; };
struct Export { std::string name; std::string forwarder; uint32_t ordinal = 0; };
struct Spec {
  bool dll = true;
  uint64_t base = 0x180000000ull;
  std::vector<Export> exports;
  std::vector<Import> imports;
  bool relocations = true;
  std::string dll_name = "test.dll";
};

// A PE32+ image with one big section at RVA 0x1000 (file offset 0x400, 0x2000 bytes) and a .reloc section at 0x3000.
//   section offsets: 0x000 code (an exported function every 0x10 bytes), 0x200 export directory, 0x800 import descriptors, 0x900 import tables, 0xC00 names,
//   0x1F00 a pointer to the image's own section (what a relocation fixes)
struct Built {
  Bytes bytes;
  std::vector<uint32_t> slots;        // the import address table entry RVAs, in import order
};

static Built build(const Spec& spec) {
  Built out;
  Bytes& b = out.bytes;
  b.assign(0x2600, 0);
  put_text(b, 0, "MZ"); b[1] = 'Z'; put(b, 0x3C, 0x40, 4);
  put_text(b, 0x40, std::string("PE\0\0", 4)); b[0x44 - 1 + 1 - 1] = 0;
  b[0x40] = 'P'; b[0x41] = 'E'; b[0x42] = 0; b[0x43] = 0;
  put(b, 0x44, 0x8664, 2); put(b, 0x46, 2, 2); put(b, 0x54, 240, 2); put(b, 0x56, spec.dll ? 0x2022 : 0x0022, 2);
  const uint64_t o = 0x58;
  put(b, o, 0x20B, 2); put(b, o + 16, 0x1000, 4); put(b, o + 24, spec.base, 8); put(b, o + 32, 0x1000, 4); put(b, o + 36, 0x200, 4);
  put(b, o + 56, 0x4000, 4); put(b, o + 60, 0x400, 4); put(b, o + 68, 3, 2); put(b, o + 108, 16, 4);
  const uint64_t s0 = 0x400;      // the section in the file
  // exports
  const uint32_t n = uint32_t(spec.exports.size());
  if (n) {
    const uint64_t d = s0 + 0x200;
    put(b, d + 12, 0x1000 + 0x300, 4);                      // name
    put(b, d + 16, 1, 4);                                   // ordinal base
    put(b, d + 20, n, 4); put(b, d + 24, n, 4);
    put(b, d + 28, 0x1000 + 0x240, 4); put(b, d + 32, 0x1000 + 0x280, 4); put(b, d + 36, 0x1000 + 0x2C0, 4);
    put_text(b, s0 + 0x300, spec.dll_name);
    uint32_t name_at = 0x340, forward_at = 0x500;
    // names must be sorted; callers give them sorted
    for (uint32_t i = 0; i < n; ++i) {
      const Export& e = spec.exports[i];
      if (!e.forwarder.empty()) {
        put(b, s0 + 0x240 + 4 * i, 0x1000 + forward_at, 4);
        put_text(b, s0 + forward_at, e.forwarder);
        forward_at += uint32_t(e.forwarder.size() + 1);
      } else {
        put(b, s0 + 0x240 + 4 * i, 0x1000 + 0x10 * (i + 1), 4);
        b[s0 + 0x10 * (i + 1)] = 0xC3;
      }
      put(b, s0 + 0x280 + 4 * i, 0x1000 + name_at, 4);
      put_text(b, s0 + name_at, e.name);
      name_at += uint32_t(e.name.size() + 1);
      put(b, s0 + 0x2C0 + 2 * i, i, 2);
    }
    put(b, o + 112, 0x1200, 4); put(b, o + 116, 0x300 + 0x40, 4);      // export directory: includes the forwarder strings (0x500..0x540)
    put(b, o + 116, 0x340, 4);
  }
  // imports
  if (!spec.imports.empty()) {
    std::vector<std::string> dlls;
    for (const Import& imp : spec.imports) {
      bool seen = false;
      for (auto& d : dlls) seen |= d == imp.dll;
      if (!seen) dlls.push_back(imp.dll);
    }
    uint32_t table = 0x900, names = 0xC00;
    for (size_t di = 0; di < dlls.size(); ++di) {
      uint32_t count = 0;
      for (const Import& imp : spec.imports) count += imp.dll == dlls[di];
      const uint64_t d = s0 + 0x800 + 20 * di;
      put(b, d, 0x1000 + table, 4);                           // OriginalFirstThunk
      put(b, d + 12, 0x1000 + names, 4);                      // name
      put(b, d + 16, 0x1000 + table + 8 * (count + 1), 4);    // FirstThunk (the IAT)
      put_text(b, s0 + names, dlls[di]);
      names += uint32_t(dlls[di].size() + 1);
      uint32_t k = 0;
      for (const Import& imp : spec.imports) {
        if (imp.dll != dlls[di]) continue;
        if (imp.by_ordinal) {
          put(b, s0 + table + 8 * k, (1ull << 63) | imp.ordinal, 8);
          put(b, s0 + table + 8 * (count + 1) + 8 * k, (1ull << 63) | imp.ordinal, 8);
        } else {
          put(b, s0 + table + 8 * k, 0x1000 + names, 8);
          put(b, s0 + table + 8 * (count + 1) + 8 * k, 0x1000 + names, 8);
          put_text(b, s0 + names + 2, imp.name);
          names += uint32_t(imp.name.size() + 3);
          names = (names + 1) & ~1u;
        }
        out.slots.push_back(0x1000 + table + 8 * (count + 1) + 8 * k);
        ++k;
      }
      table += 8 * (2 * (count + 1));
    }
    put(b, o + 120, 0x1800, 4); put(b, o + 124, 20 * (dlls.size() + 1), 4);
  }
  // a pointer inside the image, fixed by a relocation
  put(b, s0 + 0x1F00, spec.base + 0x1234, 8);
  if (spec.relocations) {
    const uint64_t r = 0x2400;
    put(b, r, 0x2000, 4); put(b, r + 4, 12, 4); put(b, r + 8, (10u << 12) | 0xF00, 2); put(b, r + 10, 0, 2);
    put(b, o + 152, 0x3000, 4); put(b, o + 156, 12, 4);        // directory 5: base relocations
  }
  // section table
  const uint64_t t = 0x148;
  put_text(b, t, ".text"); put(b, t + 8, 0x2000, 4); put(b, t + 12, 0x1000, 4); put(b, t + 16, 0x2000, 4); put(b, t + 20, 0x400, 4); put(b, t + 36, 0xE0000020u, 4);
  put_text(b, t + 40, ".reloc"); put(b, t + 48, 0x200, 4); put(b, t + 52, 0x3000, 4); put(b, t + 56, 0x200, 4); put(b, t + 60, 0x2400, 4); put(b, t + 76, 0x42000040, 4);
  put(b, 0x58 + 16, spec.dll ? 0x1000 : 0x1000, 4);
  return out;
}

struct Provider final : ILibraryProvider {
  std::map<std::string, Bytes> libraries;
  std::vector<std::string> asked;
  bool find(StringView name, ConstByteView* image) override {
    const std::string n(name.data(), name.size());
    asked.push_back(n);
    auto it = libraries.find(n);
    if (it == libraries.end()) return false;
    *image = ConstByteView(it->second.data(), it->second.size());
    return true;
  }
};

static uint64_t u64_at(AddressSpace& s, uint64_t at) {
  uint8_t raw[8] = {};
  CHECK(s.read_privileged(at, ByteView(raw, 8)));
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i);
  return v;
}
static std::string str(StringView v) { return std::string(v.data(), v.size()); }

static void test_names() {
  String out(A);
  auto name = [&](const char* n) { return canonical_library_name(n, &out) ? str(out.view()) : std::string("<error>"); };
  CHECK(name("KERNEL32.dll") == "kernel32.dll" && name("Kernel32") == "kernel32.dll" && name("ntdll.DLL") == "ntdll.dll");
  CHECK(name("api-ms-win-core-file-l1-1-0.dll") == "kernel32.dll" && name("API-MS-WIN-CORE-HEAP-L1-2-0.dll") == "kernel32.dll" && name("KERNELBASE.dll") == "kernel32.dll");
  CHECK(name("api-ms-win-crt-stdio-l1-1-0.dll") == "api-ms-win-crt-stdio-l1-1-0.dll");      // the C runtime is not folded away
  CHECK(name("") == "<error>" && name(std::string(200, 'a').c_str()) == "<error>");
}

static Spec ntdll_spec() {
  Spec s;
  s.dll_name = "ntdll.dll";
  s.exports = {{"RtlAlpha", ""}, {"RtlBeta", ""}, {"RtlViaKernel", "KERNEL32.KFunc"}};
  return s;
}
static Spec kernel_spec() {
  Spec s;
  s.dll_name = "kernel32.dll";
  s.exports = {{"KFunc", ""}, {"KOther", ""}};
  s.imports = {{"ntdll.dll", "RtlAlpha"}, {"ntdll.dll", "RtlBeta"}};
  return s;
}

static void test_load() {
  Provider provider;
  provider.libraries["ntdll.dll"] = build(ntdll_spec()).bytes;
  const Built kernel = build(kernel_spec());
  provider.libraries["kernel32.dll"] = kernel.bytes;

  Spec program;
  program.dll = false;
  program.base = 0x140000000ull;
  program.imports = {{"KERNEL32.dll", "KFunc"}, {"KERNEL32.dll", "KOther"}, {"ntdll.dll", "RtlViaKernel"}, {"ntdll.dll", "RtlAlpha"}, {"KERNEL32.dll", "NoSuchThing"},
                     {"api-ms-win-core-file-l1-1-0.dll", "KFunc"}, {"ntdll.dll", "", 2, true}};
  const Built exe = build(program);
  exe::PeFile pe(A);
  CHECK(pe.parse(ConstByteView(exe.bytes.data(), exe.bytes.size())) && !pe.is_dll() && pe.imports().count() == 7);
  CHECK(pe.imports()[0].slot_rva == exe.slots[0] && pe.imports()[5].slot_rva == exe.slots[5] && pe.imports()[5].by_ordinal && pe.imports()[5].ordinal == 2);
  CHECK(pe.relocations_ok() && pe.relocations().count() == 1 && pe.relocations()[0].rva == 0x2F00 && pe.relocations()[0].type == exe::kPeRelocationDir64);

  AddressSpace space(A);
  LoadedImage image;
  CHECK(load_pe(pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), space, &image) == LoadStatus::Ok);
  ModuleSet set(A);
  String detail(A);
  CHECK(load_windows_modules(space, A, pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), "app.exe", provider, &set, &detail) == LoadStatus::Ok);
  CHECK(set.count() == 3 && set.at(0).main && set.at(0).name.view().equals("app.exe"));
  const Module* nt = set.find("ntdll.dll");
  const Module* k32 = set.find("kernel32.dll");
  CHECK(nt && k32 && set.find("kernelbase.dll") == nullptr);
  // Both prefer 0x180000000: the first keeps it, the second moves, and its own pointer follows it.
  CHECK(k32->base != nt->base && (nt->base == 0x180000000ull || k32->base == 0x180000000ull));
  const Module* moved = nt->base == 0x180000000ull ? k32 : nt;
  const Module* kept = nt->base == 0x180000000ull ? nt : k32;
  CHECK(u64_at(space, moved->base + 0x2F00) == moved->base + 0x1234 && u64_at(space, kept->base + 0x2F00) == kept->base + 0x1234);
  CHECK(moved->base % 0x10000 == 0 && moved->base >= 0x7FF800000000ull);
  // The program's import address table (the tables are in the order of the libraries: KERNEL32 x3, ntdll x3, the API set).
  const uint64_t base = 0x140000000ull;
  const uint64_t kfunc = k32->base + 0x1010, kother = k32->base + 0x1020;
  CHECK(u64_at(space, base + exe.slots[0]) == kfunc && u64_at(space, base + exe.slots[1]) == kother);
  CHECK(u64_at(space, base + exe.slots[3]) == kfunc);                                    // a forwarder: ntdll.RtlViaKernel -> KERNEL32.KFunc
  CHECK(u64_at(space, base + exe.slots[4]) == nt->base + 0x1010);
  CHECK(u64_at(space, base + exe.slots[6]) == kfunc);                                    // an API-set name
  CHECK(u64_at(space, base + exe.slots[5]) == nt->base + 0x1020);                        // by ordinal 2 = the second export
  // The missing function lands on a stub that says what it was.
  const uint64_t stub = u64_at(space, base + exe.slots[2]);
  String what(A);
  CHECK(set.missing_count() == 1 && set.describe_missing(stub, &what) && str(what.view()) == "KERNEL32.dll!NoSuchThing" && !set.describe_missing(kfunc, &what));
  uint8_t code[2];
  CHECK(space.protection_at(stub, &code[0]) && (code[0] & kProtExecute) && !(code[0] & kProtWrite));      // code, not writable
  CHECK(space.read_privileged(stub, ByteView(code, 2)) && code[0] == 0x0F && code[1] == 0x0B);                                   // ud2
  // kernel32's own imports of ntdll.
  CHECK(u64_at(space, k32->base + kernel.slots[0]) == nt->base + 0x1010 && u64_at(space, k32->base + kernel.slots[1]) == nt->base + 0x1020);
  // ntdll first, then kernel32 (which imports it).
  CHECK(set.init_count() == 2 && set.init_at(0).name.view().equals("ntdll.dll") && set.init_at(1).name.view().equals("kernel32.dll"));
  CHECK(set.export_address("ntdll.dll", "RtlAlpha") == nt->base + 0x1010 && set.export_address("NTDLL", "RtlViaKernel") == kfunc && set.export_address("ntdll.dll", "nope") == 0 &&
        set.export_address("nodll", "x") == 0);
  CHECK(nt->entry == nt->base + 0x1000 && !set.at(0).pe);
  // Each library was asked for once.
  int asked_nt = 0;
  for (auto& n : provider.asked) asked_nt += n == "ntdll.dll";
  CHECK(asked_nt == 1);
}

static void test_failures() {
  Spec program;
  program.dll = false;
  program.base = 0x140000000ull;
  program.imports = {{"kernel32.dll", "KFunc"}};
  const Built exe = build(program);
  exe::PeFile pe(A);
  CHECK(pe.parse(ConstByteView(exe.bytes.data(), exe.bytes.size())));
  auto attempt = [&](Provider& provider, String* detail) {
    AddressSpace space(A);
    LoadedImage image;
    CHECK(load_pe(pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), space, &image) == LoadStatus::Ok);
    ModuleSet set(A);
    return load_windows_modules(space, A, pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), "app.exe", provider, &set, detail);
  };
  String detail(A);
  Provider none;                                      // no library at all: the load goes on, every import is a stub
  CHECK(attempt(none, &detail) == LoadStatus::Ok);
  Provider junk;
  junk.libraries["kernel32.dll"] = Bytes(100, 'x');
  CHECK(attempt(junk, &detail) == LoadStatus::Malformed && detail.view().find("kernel32.dll") != StringView::npos);
  Provider notdll;                                    // an executable where a library should be
  Spec exe_spec; exe_spec.dll = false;
  notdll.libraries["kernel32.dll"] = build(exe_spec).bytes;
  detail.clear();
  CHECK(attempt(notdll, &detail) == LoadStatus::Malformed);
  // A DLL that cannot move: it prefers 0x140000000 (the program's place) and has no relocations.
  Provider fixed;
  Spec fixed_spec = kernel_spec();
  fixed_spec.base = 0x140000000ull;
  fixed_spec.relocations = false;
  fixed.libraries["kernel32.dll"] = build(fixed_spec).bytes;
  fixed.libraries["ntdll.dll"] = build(ntdll_spec()).bytes;
  detail.clear();
  CHECK(attempt(fixed, &detail) == LoadStatus::Malformed && detail.view().find("relocations") != StringView::npos);
  // Two libraries that import each other, and one that imports itself: loaded once, no endless loop.
  Provider cycle;
  Spec a = kernel_spec(); a.imports = {{"ntdll.dll", "RtlAlpha"}, {"kernel32.dll", "KFunc"}};
  Spec b = ntdll_spec(); b.imports = {{"kernel32.dll", "KFunc"}};
  cycle.libraries["kernel32.dll"] = build(a).bytes;
  cycle.libraries["ntdll.dll"] = build(b).bytes;
  CHECK(attempt(cycle, &detail) == LoadStatus::Ok);
  // Many missing functions: stubs run out, the load is refused instead of overflowing.
  Spec greedy;
  greedy.dll = false; greedy.base = 0x140000000ull;
  for (int i = 0; i < 300; ++i) greedy.imports.push_back({"missing.dll", "F" + std::to_string(i)});
  // (one section holds at most 0x600 bytes of import tables in this builder: 300 imports of 16 bytes do not fit, so use fewer)
  greedy.imports.resize(40);
  const Built g = build(greedy);
  exe::PeFile gp(A);
  CHECK(gp.parse(ConstByteView(g.bytes.data(), g.bytes.size())));
  AddressSpace space(A);
  LoadedImage image;
  CHECK(load_pe(gp, ConstByteView(g.bytes.data(), g.bytes.size()), space, &image) == LoadStatus::Ok);
  ModuleSet set(A);
  CHECK(load_windows_modules(space, A, gp, ConstByteView(g.bytes.data(), g.bytes.size()), "g.exe", none, &set, &detail) == LoadStatus::Ok && set.missing_count() == 40);

  // Damage anywhere in the headers of a library never reads or writes out of bounds.
  Provider good;
  good.libraries["kernel32.dll"] = build(kernel_spec()).bytes;
  good.libraries["ntdll.dll"] = build(ntdll_spec()).bytes;
  for (size_t i = 0; i < 0x200; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      Provider damaged = good;
      damaged.libraries["kernel32.dll"][i] ^= uint8_t(1 << bit);
      detail.clear();
      (void)attempt(damaged, &detail);
    }
  }
  for (size_t i = 0x800; i < 0x840; ++i) {                    // and in the import and export tables
    Provider damaged = good;
    damaged.libraries["kernel32.dll"][0x400 + i] ^= 0xFF;
    damaged.libraries["ntdll.dll"][0x400 + i - 0x600] ^= 0xFF;
    (void)attempt(damaged, &detail);
  }
}

int main() {
  test_names();
  test_load();
  test_failures();
  CHECK(A.live == 0);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("ModuleLoaderTest: all passed\n");
  return 0;
}
