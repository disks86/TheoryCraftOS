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
// The Linux vDSO: the image's layout and symbol table (looked up the way glibc and musl look), the time formula against the kernel's clocks, the data page,
// the mapping and AT_SYSINFO_EHDR, and the vDSO's own code run on this machine (x86-64 hosts) over a data page the test fills.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <time.h>
#if defined(__x86_64__)
#include <sys/mman.h>
#include <sys/time.h>
#include <x86intrin.h>
#endif
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Process/LinuxVdso.hpp"
#include "Process/LinuxVdso/vdso_data.h"
#include "Process/UserProgram.hpp"

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

struct ClockHost final : IProcessHost {
  bool has_clock = true;
  UserClock clock;
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
  bool user_clock(UserClock* out) override { if (!has_clock) return false; *out = clock; return true; }
};

// --- reading the image (bounds-checked: a failed read is a failed lookup) -------------------------------------------------------------------------------------

static bool rd(ConstByteView v, uint64_t at, uint32_t bytes, uint64_t* out) {
  ConstByteView part;
  if (!v.try_subview(at, bytes, &part)) return false;
  uint64_t value = 0;
  for (uint32_t i = 0; i < bytes; ++i) value |= uint64_t{part[i]} << (8 * i);
  *out = value;
  return true;
}
static uint64_t r64(ConstByteView v, uint64_t at) { uint64_t x = ~uint64_t{0}; return rd(v, at, 8, &x) ? x : ~uint64_t{0}; }
static uint64_t r32(ConstByteView v, uint64_t at) { uint64_t x = ~uint64_t{0}; return rd(v, at, 4, &x) ? x : ~uint64_t{0}; }
static uint64_t r16(ConstByteView v, uint64_t at) { uint64_t x = ~uint64_t{0}; return rd(v, at, 2, &x) ? x : ~uint64_t{0}; }
static std::string cstr(ConstByteView v, uint64_t at) {
  std::string s;
  for (uint64_t i = at; i < v.size() && v[i] != 0 && s.size() < 256; ++i) s.push_back(char(v[i]));
  return s;
}

struct Dynamic {
  uint64_t base = ~uint64_t{0}, dynamic = 0, strtab = 0, symtab = 0, hash = 0, gnu_hash = 0, versym = 0, verdef = 0, soname = 0;
  bool relocations = false, needed = false, text_relocations = false;
  bool ok = false;
};

// What both C libraries read first: the program headers (base = where offset 0 is), then the dynamic section (addresses relative to the image).
static Dynamic read_dynamic(ConstByteView image) {
  Dynamic d;
  const uint64_t phoff = r64(image, 32), phentsize = r16(image, 54), phnum = r16(image, 56);
  if (phoff == ~uint64_t{0} || phentsize < 56 || phnum > 64) return d;
  bool have_dynamic = false;
  for (uint64_t i = 0; i < phnum; ++i) {
    const uint64_t ph = phoff + i * phentsize;
    const uint64_t type = r32(image, ph), offset = r64(image, ph + 8), vaddr = r64(image, ph + 16);
    if (type == 1) d.base = offset - vaddr;
    if (type == 2) { d.dynamic = offset; have_dynamic = true; }
  }
  if (!have_dynamic || d.base == ~uint64_t{0}) return d;
  for (uint64_t at = d.dynamic;; at += 16) {
    const uint64_t tag = r64(image, at), value = r64(image, at + 8);
    if (tag == ~uint64_t{0} || value == ~uint64_t{0}) return d;
    if (tag == 0) break;
    if (tag == 5) d.strtab = value; else if (tag == 6) d.symtab = value; else if (tag == 4) d.hash = value; else if (tag == 0x6ffffef5) d.gnu_hash = value;
    else if (tag == 0x6ffffff0) d.versym = value; else if (tag == 0x6ffffffc) d.verdef = value; else if (tag == 14) d.soname = value;
    else if (tag == 7 || tag == 17 || tag == 23 || tag == 0x6ffffff9 || tag == 0x6ffffffa) d.relocations = true;      // RELA, REL, JMPREL, RELACOUNT, RELCOUNT
    else if (tag == 1) d.needed = true;
    else if (tag == 22) d.text_relocations = true;
  }
  d.ok = d.strtab && d.symtab;
  return d;
}

// musl's checkver: the version index of the symbol names a definition (not the base one) whose first name is `version`.
static bool musl_checkver(ConstByteView image, const Dynamic& d, uint64_t vsym, const std::string& version) {
  vsym &= 0x7fff;
  uint64_t def = d.base + d.verdef;
  for (int guard = 0; guard < 64; ++guard) {
    const uint64_t flags = r16(image, def + 2), ndx = r16(image, def + 4), next = r32(image, def + 16);
    if (flags == ~uint64_t{0}) return false;
    if (!(flags & 1) && (ndx & 0x7fff) == vsym) {
      const uint64_t aux = r32(image, def + 12);
      return cstr(image, d.base + d.strtab + r32(image, def + aux)) == version;
    }
    if (next == 0) return false;
    def += next;
  }
  return false;
}

// musl's __vdsosym: walks DT_HASH's symbol count (musl needs DT_HASH), taking a function or object symbol that is defined, global or weak, of that name and version.
static uint64_t musl_lookup(ConstByteView image, const std::string& version, const std::string& name) {
  const Dynamic d = read_dynamic(image);
  if (!d.ok || !d.hash) return 0;
  const uint64_t count = r32(image, d.base + d.hash + 4);
  if (count > 4096) return 0;
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t sym = d.base + d.symtab + i * 24;
    const uint64_t name_at = r32(image, sym), info = r16(image, sym + 4) & 0xff, shndx = r16(image, sym + 6), value = r64(image, sym + 8);
    if (value == ~uint64_t{0}) return 0;
    if (!((1u << (info & 0xf)) & 0x27)) continue;      // NOTYPE, OBJECT, FUNC, COMMON
    if (!((1u << (info >> 4)) & ((1u << 1) | (1u << 2) | (1u << 10)))) continue;      // GLOBAL, WEAK, GNU_UNIQUE
    if (!shndx) continue;
    if (cstr(image, d.base + d.strtab + name_at) != name) continue;
    if (d.versym && d.verdef && !musl_checkver(image, d, r16(image, d.base + d.versym + 2 * i), version)) continue;
    return d.base + value;
  }
  return 0;
}

static uint32_t elf_hash(const std::string& s) {
  uint32_t h = 0;
  for (unsigned char c : s) { h = (h << 4) + c; const uint32_t g = h & 0xf0000000u; if (g) h ^= g >> 24; h &= ~g; }
  return h;
}

// glibc's way: the GNU hash table (bloom filter, bucket, chain), then the version: the symbol's index must name a definition whose hash and name are LINUX_2.6's.
static uint64_t glibc_lookup(ConstByteView image, const std::string& version, const std::string& name) {
  const Dynamic d = read_dynamic(image);
  if (!d.ok || !d.gnu_hash) return 0;
  const uint64_t table = d.base + d.gnu_hash;
  const uint64_t buckets = r32(image, table), symoffset = r32(image, table + 4), bloom_size = r32(image, table + 8), shift = r32(image, table + 12);
  if (buckets == 0 || buckets > 4096 || bloom_size == 0 || bloom_size > 4096 || shift >= 32) return 0;
  uint32_t h = 5381;
  for (unsigned char c : name) h = h * 33 + c;
  const uint64_t word = r64(image, table + 16 + 8 * ((h / 64) % bloom_size));
  if (!((word >> (h % 64)) & (word >> ((h >> shift) % 64)) & 1)) return 0;
  const uint64_t bucket_at = table + 16 + 8 * bloom_size, chain_at = bucket_at + 4 * buckets;
  uint64_t index = r32(image, bucket_at + 4 * (h % buckets));
  if (index == 0 || index < symoffset) return 0;
  for (int guard = 0; guard < 4096; ++guard, ++index) {
    const uint64_t h2 = r32(image, chain_at + 4 * (index - symoffset));
    if (h2 == ~uint64_t{0}) return 0;
    const uint64_t sym = d.base + d.symtab + index * 24;
    if ((h | 1) == (h2 | 1) && cstr(image, d.base + d.strtab + r32(image, sym)) == name) {
      const uint64_t vsym = r16(image, d.base + d.versym + 2 * index) & 0x7fff;
      for (uint64_t def = d.base + d.verdef, g2 = 0; g2 < 64; ++g2) {
        if (r16(image, def + 4) == vsym && r32(image, def + 8) == elf_hash(version) &&
            cstr(image, d.base + d.strtab + r32(image, def + r32(image, def + 12))) == version)
          return d.base + r64(image, sym + 8);
        const uint64_t next = r32(image, def + 16);
        if (next == 0 || next == ~uint64_t{0}) break;
        def += next;
      }
    }
    if (h2 & 1) return 0;
  }
  return 0;
}

static const char* const kNames[] = {"__vdso_clock_gettime", "__vdso_gettimeofday", "__vdso_time", "__vdso_clock_getres", "clock_gettime", "gettimeofday", "time", "clock_getres"};

static void test_image() {
  const ConstByteView image = linux_vdso_image(UserArch::X86_64);
  CHECK(image.size() > 64 && image.size() <= 2 * AddressSpace::kPageSize);
  CHECK(linux_vdso_image(UserArch::AArch64).empty() && linux_vdso_image(UserArch::RiscV64).empty() && linux_vdso_image(UserArch::X86_32).empty());
  CHECK(r32(image, 0) == 0x464C457Fu && image[4] == 2 && image[5] == 1 && r16(image, 16) == 3 && r16(image, 18) == 62);      // ELF64 LE, ET_DYN, x86-64
  // One loadable segment, read + execute, offset = address = 0, nothing past the file (the kernel maps the file as it is).
  int loads = 0, notes = 0;
  const uint64_t phoff = r64(image, 32), phnum = r16(image, 56);
  for (uint64_t i = 0; i < phnum; ++i) {
    const uint64_t ph = phoff + i * 56, type = r32(image, ph);
    if (type == 1) {
      ++loads;
      CHECK(r32(image, ph + 4) == 5 && r64(image, ph + 8) == 0 && r64(image, ph + 16) == 0 && r64(image, ph + 32) == r64(image, ph + 40) && r64(image, ph + 32) <= image.size());
    }
    if (type == 4) {      // the Linux version note: 6.1.0, what uname reports
      ++notes;
      const uint64_t at = r64(image, ph + 8);
      CHECK(r32(image, at) == 6 && r32(image, at + 4) == 4 && cstr(image, at + 12) == "Linux" && r32(image, at + 20) == 0x060100);
    }
  }
  CHECK(loads == 1 && notes == 1);
  const Dynamic d = read_dynamic(image);
  CHECK(d.ok && d.base == 0 && d.hash && d.gnu_hash && d.versym && d.verdef);
  CHECK(!d.relocations && !d.needed && !d.text_relocations);      // nothing to relocate or load: glibc and musl use it in place
  CHECK(cstr(image, d.strtab + d.soname) == "linux-vdso.so.1");
  // Every function, both ways, at the same address; and the plain names are the same code as the __vdso_ ones.
  uint64_t first[4] = {};
  for (int i = 0; i < 8; ++i) {
    const uint64_t musl = musl_lookup(image, "LINUX_2.6", kNames[i]), glibc = glibc_lookup(image, "LINUX_2.6", kNames[i]);
    if (!(musl != 0 && musl == glibc)) printf("  symbol %s: musl %llx glibc %llx\n", kNames[i], (unsigned long long)musl, (unsigned long long)glibc);
    CHECK(musl != 0 && musl == glibc && musl < image.size());
    if (i < 4) first[i] = musl; else CHECK(musl == first[i - 4]);
  }
  CHECK(musl_lookup(image, "LINUX_2.5", "__vdso_clock_gettime") == 0 && glibc_lookup(image, "LINUX_2.5", "__vdso_clock_gettime") == 0);      // the wrong version
  CHECK(musl_lookup(image, "LINUX_2.6", "__vdso_getcpu") == 0 && glibc_lookup(image, "LINUX_2.6", "__vdso_getcpu") == 0);                  // not provided
  CHECK(musl_lookup(image, "LINUX_2.6", "tcos_vvar") == 0 && glibc_lookup(image, "LINUX_2.6", "tcos_vvar") == 0);                          // not exported
  // Damaged copies never send a lookup out of bounds (the readers are what a C library would do; ASan watches).
  std::vector<uint8_t> copy(image.data(), image.data() + image.size());
  for (size_t i = 0; i < copy.size(); i += 3) for (int bit = 0; bit < 8; bit += 3) {
    copy[i] ^= uint8_t(1 << bit);
    (void)musl_lookup(ConstByteView(copy.data(), copy.size()), "LINUX_2.6", "__vdso_time");
    (void)glibc_lookup(ConstByteView(copy.data(), copy.size()), "LINUX_2.6", "__vdso_time");
    copy[i] ^= uint8_t(1 << bit);
  }
  for (size_t n = 0; n < image.size(); n += 61) {
    (void)musl_lookup(image.first(n), "LINUX_2.6", "__vdso_clock_gettime");
    (void)glibc_lookup(image.first(n), "LINUX_2.6", "__vdso_clock_gettime");
  }
}

// The kernel's clocks, written out as in hardware::uptime_us() and ProgramHost: monotonic = uptime_us() * 1000, realtime = start_real + (monotonic - start_mono).
static uint64_t kernel_monotonic_ns(uint64_t tsc, uint64_t origin, uint64_t rate) { return (tsc - origin) / rate * 1000; }

static void test_formula() {
  tcos_vdso_data data = {};
  data.magic = TCOS_VDSO_MAGIC;
  uint64_t ns = 7;
  CHECK(!tcos_vdso_clock_ns(&data, TCOS_CLOCK_MONOTONIC, 12345, &ns) && ns == 7);      // no rate: ask the kernel
  const uint64_t rates[] = {1, 3, 1000, 2894, 4199, 1ull << 20};
  const uint64_t origins[] = {0, 1, 123456789, 1ull << 40, ~uint64_t{0} - (1ull << 30)};
  const uint64_t start_real = 1795046400ull * 1000000000ull;      // 2026-11-19, whole seconds as ProgramHost takes them
  uint64_t seed = 88172645463325252ull;
  for (uint64_t rate : rates) for (uint64_t origin : origins) {
    data.tsc_ticks_per_us = rate;
    data.tsc_at_start = origin;
    const uint64_t start_mono = kernel_monotonic_ns(origin + rate * 5000 + 17, origin, rate);
    data.realtime_offset_ns = start_real - start_mono;
    for (int i = 0; i < 2000; ++i) {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      const uint64_t span = ~uint64_t{0} - origin;
      const uint64_t tsc = origin + (span ? seed % (span < (1ull << 50) ? span : (1ull << 50)) : 0);
      const uint64_t mono = kernel_monotonic_ns(tsc, origin, rate);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_MONOTONIC, tsc, &ns) && ns == mono);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_MONOTONIC_RAW, tsc, &ns) && ns == mono);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_MONOTONIC_COARSE, tsc, &ns) && ns == mono);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_BOOTTIME, tsc, &ns) && ns == mono);
      const uint64_t real = start_real + (mono - start_mono);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_REALTIME, tsc, &ns) && ns == real);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_REALTIME_COARSE, tsc, &ns) && ns == real);
      CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_TAI, tsc, &ns) && ns == real + 37000000000ull);
      CHECK(ns % 1000 == 0);      // whole microseconds, like the kernel's clock
      if (g_failed) return;
    }
    // A counter a little behind the start (another core) reads as 0, never as a time far in the future.
    if (origin > 0) CHECK(tcos_vdso_clock_ns(&data, TCOS_CLOCK_MONOTONIC, origin - 1, &ns) && ns == 0);
  }
  // Clocks the vDSO leaves to the kernel.
  const int others[] = {2, 3, 8, 9, 10, 12, 99, -1, -6};
  for (int clock : others) CHECK(!tcos_vdso_clock_ns(&data, clock, 1, &ns));
}

static void test_data_page() {
  uint8_t page[AddressSpace::kPageSize];
  memset(page, 0xAB, sizeof(page));
  UserClock clock;
  clock.tsc_ticks_per_us = 2894;
  clock.tsc_at_start = 0x1122334455667788ull;
  clock.realtime_offset_ns = 0x8877665544332211ull;
  CHECK(fill_linux_vdso_data(&clock, ByteView(page, sizeof(page))));
  const ConstByteView v(page, sizeof(page));
  CHECK(r64(v, TCOS_VDSO_DATA_MAGIC_AT) == TCOS_VDSO_MAGIC && r32(v, TCOS_VDSO_DATA_VERSION_AT) == TCOS_VDSO_VERSION && r32(v, TCOS_VDSO_DATA_FLAGS_AT) == 0);
  CHECK(r64(v, TCOS_VDSO_DATA_RATE_AT) == 2894 && r64(v, TCOS_VDSO_DATA_START_AT) == 0x1122334455667788ull && r64(v, TCOS_VDSO_DATA_OFFSET_AT) == 0x8877665544332211ull);
  bool rest_zero = true;
  for (uint64_t i = TCOS_VDSO_DATA_BYTES; i < sizeof(page); ++i) rest_zero = rest_zero && page[i] == 0;
  CHECK(rest_zero);
  // The bytes are the structure the vDSO reads.
  tcos_vdso_data as_struct;
  memcpy(&as_struct, page, sizeof(as_struct));
  CHECK(as_struct.tsc_ticks_per_us == 2894 && as_struct.tsc_at_start == clock.tsc_at_start && as_struct.realtime_offset_ns == clock.realtime_offset_ns);
  CHECK(fill_linux_vdso_data(nullptr, ByteView(page, sizeof(page))) && r64(v, TCOS_VDSO_DATA_RATE_AT) == 0 && r64(v, TCOS_VDSO_DATA_MAGIC_AT) == TCOS_VDSO_MAGIC);
  CHECK(!fill_linux_vdso_data(&clock, ByteView(page, TCOS_VDSO_DATA_BYTES - 1)));
}

static void test_mapping() {
  ClockHost host;
  host.clock.tsc_ticks_per_us = 3000;
  host.clock.tsc_at_start = 1000000;
  host.clock.realtime_offset_ns = 42;
  AddressSpace space(A);
  uint64_t base = 0;
  CHECK(map_linux_vdso(space, UserArch::X86_64, host, &base) == LoadStatus::Ok);
  CHECK(base == kLinuxVdsoHint + AddressSpace::kPageSize);
  const ConstByteView image = linux_vdso_image(UserArch::X86_64);
  std::vector<uint8_t> back(image.size());
  CHECK(space.read(base, ByteView(back.data(), back.size())) && memcmp(back.data(), image.data(), image.size()) == 0);
  CHECK(space.check(base, image.size(), kProtRead | kProtExecute) && !space.check(base, 1, kProtWrite));
  CHECK(space.check(base - AddressSpace::kPageSize, AddressSpace::kPageSize, kProtRead) && !space.check(base - AddressSpace::kPageSize, 1, kProtWrite) &&
        !space.check(base - AddressSpace::kPageSize, 1, kProtExecute));
  uint8_t data[TCOS_VDSO_DATA_BYTES];
  CHECK(space.read(base - AddressSpace::kPageSize, ByteView(data, sizeof(data))));
  const ConstByteView dv(data, sizeof(data));
  CHECK(r64(dv, TCOS_VDSO_DATA_RATE_AT) == 3000 && r64(dv, TCOS_VDSO_DATA_START_AT) == 1000000 && r64(dv, TCOS_VDSO_DATA_OFFSET_AT) == 42);
  // A second one finds other room; a fork's copy gets its own host's clock.
  uint64_t second = 0;
  CHECK(map_linux_vdso(space, UserArch::X86_64, host, &second) == LoadStatus::Ok && second != base);
  ClockHost other;
  other.has_clock = false;
  CHECK(refresh_linux_vdso_data(space, base, other) && space.read(base - AddressSpace::kPageSize, ByteView(data, sizeof(data))) && r64(dv, TCOS_VDSO_DATA_RATE_AT) == 0);
  CHECK(!refresh_linux_vdso_data(space, 0, other));
  // Nothing for other CPUs; no room is no mapping.
  uint64_t none = 0;
  CHECK(map_linux_vdso(space, UserArch::AArch64, host, &none) == LoadStatus::Unsupported && none == 0);
  AddressSpace tight(A, AddressSpace::kPageSize);
  CHECK(map_linux_vdso(tight, UserArch::X86_64, host, &none) == LoadStatus::OutOfMemory && tight.region_count() == 0);
}

// A static x86-64 program (entry 0x400100, prints nothing) for UserProgram::load.
static std::vector<uint8_t> make_elf(uint16_t machine = 62) {
  std::vector<uint8_t> b(0x1010, 0);
  auto put = [&](uint64_t off, uint64_t v, int n) { for (int i = 0; i < n; ++i) b[off + i] = uint8_t(v >> (8 * i)); };
  b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
  put(16, 2, 2); put(18, machine, 2); put(20, 1, 4); put(24, 0x400100, 8); put(32, 64, 8);
  put(52, 64, 2); put(54, 56, 2); put(56, 1, 2); put(58, 64, 2);
  put(64, 1, 4); put(68, 5, 4); put(72, 0, 8); put(80, 0x400000, 8); put(88, 0x400000, 8); put(96, 0x1000, 8); put(104, 0x1000, 8); put(112, 0x1000, 8);
  b[0x100] = 0xC3;
  return b;
}

static uint64_t aux_value(AddressSpace& space, uint64_t sp, uint64_t key, bool* found) {
  uint64_t argc = 0, at = sp, word = 0;
  *found = false;
  uint8_t raw[8];
  auto read = [&](uint64_t a) { if (!space.read(a, ByteView(raw, 8))) return ~uint64_t{0}; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i); return v; };
  argc = read(at);
  at += 8 * (argc + 2);
  while ((word = read(at)) != 0 && word != ~uint64_t{0}) at += 8;      // the environment
  for (at += 8; at < sp + 4096; at += 16) {
    const uint64_t k = read(at), v = read(at + 8);
    if (k == 0 || k == ~uint64_t{0}) break;
    if (k == key) { *found = true; return v; }
  }
  return 0;
}

static void test_user_program() {
  ClockHost host;
  host.clock.tsc_ticks_per_us = 2000;
  host.clock.tsc_at_start = 77;
  host.clock.realtime_offset_ns = 5;
  uint8_t random[16] = {};
  const StringView args[] = {"prog"};
  const std::vector<uint8_t> elf = make_elf();
  {
    UserProgram u(A, 1, host);
    CHECK(u.load(ConstByteView(elf.data(), elf.size()), ArrayView<const StringView>(args, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) == LoadStatus::Ok);
    bool found = false;
    const uint64_t ehdr = aux_value(u.address_space(), u.start().stack_pointer, 33, &found);
    CHECK(found && ehdr == u.image().vdso_base && ehdr != 0);
    uint8_t magic[4] = {};
    CHECK(u.address_space().read(ehdr, ByteView(magic, 4)) && magic[0] == 0x7F && magic[1] == 'E');
    uint8_t data[TCOS_VDSO_DATA_BYTES];
    CHECK(u.address_space().read(ehdr - AddressSpace::kPageSize, ByteView(data, sizeof(data))) && r64(ConstByteView(data, sizeof(data)), TCOS_VDSO_DATA_RATE_AT) == 2000);
    // The program's own mappings land elsewhere.
    SyscallRequest r;
    r.number = 9; r.args[0] = 0; r.args[1] = 1 << 20; r.args[2] = 3; r.args[3] = 0x22; r.args[4] = ~uint64_t{0};
    const uint64_t mapped = u.process()->syscall(r);
    CHECK(mapped < AddressSpace::kUserTop && (mapped + (1 << 20) <= ehdr - AddressSpace::kPageSize || mapped >= ehdr + 2 * AddressSpace::kPageSize));
  }
  {      // an AArch64 program has none (the emulator's guests: AT_IGNORE instead)
    UserProgram u(A, 2, host);
    const std::vector<uint8_t> arm = make_elf(183);
    CHECK(u.load(ConstByteView(arm.data(), arm.size()), ArrayView<const StringView>(args, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) == LoadStatus::Ok);
    bool found = false;
    (void)aux_value(u.address_space(), u.start().stack_pointer, 33, &found);
    CHECK(!found && u.image().vdso_base == 0);
  }
}

#if defined(__x86_64__)
// --- the vDSO's code itself, on this machine ------------------------------------------------------------------------------------------------------------------------

struct Mapped {
  uint8_t* memory = nullptr;
  uint64_t bytes = 0;
  uint8_t* data() { return memory; }
  uint8_t* image() { return memory + AddressSpace::kPageSize; }
  ~Mapped() { if (memory) munmap(memory, bytes); }
};
typedef int (*ClockGettime)(int, struct timespec*);
typedef int (*Gettimeofday)(struct timeval*, void*);
typedef long (*Time)(long*);
typedef int (*ClockGetres)(int, struct timespec*);
// The one cast: the address of code in the executable mapping, which stays mapped while `m` lives, as the function type its C declaration has.
template <typename F> static F function_at(Mapped& m, uint64_t offset) { return reinterpret_cast<F>(m.image() + offset); }

static bool map_here(Mapped* m, const UserClock* clock) {
  const ConstByteView image = linux_vdso_image(UserArch::X86_64);
  m->bytes = AddressSpace::kPageSize + (image.size() + AddressSpace::kPageSize - 1) / AddressSpace::kPageSize * AddressSpace::kPageSize;
  void* at = mmap(nullptr, m->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (at == MAP_FAILED) return false;
  m->memory = static_cast<uint8_t*>(at);
  ByteView(m->image(), image.size()).copy_from(image);
  return fill_linux_vdso_data(clock, ByteView(m->data(), AddressSpace::kPageSize)) && mprotect(m->data(), AddressSpace::kPageSize, PROT_READ) == 0 &&
         mprotect(m->image(), m->bytes - AddressSpace::kPageSize, PROT_READ | PROT_EXEC) == 0;
}

static void test_run_here() {
  const ConstByteView image = linux_vdso_image(UserArch::X86_64);
  const uint64_t offsets[4] = {musl_lookup(image, "LINUX_2.6", "__vdso_clock_gettime"), musl_lookup(image, "LINUX_2.6", "__vdso_gettimeofday"),
                               musl_lookup(image, "LINUX_2.6", "__vdso_time"), musl_lookup(image, "LINUX_2.6", "__vdso_clock_getres")};
  for (uint64_t o : offsets) CHECK(o != 0);
  if (g_failed) return;
  // With a clock: the time comes from the counter and the data page alone (a rate that is not this machine's: proof that no system call answered).
  UserClock clock;
  clock.tsc_ticks_per_us = 1234;
  clock.tsc_at_start = __rdtsc() - 1234ull * 1000000 * 3600;      // an hour ago
  clock.realtime_offset_ns = 1000000000ull * 1000000000ull;           // realtime = monotonic + 10^9 s
  Mapped m;
  CHECK(map_here(&m, &clock));
  if (g_failed) return;
  ClockGettime cg = function_at<ClockGettime>(m, offsets[0]);
  Gettimeofday gt = function_at<Gettimeofday>(m, offsets[1]);
  Time tm = function_at<Time>(m, offsets[2]);
  ClockGetres gr = function_at<ClockGetres>(m, offsets[3]);
  tcos_vdso_data data;
  memcpy(&data, m.data(), sizeof(data));
  auto expect = [&](int which, uint64_t tsc) { uint64_t ns = 0; tcos_vdso_clock_ns(&data, which, tsc, &ns); return ns; };
  const int clocks[] = {0, 1, 4, 5, 6, 7, 11};
  for (int c : clocks) {
    struct timespec ts = {};
    const uint64_t before = __rdtsc();
    CHECK(cg(c, &ts) == 0);
    const uint64_t after = __rdtsc();
    const uint64_t got = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    CHECK(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000 && got >= expect(c, before) && got <= expect(c, after));
  }
  {
    struct timespec ts = {};
    CHECK(cg(1, &ts) == 0 && ts.tv_sec >= 3600 && ts.tv_sec < 3700);      // the hour since the made-up start
    struct timeval tv = {};
    const uint64_t before = __rdtsc();
    CHECK(gt(&tv, nullptr) == 0);
    const uint64_t after = __rdtsc();
    const uint64_t us = uint64_t(tv.tv_sec) * 1000000 + uint64_t(tv.tv_usec);
    CHECK(tv.tv_usec >= 0 && tv.tv_usec < 1000000 && us >= expect(0, before) / 1000 && us <= expect(0, after) / 1000);
    CHECK(gt(nullptr, nullptr) == 0);
    long t = 0;
    const long seconds = tm(&t);
    CHECK(seconds == t && uint64_t(seconds) >= expect(0, before) / 1000000000ull && uint64_t(seconds) <= expect(0, __rdtsc()) / 1000000000ull);
    CHECK(tm(nullptr) >= seconds);
    struct timespec res = {5, 5};
    CHECK(gr(1, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 1 && gr(11, nullptr) == 0);
  }
  // What it leaves to the kernel (here: this machine's Linux): the CPU-time clocks, unknown clocks, a time zone, clock_getres of an unknown clock.
  {
    struct timespec ts = {-1, -1};
    CHECK(cg(2, &ts) == 0 && ts.tv_sec >= 0 && ts.tv_sec < 100000);      // CLOCK_PROCESS_CPUTIME_ID: this process's CPU time
    CHECK(cg(99, &ts) == -22);                                             // EINVAL, as the system call answers
    struct timespec res;
    CHECK(gr(99, &res) == -22 && gr(-1, &res) == -22);
    struct timeval tv = {};
    struct timezone tz = {77, 77};
    CHECK(gt(&tv, &tz) == 0 && tv.tv_sec > 1700000000 && tv.tv_sec < 1700000000 + (1ll << 31));      // this machine's wall clock, not the made-up one
  }
  // No rate published: every call is the system call, and agrees with what this machine's C library says.
  Mapped plain;
  CHECK(map_here(&plain, nullptr));
  if (g_failed) return;
  ClockGettime pcg = function_at<ClockGettime>(plain, offsets[0]);
  Gettimeofday pgt = function_at<Gettimeofday>(plain, offsets[1]);
  Time ptm = function_at<Time>(plain, offsets[2]);
  for (int c : {0, 1, 7}) {
    struct timespec ours = {}, before = {}, after = {};
    clock_gettime(c, &before);
    CHECK(pcg(c, &ours) == 0);
    clock_gettime(c, &after);
    const auto ns = [](const timespec& t) { return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec); };
    CHECK(ns(ours) >= ns(before) && ns(ours) <= ns(after));
  }
  struct timeval tv = {};
  CHECK(pgt(&tv, nullptr) == 0 && tv.tv_sec > 1700000000);
  const long now = ::time(nullptr);
  CHECK(ptm(nullptr) >= now && ptm(nullptr) <= now + 2);
}
#endif

int main() {
  test_image();
  test_formula();
  test_data_page();
  test_mapping();
  test_user_program();
#if defined(__x86_64__)
  test_run_here();
#endif
  if (A.live != 0) { printf("FAILED: %d allocations leaked\n", A.live); ++g_failed; }
  if (g_failed) { printf("LinuxVdsoTest: %d failures\n", g_failed); return 1; }
  printf("LinuxVdsoTest: all passed\n");
  return 0;
}
