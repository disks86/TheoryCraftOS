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

// Host-side tests for the AMD IP discovery table parser and the video engine probe built on it. The tables are built here byte by byte in the layout
// the amdgpu driver documents; the parser is run on every truncation of them and on every single-bit flip (it must never read out of range, and a damaged
// table is either refused or parsed to something that still holds together).
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>
#include <utility>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdIpDiscovery.hpp"
#include "Hardware/VideoCodec/AMD/AmdMediaEngineHw.hpp"
#include "Utility/ByteBuffer.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Ip {
  uint16_t hw_id;
  uint8_t major, minor, revision;
  uint8_t bases = 2;
};

// Builds a table: the container header, the IP list header, then one die per entry of `dies`.
static std::vector<uint8_t> build(const std::vector<std::vector<Ip>>& dies, uint16_t list_version = 2, bool wide = false, bool good_checksum = true) {
  std::vector<uint8_t> t;
  auto u8 = [&](uint32_t v) { t.push_back(static_cast<uint8_t>(v)); };
  auto u16 = [&](uint32_t v) { u8(v); u8(v >> 8); };
  auto u32 = [&](uint32_t v) { u16(v); u16(v >> 16); };
  u32(0x28211407); u16(2); u16(0); u16(0); u16(0);          // signature, version 2.0, checksum, size: patched below
  const uint32_t table_offset = 12 + 6 * 8;
  u16(table_offset);                                          // table 0: the IP list
  while (t.size() < table_offset) u8(0);
  u32(0x53445049); u16(list_version); u16(0); u32(1); u16(static_cast<uint16_t>(dies.size()));
  const uint32_t die_table = static_cast<uint32_t>(t.size());
  for (int i = 0; i < 16; ++i) { u16(i); u16(0); }            // die offsets patched below
  u8(wide ? 1 : 0); u8(0);
  for (size_t d = 0; d < dies.size(); ++d) {
    const uint32_t at = static_cast<uint32_t>(t.size());
    t[die_table + d * 4 + 2] = static_cast<uint8_t>(at); t[die_table + d * 4 + 3] = static_cast<uint8_t>(at >> 8);
    u16(static_cast<uint32_t>(d)); u16(static_cast<uint32_t>(dies[d].size()));
    for (const Ip& ip : dies[d]) {
      u16(ip.hw_id); u8(0); u8(ip.bases); u8(ip.major); u8(ip.minor); u8(ip.revision); u8(0);
      for (uint32_t b = 0; b < ip.bases; ++b) { u32(0x1000 * (b + 1)); if (wide && list_version >= 4) u32(0); }
    }
  }
  t[10] = static_cast<uint8_t>(t.size()); t[11] = static_cast<uint8_t>(t.size() >> 8);
  uint16_t sum = 0;
  for (size_t i = 10; i < t.size(); ++i) sum = static_cast<uint16_t>(sum + t[i]);
  if (!good_checksum) sum = static_cast<uint16_t>(sum + 1);
  t[8] = static_cast<uint8_t>(sum); t[9] = static_cast<uint8_t>(sum >> 8);
  return t;
}

static AmdIpDiscoveryInfo parse(const std::vector<uint8_t>& t) { return amd_parse_ip_discovery(ConstByteView(t.data(), t.size())); }

// A Navi 31 like chip: graphics 11.0.0, two DMA engines, two VCN instances (4.0.0), and a block we do not count.
static const std::vector<std::vector<Ip>> kNavi31 = {{{kAmdHwIdGc, 11, 0, 0}, {kAmdHwIdSdma0, 6, 0, 0}, {kAmdHwIdSdma1, 6, 0, 0}, {kAmdHwIdVcn, 4, 0, 0}, {kAmdHwIdVcn, 4, 0, 0}, {99, 1, 2, 3, 0}}};

static void test_parse() {
  AmdIpDiscoveryInfo info = parse(build(kNavi31));
  CHECK(info.valid && info.checksum_ok && info.binary_version == 0x0200 && info.table_version == 2);
  CHECK(info.ip_count == 6 && info.gc_instances == 1 && info.gc.major == 11 && info.vcn_instances == 2 && info.vcn.major == 4 && info.vcn.minor == 0 && info.sdma_instances == 2);
  // The first instance's version is the one reported; a second die adds its blocks.
  info = parse(build({{{kAmdHwIdVcn, 3, 1, 2}}, {{kAmdHwIdVcn, 3, 0, 0}, {kAmdHwIdGc, 10, 3, 0}}}));
  CHECK(info.valid && info.vcn_instances == 2 && info.vcn.major == 3 && info.vcn.minor == 1 && info.vcn.revision == 2 && info.gc_instances == 1);
  // A chip without video engine blocks.
  info = parse(build({{{kAmdHwIdGc, 9, 4, 1}}}));
  CHECK(info.valid && info.vcn_instances == 0 && info.sdma_instances == 0);
  // 64-bit base addresses (list version 4 with the flag) change the entry size.
  info = parse(build(kNavi31, 4, true));
  CHECK(info.valid && info.vcn_instances == 2 && info.ip_count == 6);
  // A checksum that does not match is reported, not fatal.
  info = parse(build(kNavi31, 2, false, false));
  CHECK(info.valid && !info.checksum_ok && info.vcn_instances == 2);
  // Not a table.
  CHECK(!parse({}).valid && !parse(std::vector<uint8_t>(256, 0)).valid && !parse(std::vector<uint8_t>(256, 0xFF)).valid);
  CHECK(amd_ip_discovery_declared_size(ConstByteView()) == 0);
}

static void test_truncation_and_flips() {
  const std::vector<uint8_t> good = build(kNavi31);
  // Every prefix: never out of range (ASan), and anything that parses is the whole table or a shorter table whose declared size it still fits.
  for (size_t n = 0; n < good.size(); ++n) {
    const AmdIpDiscoveryInfo info = amd_parse_ip_discovery(ConstByteView(good.data(), n));
    CHECK(!info.valid);   // the declared size is more than what is there
  }
  CHECK(parse(good).valid);
  // Every single-bit flip: must not crash; counts stay within what the bytes could hold.
  for (size_t i = 0; i < good.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> t = good;
      t[i] ^= static_cast<uint8_t>(1u << bit);
      const AmdIpDiscoveryInfo info = parse(t);
      CHECK(info.ip_count <= t.size() / 8);
      CHECK(info.vcn_instances <= info.ip_count && info.gc_instances <= info.ip_count && info.sdma_instances <= info.ip_count);
    }
  // A flip in the body (past the checksum field) of an otherwise valid table is caught by the checksum.
  for (size_t i = 12; i < good.size(); ++i) {
    std::vector<uint8_t> t = good;
    t[i] ^= 0x01;
    const AmdIpDiscoveryInfo info = parse(t);
    CHECK(!info.valid || !info.checksum_ok);
  }
  // Every byte forced to 0x00 and 0xFF (lying counts, sizes and offsets): parsed or refused, never read out of range.
  for (size_t i = 0; i < good.size(); ++i)
    for (int v : {0x00, 0xFF}) {
      std::vector<uint8_t> t = good;
      t[i] = static_cast<uint8_t>(v);
      (void)parse(t);
    }
}

// Pretend video memory: plain words the test fills; the window is the last 64 KiB of it.
struct FakeVram {
  std::vector<uint32_t> words;
  explicit FakeVram(const std::vector<uint8_t>& table, size_t bytes = AmdMediaEngineHw::kDiscoveryWindow) : words(bytes / 4, 0) {
    for (size_t i = 0; i < table.size() && i / 4 < words.size(); ++i) words[i / 4] |= static_cast<uint32_t>(table[i]) << (8 * (i % 4));
  }
  MmioRegion region() { return MmioRegion(reinterpret_cast<uint64_t>(words.data()), words.size() * 4); }
};

static void test_probe() {
  TestAllocator a;
  FakeVram vram(build(kNavi31));
  AmdMediaEngineStatus s = AmdMediaEngineHw::probe(a, vram.region(), AmdGpuArchitecture::Rdna3);
  CHECK(s.probed && s.vcn_count() == 2 && s.discovery.vcn.major == 4 && a.live == 0);
  // Generations without a table are not read at all.
  CHECK(!AmdMediaEngineHw::probe(a, vram.region(), AmdGpuArchitecture::Vega).probed && !AmdMediaEngineHw::probe(a, vram.region(), AmdGpuArchitecture::Polaris).probed);
  CHECK(!AmdMediaEngineHw::probe(a, vram.region(), AmdGpuArchitecture::Cdna).probed && !AmdMediaEngineHw::probe(a, vram.region(), AmdGpuArchitecture::Unknown).probed);
  CHECK(AmdMediaEngineHw::supported(AmdGpuArchitecture::Rdna1) && AmdMediaEngineHw::supported(AmdGpuArchitecture::Rdna4));
  // No window, a window that holds no table, a table that declares more than the window, and memory running out.
  CHECK(!AmdMediaEngineHw::probe(a, MmioRegion(), AmdGpuArchitecture::Rdna3).probed);
  FakeVram empty(std::vector<uint8_t>{});
  CHECK(!AmdMediaEngineHw::probe(a, empty.region(), AmdGpuArchitecture::Rdna3).probed);
  FakeVram ones(std::vector<uint8_t>{}); for (uint32_t& w : ones.words) w = 0xFFFFFFFFu;   // a GPU that reads all ones
  CHECK(!AmdMediaEngineHw::probe(a, ones.region(), AmdGpuArchitecture::Rdna3).probed);
  const std::vector<uint8_t> table = build(kNavi31);
  FakeVram small(table, 32);
  CHECK(!AmdMediaEngineHw::probe(a, small.region(), AmdGpuArchitecture::Rdna3).probed);
  a.fail = true;
  CHECK(!AmdMediaEngineHw::probe(a, vram.region(), AmdGpuArchitecture::Rdna3).probed);
  a.fail = false;
  CHECK(a.live == 0);
  // A corrupted table in memory is refused, not half-reported.
  FakeVram bad(table);
  bad.words[20] ^= 0x00010000u;
  s = AmdMediaEngineHw::probe(a, bad.region(), AmdGpuArchitecture::Rdna3);
  CHECK(!s.probed || !s.discovery.checksum_ok);
}

// Pretend memory BAR of an APU: `mib` MiB of words, with tables copied in at the given offsets.
static std::vector<uint32_t> aperture(size_t mib, std::initializer_list<std::pair<size_t, std::vector<uint8_t>>> tables) {
  std::vector<uint32_t> words(mib * (1u << 20) / 4, 0);
  for (const auto& entry : tables)
    for (size_t i = 0; i < entry.second.size(); ++i) words[(entry.first + i) / 4] |= static_cast<uint32_t>(entry.second[i]) << (8 * ((entry.first + i) % 4));
  return words;
}
static MmioRegion window_of(std::vector<uint32_t>& words) { return MmioRegion(reinterpret_cast<uint64_t>(words.data()), words.size() * 4); }

static void test_aperture() {
  TestAllocator a;
  constexpr size_t kMiB = 1u << 20, k64K = 64 * 1024;
  const std::vector<uint8_t> good = build(kNavi31);
  // An APU with a 3 MiB carve-out inside a 4 MiB aperture: the table is at the end of the carve-out, and the scan says how big the carve-out is.
  std::vector<uint32_t> bar = aperture(4, {{3 * kMiB - k64K, good}});
  AmdMediaEngineStatus s = AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna2);
  CHECK(s.probed && s.carve_out_bytes == 3 * kMiB && s.vcn_count() == 2 && s.discovery.checksum_ok && a.live == 0);
  // A carve-out as big as the aperture is found at the very end.
  bar = aperture(4, {{4 * kMiB - k64K, good}});
  s = AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna3_5);
  CHECK(s.probed && s.carve_out_bytes == 4 * kMiB);
  // Two copies: the highest valid one wins; a damaged copy above it is skipped (checksum), and so is one that is only a signature.
  std::vector<uint8_t> damaged = good;
  damaged[40] ^= 0x10;
  const std::vector<uint8_t> bare = {0x07, 0x14, 0x21, 0x28};
  bar = aperture(8, {{2 * kMiB - k64K, good}, {5 * kMiB - k64K, damaged}, {7 * kMiB - k64K, bare}});
  s = AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna3);
  CHECK(s.probed && s.carve_out_bytes == 2 * kMiB);
  // A table that is not on a MiB boundary (minus 64 KiB), no table at all, all ones, and nothing but a damaged one.
  bar = aperture(4, {{2 * kMiB - k64K - 4096, good}});
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna2).probed);
  bar = aperture(4, {});
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna2).probed);
  for (uint32_t& w : bar) w = 0xFFFFFFFFu;
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna2).probed);
  bar = aperture(4, {{3 * kMiB - k64K, damaged}});
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna2).probed);
  // Generations without a table, an empty aperture and one shorter than the table window are not scanned.
  bar = aperture(4, {{3 * kMiB - k64K, good}});
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Vega).probed && !AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Unknown).probed);
  CHECK(!AmdMediaEngineHw::probe_aperture(a, MmioRegion(), AmdGpuArchitecture::Rdna2).probed);
  std::vector<uint32_t> tiny(k64K / 4 - 1, 0);
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(tiny), AmdGpuArchitecture::Rdna2).probed);
  // An aperture that is not a whole number of MiB: the partial MiB is not a boundary.
  std::vector<uint32_t> odd(3 * kMiB / 4 + 1000, 0);
  for (size_t i = 0; i < good.size(); ++i) odd[(2 * kMiB - k64K + i) / 4] |= static_cast<uint32_t>(good[i]) << (8 * (i % 4));
  s = AmdMediaEngineHw::probe_aperture(a, window_of(odd), AmdGpuArchitecture::Rdna2);
  CHECK(s.probed && s.carve_out_bytes == 2 * kMiB);
  // Memory running out is a miss, not a crash.
  a.fail = true;
  bar = aperture(4, {{3 * kMiB - k64K, good}});
  CHECK(!AmdMediaEngineHw::probe_aperture(a, window_of(bar), AmdGpuArchitecture::Rdna2).probed);
  a.fail = false;
  CHECK(a.live == 0);
}

int main() {
  test_parse();
  test_truncation_and_flips();
  test_probe();
  test_aperture();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdDiscoveryTest passed\n");
  return 0;
}
