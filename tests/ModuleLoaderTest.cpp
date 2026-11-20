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
#include "Process/ImageCache.hpp"
#include "Process/ImageFileCache.hpp"
#include <thread>
#include <atomic>
#include "FileSystem/Ram/RamFileSystem.hpp"

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
  std::string manifest;      // an RT_MANIFEST resource #1 (the program's side-by-side manifest), if not empty
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
  // resources: type 24 -> id 1 -> language 1033 -> data (the tree at section offset 0x1A00, the manifest's text at 0x1400)
  if (!spec.manifest.empty()) {
    const uint64_t r = s0 + 0x1A00;
    put(b, r + 14, 1, 2); put(b, r + 16, 24, 4); put(b, r + 20, 0x80000000u | 0x18, 4);
    put(b, r + 0x18 + 14, 1, 2); put(b, r + 0x18 + 16, 1, 4); put(b, r + 0x18 + 20, 0x80000000u | 0x30, 4);
    put(b, r + 0x30 + 14, 1, 2); put(b, r + 0x30 + 16, 1033, 4); put(b, r + 0x30 + 20, 0x48, 4);
    put(b, r + 0x48, 0x1000 + 0x1400, 4); put(b, r + 0x48 + 4, spec.manifest.size(), 4);
    put_text(b, s0 + 0x1400, spec.manifest);
    put(b, o + 128, 0x1000 + 0x1A00, 4); put(b, o + 132, 0x58, 4);      // directory 2: resources
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
  CHECK(name("api-ms-win-core-winrt-l1-1-0.dll") == "combase.dll" && name("api-ms-win-core-winrt-string-l1-1-0.dll") == "combase.dll" &&
        name("api-ms-win-core-realtime-l1-1-1.dll") == "kernel32.dll");      // the WinRT API sets are combase's, the other core ones kernel32's
  CHECK(name("api-ms-win-power-base-l1-1-0.dll") == "powrprof.dll" && name("api-ms-win-shcore-scaling-l1-1-1.dll") == "shcore.dll");
  CHECK(name("WINSPOOL.DRV") == "winspool.dll" && name("bthprops.cpl") == "bthprops.dll" && name("other.cpl") == "other.cpl");
  CHECK(name("api-ms-win-crt-stdio-l1-1-0.dll") == "msvcrt.dll" && name("UCRTBASE.DLL") == "msvcrt.dll" && name("msvcr120.dll") == "msvcrt.dll" && name("msvcrt") == "msvcrt.dll");      // every C runtime is the one msvcrt.dll
  CHECK(name("XInput9_1_0.dll") == "xinput1_4.dll" && name("xinput1_3.dll") == "xinput1_4.dll" && name("XINPUT1_4.DLL") == "xinput1_4.dll" && name("xinputuap") == "xinput1_4.dll");      // every XInput is one library
  CHECK(name("xinput.dll") == "xinput.dll" && name("wsock32.dll") == "ws2_32.dll");
  CHECK(name("vcruntime140.dll") == "msvcrt.dll" && name("user32.dll") == "user32.dll");
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

// Libraries at their preferred base share their pages between address spaces (the image cache); what one writes is its own.
static void test_shared_images() {
  ImageCache cache(A);
  set_image_cache(&cache);
  Provider provider;
  provider.libraries["ntdll.dll"] = build(ntdll_spec()).bytes;
  provider.libraries["kernel32.dll"] = build(kernel_spec()).bytes;
  Spec program;
  program.dll = false;
  program.base = 0x140000000ull;
  program.imports = {{"KERNEL32.dll", "KFunc"}, {"ntdll.dll", "RtlAlpha"}};
  const Built exe = build(program);
  exe::PeFile pe(A);
  CHECK(pe.parse(ConstByteView(exe.bytes.data(), exe.bytes.size())));
  {
    AddressSpace one(A), two(A);
    ModuleSet set_one(A), set_two(A);
    String detail(A);
    LoadedImage image;
    CHECK(load_pe(pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), one, &image) == LoadStatus::Ok && load_pe(pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), two, &image) == LoadStatus::Ok);
    CHECK(load_windows_modules(one, A, pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), "app.exe", provider, &set_one, &detail) == LoadStatus::Ok);
    CHECK(load_windows_modules(two, A, pe, ConstByteView(exe.bytes.data(), exe.bytes.size()), "app.exe", provider, &set_two, &detail) == LoadStatus::Ok);
    const Module* kept = set_one.find("ntdll.dll")->base == 0x180000000ull ? set_one.find("ntdll.dll") : set_one.find("kernel32.dll");
    const uint64_t page = kept->base + 0x2000;      // (the second page of its section: nothing the loader writes)
    CHECK(cache.count() == 2 && one.is_shared(page) && two.is_shared(page) && one.storage_at(page) == two.storage_at(page));      // (the moved library is shared too, relocated once)
    const Module* moved = set_one.find("ntdll.dll") == kept ? set_one.find("kernel32.dll") : set_one.find("ntdll.dll");
    CHECK(one.is_shared(moved->base + 0x2000) && two.is_shared(moved->base + 0x2000) && one.storage_at(moved->base + 0x2000) == two.storage_at(moved->base + 0x2000));
    CHECK(u64_at(one, moved->base + 0x2F00) == moved->base + 0x1234 && u64_at(two, moved->base + 0x2F00) == moved->base + 0x1234);
    CHECK(u64_at(one, kept->base + 0x2F00) == kept->base + 0x1234 && u64_at(two, kept->base + 0x2F00) == kept->base + 0x1234);
    const uint8_t mark = 0x77;
    CHECK(one.write_privileged(page, ConstByteView(&mark, 1)) && !one.is_shared(page) && two.is_shared(page));
    uint8_t seen[2] = {};
    CHECK(one.read_privileged(page, ByteView(seen, 1)) && two.read_privileged(page, ByteView(seen + 1, 1)) && seen[0] == 0x77 && seen[1] != 0x77);
    // The program's imports reach the shared library's code alike in both.
    CHECK(set_one.export_address("ntdll.dll", "RtlAlpha") == set_two.export_address("ntdll.dll", "RtlAlpha"));
  }
  CHECK(cache.count() == 0);      // nobody maps it any more: it is gone
  set_image_cache(nullptr);
}

static std::vector<std::string> strings(const List<String>& list) {
  std::vector<std::string> out;
  for (const String& s : list.view()) out.push_back(str(s.view()));
  return out;
}

static void write_file(filesystem::IFileSystem& fs, const char* path, const Bytes& bytes) {
  UniquePointer<filesystem::IFile> file;
  uint64_t done = 0;
  CHECK(fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
  if (file) CHECK(file->write(0, ConstByteView(bytes.data(), bytes.size()), &done) == filesystem::Status::Ok && done == bytes.size());
}
static void write_file(filesystem::IFileSystem& fs, const char* path, const std::string& text) { write_file(fs, path, Bytes(text.begin(), text.end())); }

// Side-by-side manifests: a program's private assemblies (Chrome: chrome.exe -> "<version>\\<version>.manifest" -> chrome_elf.dll in that folder).
static void test_private_assemblies() {
  const std::string program_manifest =
      "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>\n"
      "<dependency><dependentAssembly><assemblyIdentity type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' "
      "publicKeyToken='6595b64144ccf1df' language='*'/></dependentAssembly></dependency>\n"
      "<dependency>\n  <dependentAssembly>\n    <assemblyIdentity name=\"131.0.6778.86\" version=\"131.0.6778.86\" type=\"win32\"/>\n  </dependentAssembly>\n</dependency>\n"
      "<dependency><dependentAssembly><assemblyIdentity name='..' type='win32'/></dependentAssembly></dependency>\n"
      "<dependency><dependentAssembly><assemblyIdentity name='a/b' type='win32'/></dependentAssembly></dependency>\n"
      "</assembly>\n";
  {
    List<String> names(A);
    CHECK(manifest_private_assemblies(StringView(program_manifest.c_str()), A, &names));
    CHECK(strings(names) == std::vector<std::string>{"131.0.6778.86"});      // (the shared one has a token; ".." and "a/b" are no folder names)
    List<String> files(A);
    CHECK(manifest_files(StringView("<assembly><assemblyIdentity name='x'/><file name='chrome_elf.dll'/><file  name = \"Other.DLL\" hash='1'></file>"
                                    "<fileX name='no.dll'/><file name='../up.dll'/><file name='chrome_elf.dll'/></assembly>"), A, &files));
    CHECK(strings(files) == std::vector<std::string>{"chrome_elf.dll", "Other.DLL"});
    // Truncated or broken text: nothing found, nothing read past the end.
    for (size_t cut = 0; cut < program_manifest.size(); cut += 7) {
      List<String> some(A);
      CHECK(manifest_private_assemblies(StringView(program_manifest.substr(0, cut).c_str()), A, &some));
      for (const String& n : some.view()) CHECK(str(n.view()) == "131.0.6778.86");
    }
  }
  // The resource is found in the image; a damaged resource directory is no resource.
  Spec program_spec;
  program_spec.dll = false;
  program_spec.base = 0x140000000ull;
  program_spec.manifest = program_manifest;
  program_spec.imports = {{"chrome_elf.dll", "IsBrowserProcess"}};
  const Built program = build(program_spec);
  {
    exe::PeFile pe(A);
    ConstByteView found;
    CHECK(pe.parse(ConstByteView(program.bytes.data(), program.bytes.size())));
    CHECK(pe.find_resource(ConstByteView(program.bytes.data(), program.bytes.size()), 24, 1, &found) && str(StringView(found)) == program_manifest);
    CHECK(!pe.find_resource(ConstByteView(program.bytes.data(), program.bytes.size()), 16, 1, &found));      // (no version resource)
    for (uint64_t at = 0x400 + 0x1A00; at < 0x400 + 0x1A00 + 0x50; ++at) {      // a bit flipped anywhere in the tree: found or not, never out of bounds
      for (int bit = 0; bit < 8; ++bit) {
        Bytes flipped = program.bytes;
        flipped[at] ^= uint8_t(1u << bit);
        exe::PeFile damaged(A);
        ConstByteView any;
        if (damaged.parse(ConstByteView(flipped.data(), flipped.size())) && damaged.find_resource(ConstByteView(flipped.data(), flipped.size()), 24, 1, &any))
          CHECK(any.data() >= flipped.data() && any.data() + any.size() <= flipped.data() + flipped.size());
      }
    }
  }
  // The provider finds the assembly's DLL in its folder, before the program's folder and System32.
  {
    filesystem::RamFileSystem fs(A);
    for (const char* d : {"/Windows", "/Windows/Windows", "/Windows/Windows/System32", "/Windows/App", "/Windows/App/131.0.6778.86"}) CHECK(fs.create_directory(d) == filesystem::Status::Ok);
    Spec elf;
    elf.exports = {{"IsBrowserProcess", "", 0}};
    elf.dll_name = "chrome_elf.dll";
    Bytes elf_bytes = build(elf).bytes;
    write_file(fs, "/Windows/App/131.0.6778.86/chrome_elf.dll", elf_bytes);
    Bytes decoy = elf_bytes;
    decoy[0x400] = 0xCC;
    write_file(fs, "/Windows/App/chrome_elf.dll.old", decoy);
    write_file(fs, "/Windows/App/131.0.6778.86/131.0.6778.86.manifest",
               std::string("<assembly><assemblyIdentity name='131.0.6778.86' version='131.0.6778.86' type='win32'/><file name='chrome_elf.dll'/></assembly>"));
    write_file(fs, "/Windows/App/chrome.exe", program.bytes);
    FileLibraryProvider provider(A, &fs);
    CHECK(provider.add_program("/Windows/App/chrome.exe", ConstByteView(program.bytes.data(), program.bytes.size())));
    ConstByteView image;
    CHECK(provider.find("chrome_elf.dll", &image) && image.size() == elf_bytes.size() && image[0x400] == elf_bytes[0x400]);
    String directory(A);
    CHECK(provider.directory_of("chrome_elf.dll", &directory) && str(directory.view()) == "C:\\App\\131.0.6778.86\\");
    CHECK(!provider.find("other.dll", &image));
    // A program without a manifest only adds its folder.
    FileLibraryProvider plain(A, &fs);
    Spec no_manifest = program_spec;
    no_manifest.manifest.clear();
    const Built bare = build(no_manifest);
    CHECK(plain.add_program("/Windows/App/chrome.exe", ConstByteView(bare.bytes.data(), bare.bytes.size())));
    CHECK(!plain.find("chrome_elf.dll", &image));
  }
}

// The DLL files NtLoadModule keeps (ImageFileCache): one copy of a file for every load of it, a file that changed is read again, unused files go when they do not
// fit in the budget (oldest first) and ones in use stay; several cores at once.
static SharedPointer<ByteBuffer> file_bytes(uint64_t size, uint8_t fill) {
  SharedPointer<ByteBuffer> b = SharedPointer<ByteBuffer>::make(A, A);
  if (b && b->put_zeros(size)) b->writable_view().fill(fill);
  return b;
}
static void test_image_file_cache() {
  {
    ImageFileCache cache(A, 3000);
    const ImageFileCache::Key big{1000, 5, 50, 7}, changed{1000, 6, 60, 7};
    CHECK(!cache.find("/Windows/a.dll", big));
    SharedPointer<ByteBuffer> a = file_bytes(1000, 0xAA);
    SharedPointer<ByteBuffer> kept = cache.put("/Windows/a.dll", big, a);
    CHECK(kept.get() == a.get() && cache.count() == 1 && cache.bytes() == 1000);
    SharedPointer<ByteBuffer> again = cache.find("/Windows/a.dll", big);
    CHECK(again.get() == a.get());      // (the same block: nothing read or taken again)
    SharedPointer<ByteBuffer> second = file_bytes(1000, 0xAA);      // another process read it at the same time: the first copy wins
    CHECK(cache.put("/Windows/a.dll", big, second).get() == a.get() && cache.count() == 1);
    CHECK(!cache.find("/Windows/a.dll", changed) && cache.count() == 0);      // written again since: forgotten (the holders keep their copy)
    CHECK(a && a->size() == 1000 && a->view()[999] == 0xAA);
  }
  {
    ImageFileCache cache(A, 2500);      // room for two unused files of 1000 bytes
    const ImageFileCache::Key k{1000, 1, 1, 1};
    SharedPointer<ByteBuffer> held = cache.put("/held.dll", k, file_bytes(1000, 1));      // in use by a load the whole time
    cache.put("/old.dll", k, file_bytes(1000, 2));
    cache.put("/mid.dll", k, file_bytes(1000, 3));
    CHECK(cache.count() == 3);
    cache.put("/new.dll", k, file_bytes(1000, 4));      // three unused now: the least recently used one goes
    CHECK(cache.count() == 3 && !cache.find("/old.dll", k) && cache.find("/held.dll", k) && cache.find("/mid.dll", k) && cache.find("/new.dll", k));
    held = SharedPointer<ByteBuffer>();
  }
  {
    ImageFileCache cache(A, 1u << 20);
    const ImageFileCache::Key k{4096, 9, 9, 9};
    std::vector<std::thread> threads;
    std::atomic<int> different{0};
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&cache, &k, &different, t]() {
        for (int i = 0; i < 200; ++i) {
          char path[32];
          snprintf(path, sizeof path, "/lib%d.dll", (i + t) % 5);
          SharedPointer<ByteBuffer> b = cache.find(path, k);
          if (!b) b = cache.put(path, k, file_bytes(4096, uint8_t((i + t) % 5)));
          if (!b || b->size() != 4096 || b->view()[0] != uint8_t((i + t) % 5)) different.fetch_add(1);
        }
      });
    }
    for (std::thread& t : threads) t.join();
    CHECK(different.load() == 0 && cache.count() == 5);
  }
}

int main() {
  test_names();
  test_private_assemblies();
  test_load();
  test_failures();
  test_shared_images();
  test_image_file_cache();
  CHECK(A.live == 0);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("ModuleLoaderTest: all passed\n");
  return 0;
}
