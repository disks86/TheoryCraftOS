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

// Host-side tests for finding the memory block and the address configuration of a Raven APU's video engine: the register reads (at addresses written out here
// from the Vega10 register-base table, separately from the code under test), the per-variant constant, and the planning of the block inside the carve-out. The plan
// is checked against the properties that keep the engine's DMA off the firmware's memory, over a sweep of carve-out sizes and framebuffer positions.
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdRavenMemory.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kMiB = 1ull << 20;

// The registers the code reads, as dword numbers: block base + register number, from vega10_ip_offset.h and the asic_reg headers.
constexpr uint32_t kRegMemsize = 0x0D20 + 0x00C3;      // NBIO segment 2: RCC_CONFIG_MEMSIZE
constexpr uint32_t kRegFbBase = 0x1A000 + 0x082C;      // MMHUB: MC_VM_FB_LOCATION_BASE
constexpr uint32_t kRegFbTop = 0x1A000 + 0x082D;       // MMHUB: MC_VM_FB_LOCATION_TOP
constexpr uint32_t kRegFbOffset = 0x2000 + 0x096B;     // GC: MC_VM_FB_OFFSET
constexpr uint32_t kRegGbAddrConfig = 0x2000 + 0x063E; // GC: GB_ADDR_CONFIG

struct FakeRegisters final : IVcnRegisters {
  std::map<uint32_t, uint32_t> value;
  std::vector<uint32_t> reads;
  int writes = 0;
  uint32_t read(uint32_t dword) override { reads.push_back(dword); auto it = value.find(dword); return it == value.end() ? 0xFFFFFFFFu : it->second; }   // a block that is off reads all ones
  void write(uint32_t, uint32_t) override { ++writes; }
};

// A machine like a Ryzen 5 2500U: 512 MiB carved out, the memory controller at 0xF400000000 (GPU view), the carve-out at CPU address 0xE0000000.
static void fill(FakeRegisters& r, uint32_t mib, uint64_t gpu_base, uint64_t cpu_base, uint32_t gb) {
  r.value[kRegMemsize] = mib;
  r.value[kRegFbBase] = static_cast<uint32_t>(gpu_base >> 24);
  r.value[kRegFbTop] = static_cast<uint32_t>((gpu_base + uint64_t{mib} * kMiB - (1u << 24)) >> 24);
  r.value[kRegFbOffset] = static_cast<uint32_t>(cpu_base >> 24);
  r.value[kRegGbAddrConfig] = gb;
}

static void test_read() {
  FakeRegisters r;
  fill(r, 512, 0xF400000000ull, 0xE0000000ull, 0x24000042);
  AmdRavenMemoryMap m = amd_read_raven_memory_map(r);
  CHECK(m.read && m.video_memory_bytes == 512 * kMiB && m.gpu_base == 0xF400000000ull && m.cpu_base == 0xE0000000ull && m.hardware_gb_addr_config == 0x24000042);
  CHECK(m.gpu_top == 0xF400000000ull + 512 * kMiB - (1u << 24));
  // Only reads, only the five registers, nothing written.
  CHECK(r.writes == 0 && r.reads.size() == 5);
  for (uint32_t d : r.reads) CHECK(d == kRegMemsize || d == kRegFbBase || d == kRegFbTop || d == kRegFbOffset || d == kRegGbAddrConfig);
  // The base registers are 24-bit fields counted in 16 MiB: bits above them are ignored, as the driver masks them.
  r.value[kRegFbBase] |= 0xAB000000u;
  r.value[kRegFbOffset] = 0x0000E0u;
  m = amd_read_raven_memory_map(r);
  CHECK(m.read && m.gpu_base == 0xF400000000ull && m.cpu_base == 0xE0ull << 24);
  // A card that is off (all ones everywhere), no memory, and sizes that cannot be a carve-out are not read.
  FakeRegisters off;
  CHECK(!amd_read_raven_memory_map(off).read);
  for (uint32_t bad : {0u, 8u, 70000u}) { FakeRegisters b; fill(b, bad, 0xF400000000ull, 0xE0000000ull, 0x24000042); CHECK(!amd_read_raven_memory_map(b).read); }
  { FakeRegisters b; fill(b, 512, 0xF400000000ull, 0xE0000000ull, 0x24000042); b.value[kRegFbOffset] = 0xFFFFFFFFu; CHECK(!amd_read_raven_memory_map(b).read); }
  { FakeRegisters b; fill(b, 512, 0xF400000000ull, 0xE0000000ull, 0x24000042); b.value[kRegFbTop] = 0xFFFFFFFFu; CHECK(!amd_read_raven_memory_map(b).read); }
  // The smallest and largest sizes accepted.
  { FakeRegisters b; fill(b, 16, 0, 0x1000000, 0); CHECK(amd_read_raven_memory_map(b).read); }
  { FakeRegisters b; fill(b, 65536, 0, 0x1000000, 0); CHECK(amd_read_raven_memory_map(b).read); }
}

static void test_gb_addr_config() {
  // Raven and Picasso below revision 8 have one constant, revision 8 and up (Raven2, Dali) the other, whichever device ID.
  CHECK(amd_raven_gb_addr_config(0x15DD, 0x00) == 0x24000042 && amd_raven_gb_addr_config(0x15DD, 0x07) == 0x24000042 && amd_raven_gb_addr_config(0x15D8, 0x00) == 0x24000042);
  CHECK(amd_raven_gb_addr_config(0x15DD, 0x08) == 0x26013041 && amd_raven_gb_addr_config(0x15D8, 0xC8) == 0x26013041 && amd_raven_gb_addr_config(0x15D8, 0x08) == 0x26013041);
  for (uint32_t id = 0; id <= 0xFFFF; ++id)
    CHECK((amd_raven_gb_addr_config(static_cast<uint16_t>(id), 0) != 0) == (id == 0x15DD || id == 0x15D8));
  CHECK(amd_raven_gb_addr_config(0x1636, 0) == 0 && amd_raven_gb_addr_config(0x744C, 0) == 0);   // Renoir (VCN 2), Navi 31
  // The hardware register: equal is a match, zero or all ones says nothing, anything else is a different chip.
  CHECK(amd_check_gb_addr_config(0x24000042, 0x24000042) == AmdGbAddrConfigCheck::Matches);
  CHECK(amd_check_gb_addr_config(0x24000042, 0) == AmdGbAddrConfigCheck::HardwareUnreadable && amd_check_gb_addr_config(0x24000042, 0xFFFFFFFFu) == AmdGbAddrConfigCheck::HardwareUnreadable);
  CHECK(amd_check_gb_addr_config(0x24000042, 0x26013041) == AmdGbAddrConfigCheck::Differs && amd_check_gb_addr_config(0x26013041, 0x24000042) == AmdGbAddrConfigCheck::Differs);
}

static AmdRavenMemoryMap machine(uint32_t mib = 512, uint64_t hw_gb = 0x24000042) {
  FakeRegisters r;
  fill(r, mib, 0xF400000000ull, 0xE0000000ull, static_cast<uint32_t>(hw_gb));
  return amd_read_raven_memory_map(r);
}

constexpr uint64_t kBlock = 1032192;   // AmdVcn1::block_bytes for the shipped firmware

static void test_plan() {
  const uint64_t cpu = 0xE0000000ull, gpu = 0xF400000000ull;
  // 1080p framebuffer at the start of the carve-out: under 9 MiB, so the 9 MiB reservation is what counts. 9 + 16 guard = 25 MiB; the rings follow the block.
  AmdVcn1Plan p = amd_plan_vcn1_memory(machine(), 0x15DD, 0x00, kBlock, cpu, 1920ull * 1080 * 4);
  CHECK(p.result == AmdVcn1Plan::Result::Ok && p.gb_addr_config == 0x24000042 && p.gb_check == AmdGbAddrConfigCheck::Matches);
  CHECK(p.block_offset == 25 * kMiB && p.rings_offset == 26 * kMiB && p.block_bytes == kBlock);
  CHECK(p.block_gpu_address == gpu + 25 * kMiB && p.rings_gpu_address == gpu + 26 * kMiB && p.block_physical == cpu + 25 * kMiB && p.rings_physical == cpu + 26 * kMiB);
  // The work area for decoding sessions: from the MiB after the rings (27) to the tail guard of a 512 MiB carve-out (496).
  CHECK(p.work_offset == 27 * kMiB && p.work_bytes == (512 - 16 - 27) * kMiB && p.work_gpu_address == gpu + 27 * kMiB && p.work_physical == cpu + 27 * kMiB);
  // The same bytes, three addresses: the GPU and CPU views differ by a constant.
  CHECK(p.block_gpu_address - p.block_physical == gpu - cpu && p.rings_gpu_address - p.rings_physical == gpu - cpu);
  // A 4K framebuffer (3840 x 2160 x 4 = 31.64 MiB) pushes it out: 31.64 + 16 guard, rounded up to the next MiB, is 48 MiB.
  p = amd_plan_vcn1_memory(machine(), 0x15DD, 0x00, kBlock, cpu, 3840ull * 2160 * 4);
  CHECK(p.result == AmdVcn1Plan::Result::Ok && p.block_offset == 48 * kMiB && p.rings_offset == 49 * kMiB);
  // A framebuffer that does not start at the carve-out's first byte counts to its end.
  p = amd_plan_vcn1_memory(machine(), 0x15DD, 0x00, kBlock, cpu + 64 * kMiB, 8 * kMiB);
  CHECK(p.result == AmdVcn1Plan::Result::Ok && p.block_offset == 88 * kMiB);
  // Raven2's constant, and a hardware register that agrees, is unreadable, or disagrees.
  p = amd_plan_vcn1_memory(machine(512, 0x26013041), 0x15DD, 0x81, kBlock, cpu, 8 * kMiB);
  CHECK(p.result == AmdVcn1Plan::Result::Ok && p.gb_addr_config == 0x26013041 && p.gb_check == AmdGbAddrConfigCheck::Matches);
  p = amd_plan_vcn1_memory(machine(512, 0), 0x15DD, 0x00, kBlock, cpu, 8 * kMiB);
  CHECK(p.result == AmdVcn1Plan::Result::Ok && p.gb_check == AmdGbAddrConfigCheck::HardwareUnreadable && p.gb_addr_config == 0x24000042);
  p = amd_plan_vcn1_memory(machine(512, 0x26013041), 0x15DD, 0x00, kBlock, cpu, 8 * kMiB);   // a Raven2 chip in a machine we took for Raven
  CHECK(p.result == AmdVcn1Plan::Result::ConfigDiffers && p.block_gpu_address == 0 && p.block_physical == 0);
  // Not Raven; registers that did not answer; no framebuffer.
  CHECK(amd_plan_vcn1_memory(machine(), 0x1636, 0, kBlock, cpu, 8 * kMiB).result == AmdVcn1Plan::Result::NotRaven);
  CHECK(amd_plan_vcn1_memory(AmdRavenMemoryMap(), 0x15DD, 0, kBlock, cpu, 8 * kMiB).result == AmdVcn1Plan::Result::NoMemoryMap);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, 0, 8 * kMiB).result == AmdVcn1Plan::Result::NoFramebuffer);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, cpu, 0).result == AmdVcn1Plan::Result::NoFramebuffer);
  // A framebuffer outside the carve-out means the registers do not describe this machine: below it, above it, straddling its end, bigger than it, or wrapping.
  for (uint64_t where : {cpu - 4096, cpu + 512 * kMiB, uint64_t{0x1000}, UINT64_MAX - 4096})
    CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, where, 8192).result == AmdVcn1Plan::Result::FramebufferOutside);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, cpu + 512 * kMiB - 4096, 8192).result == AmdVcn1Plan::Result::FramebufferOutside);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, cpu, 513 * kMiB).result == AmdVcn1Plan::Result::FramebufferOutside);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, cpu, UINT64_MAX).result == AmdVcn1Plan::Result::FramebufferOutside);
  // Too small: the rings (26 MiB + 12 KiB) plus the 16 MiB tail guard need a little over 42 MiB, so 42 MiB does not fit and 43 does.
  CHECK(amd_plan_vcn1_memory(machine(42), 0x15DD, 0, kBlock, cpu, 8 * kMiB).result == AmdVcn1Plan::Result::TooSmall);
  CHECK(amd_plan_vcn1_memory(machine(43), 0x15DD, 0, kBlock, cpu, 8 * kMiB).result == AmdVcn1Plan::Result::Ok);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, 0, cpu, 8 * kMiB).result == AmdVcn1Plan::Result::TooSmall);
  CHECK(amd_plan_vcn1_memory(machine(), 0x15DD, 0, UINT64_MAX, cpu, 8 * kMiB).result == AmdVcn1Plan::Result::TooSmall);
  for (int r = 0; r <= 6; ++r) CHECK(amd_vcn1_plan_result_name(static_cast<AmdVcn1Plan::Result>(r))[0] != 0);
}

// Over many carve-out sizes and framebuffer positions: whenever a plan is Ok, the block and the rings are inside video memory, clear of everything the firmware
// holds (the first 9 MiB and the framebuffer) by the guard and of the end by the tail guard, page aligned, and they do not overlap each other.
static void test_plan_properties() {
  const uint64_t cpu = 0xE0000000ull;
  int ok = 0, refused = 0;
  for (uint32_t mib : {16u, 24u, 40u, 42u, 43u, 64u, 100u, 256u, 512u, 1024u, 2048u, 8192u})
    for (uint64_t fb_off_mib = 0; fb_off_mib < 40 && fb_off_mib < mib; fb_off_mib += 3)
      for (uint64_t fb_mib : {1ull, 4ull, 8ull, 9ull, 16ull, 33ull}) {
        const AmdRavenMemoryMap map = machine(mib);
        const AmdVcn1Plan p = amd_plan_vcn1_memory(map, 0x15DD, 0, kBlock, cpu + fb_off_mib * kMiB, fb_mib * kMiB);
        if (p.result != AmdVcn1Plan::Result::Ok) { ++refused; CHECK(p.block_gpu_address == 0 && p.rings_physical == 0); continue; }
        ++ok;
        const uint64_t fb_end = (fb_off_mib + fb_mib) * kMiB;
        const uint64_t firmware = fb_end > 9 * kMiB ? fb_end : 9 * kMiB;
        const uint64_t video = uint64_t{mib} * kMiB;
        CHECK(fb_end <= video);
        CHECK(p.block_offset >= firmware + kAmdVcn1GuardBytes && p.block_offset % 4096 == 0 && p.rings_offset % 4096 == 0);
        CHECK(p.block_offset + kBlock <= p.rings_offset);                                   // the block ends before the rings begin
        CHECK(p.rings_offset + AmdVcn1::kRingsTotalBytes + kAmdVcn1TailGuardBytes <= video);   // and the rings before the tail guard
        CHECK(p.block_physical == map.cpu_base + p.block_offset && p.block_gpu_address == map.gpu_base + p.block_offset);
        CHECK(p.rings_physical == map.cpu_base + p.rings_offset && p.rings_gpu_address == map.gpu_base + p.rings_offset);
        // The work area starts after the rings, stays short of the tail guard, is whole MiB, and when there is none it says so with zeros.
        if (p.work_bytes) {
          CHECK(p.work_offset >= p.rings_offset + AmdVcn1::kRingsTotalBytes && p.work_offset % kMiB == 0 && p.work_bytes % kMiB == 0);
          CHECK(p.work_offset + p.work_bytes + kAmdVcn1TailGuardBytes <= video);
          CHECK(p.work_physical == map.cpu_base + p.work_offset && p.work_gpu_address == map.gpu_base + p.work_offset);
        } else {
          CHECK(p.work_offset == 0 && p.work_gpu_address == 0 && p.work_physical == 0);
        }
      }
  CHECK(ok > 50 && refused > 20);   // the sweep covers both
}

struct FakeMapper final : IRegisterMapper {
  std::vector<std::vector<uint32_t>> windows;
  std::vector<std::pair<uint64_t, uint64_t>> asked;
  uint64_t refuse_from = UINT64_MAX;    // physical addresses at and above this are "OS memory"
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    asked.push_back({physical, length});
    if (physical >= refuse_from) return false;
    windows.emplace_back(length / 4, 0);
    *region = MmioRegion(reinterpret_cast<uint64_t>(windows.back().data()), length);
    return true;
  }
};

static void test_map() {
  const uint64_t cpu = 0xE0000000ull;
  const AmdVcn1Plan p = amd_plan_vcn1_memory(machine(), 0x15DD, 0, kBlock, cpu, 8 * kMiB);
  FakeMapper mapper;
  mapper.windows.reserve(4);   // the windows must not move while the test holds regions onto them
  AmdVcn1::Memory memory;
  CHECK(amd_map_vcn1_plan(p, mapper, &memory) && mapper.asked.size() == 2);
  CHECK(mapper.asked[0].first == p.block_physical && mapper.asked[0].second == kBlock && mapper.asked[1].first == p.rings_physical && mapper.asked[1].second == AmdVcn1::kRingsTotalBytes);
  CHECK(memory.block_gpu_address == p.block_gpu_address && memory.rings_gpu_address == p.rings_gpu_address && memory.block.length() == kBlock && memory.rings.length() == 12288);
  // The mapper refusing either range (it is memory the OS owns) fails the whole thing and hands back nothing usable.
  FakeMapper block_refused;
  block_refused.refuse_from = p.block_physical;
  AmdVcn1::Memory a;
  CHECK(!amd_map_vcn1_plan(p, block_refused, &a) && !a.block.valid() && !a.rings.valid() && block_refused.asked.size() == 1);
  FakeMapper rings_refused;
  rings_refused.refuse_from = p.rings_physical;
  AmdVcn1::Memory b;
  CHECK(!amd_map_vcn1_plan(p, rings_refused, &b) && !b.block.valid() && !b.rings.valid());
  // A plan that was not Ok is never mapped, and a null destination is refused.
  FakeMapper untouched;
  AmdVcn1::Memory c;
  CHECK(!amd_map_vcn1_plan(AmdVcn1Plan(), untouched, &c) && untouched.asked.empty());
  CHECK(!amd_map_vcn1_plan(p, untouched, nullptr) && untouched.asked.empty());
}

int main() {
  test_read();
  test_gb_addr_config();
  test_plan();
  test_plan_properties();
  test_map();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdRavenMemoryTest passed\n");
  return 0;
}
