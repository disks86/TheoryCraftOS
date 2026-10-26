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

// Host-side tests for the memory discovery and planning of the Vega M's video engine (AmdGmc8Memory): the memory controller's registers read the way amdgpu reads them,
// the arithmetic that places the engine's block, ring and work area inside the part of video memory the CPU can reach, and the mapping of the plan. The register
// numbers here are typed in from AMD's bif_5_0_d.h and gmc_8_1_d.h, independently of the generated ones the code uses.
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint32_t kConfigMemsize = 0x150a, kFbLocation = 0x809, kHdpFlush = 0x1520;
constexpr uint64_t MiB = 1ull << 20;

struct FakeRegs final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<char, uint32_t>> log;   // 'r' / 'w' and the register
  uint32_t read(uint32_t r) override { log.push_back({'r', r}); auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }
  void write(uint32_t r, uint32_t v) override { log.push_back({'w', r}); regs[r] = v; }
};

// A card with `mib` MiB of memory whose controller starts at `base_unit` * 16 MiB.
static void set_card(FakeRegs& r, uint32_t mib, uint32_t base_unit) {
  r.regs[kConfigMemsize] = mib;
  const uint32_t top_unit = base_unit + (mib + 15) / 16 - 1;
  r.regs[kFbLocation] = (top_unit << 16) | base_unit;
}

static void test_read_map() {
  FakeRegs r;
  set_card(r, 4096, 0x00F4);   // the Vega M: 4 GiB of HBM2
  AmdGmc8MemoryMap m = amd_read_gmc8_memory_map(r);
  CHECK(m.read && m.video_memory_bytes == 4096 * MiB && m.gpu_base == 0xF400'0000ull && m.gpu_top == 0xF400'0000ull + 4096 * MiB);
  set_card(r, 4096, 0);
  m = amd_read_gmc8_memory_map(r);
  CHECK(m.read && m.gpu_base == 0 && m.gpu_top == 4096 * MiB);
  // A size that is not a multiple of 16 MiB: the controller's span is rounded up.
  set_card(r, 2056, 0x10);
  m = amd_read_gmc8_memory_map(r);
  CHECK(m.read && m.video_memory_bytes == 2056 * MiB && m.gpu_top - m.gpu_base == 2064 * MiB);
  // Garbage in the upper 16 bits of the size: the low half is used (as the driver does) if there is one, else it is no size at all.
  set_card(r, 4096, 0x00F4);
  r.regs[kConfigMemsize] = 0x12341000;
  CHECK(amd_read_gmc8_memory_map(r).read && amd_read_gmc8_memory_map(r).video_memory_bytes == 4096 * MiB);
  r.regs[kConfigMemsize] = 0x12340000;
  CHECK(!amd_read_gmc8_memory_map(r).read);
  // A card that does not answer, or answers nonsense.
  FakeRegs dead;
  CHECK(!amd_read_gmc8_memory_map(dead).read);
  dead.regs[kConfigMemsize] = 0xFFFFFFFF;
  dead.regs[kFbLocation] = 0xFFFFFFFF;
  CHECK(!amd_read_gmc8_memory_map(dead).read);
  set_card(r, 4096, 0x00F4);
  r.regs[kFbLocation] = 0xFFFFFFFF;
  CHECK(!amd_read_gmc8_memory_map(r).read);
  r.regs[kFbLocation] = (0x00F3u << 16) | 0x00F4;   // top below base
  CHECK(!amd_read_gmc8_memory_map(r).read);
  r.regs[kFbLocation] = ((0x00F4u + 100) << 16) | 0x00F4;   // the span is smaller than the memory
  CHECK(!amd_read_gmc8_memory_map(r).read);
  r.regs[kFbLocation] = ((0x00F4u + 1000) << 16) | 0x00F4;   // far bigger than it
  CHECK(!amd_read_gmc8_memory_map(r).read);
  r.regs[kConfigMemsize] = 0x0000FFFF;                       // 64 GiB - 1 MiB: not a card
  r.regs[kFbLocation] = (0xFFFFu << 16);
  CHECK(!amd_read_gmc8_memory_map(r).read);
}

static void test_flush() {
  FakeRegs r;
  amd_gmc8_flush_hdp(r);
  CHECK(r.log.size() == 2 && r.log[0] == std::make_pair('w', kHdpFlush) && r.log[1] == std::make_pair('r', kHdpFlush) && r.regs[kHdpFlush] == 1);
}

static AmdGmc8MemoryMap card(uint32_t mib = 4096, uint32_t base_unit = 0x00F4) {
  FakeRegs r;
  set_card(r, mib, base_unit);
  return amd_read_gmc8_memory_map(r);
}

static void test_plan() {
  const AmdGmc8MemoryMap map = card();
  const uint64_t block = 3'000'000;   // about 2.9 MiB: rounds up to 3
  // 256 MiB aperture: the memory ends at 240 MiB; 96 MiB of work, 1 MiB of ring, 3 MiB of block before that.
  AmdUvd6Plan p = amd_plan_uvd6_memory(map, 256 * MiB, block);
  CHECK(p.result == AmdUvd6Plan::Result::Ok && p.visible_bytes == 256 * MiB);
  CHECK(p.work_offset == 144 * MiB && p.work_bytes == 96 * MiB && p.ring_offset == 143 * MiB && p.block_offset == 140 * MiB && p.block_bytes == block);
  CHECK(p.block_gpu_address == map.gpu_base + 140 * MiB && p.ring_gpu_address == map.gpu_base + 143 * MiB && p.work_gpu_address == map.gpu_base + 144 * MiB);
  // Resizable BAR on: the whole 4 GiB is reachable and the memory goes at its end.
  p = amd_plan_uvd6_memory(map, 4096 * MiB, block);
  CHECK(p.result == AmdUvd6Plan::Result::Ok && p.visible_bytes == 4096 * MiB && p.work_offset + p.work_bytes == 4080 * MiB && p.block_offset == 4080 * MiB - 96 * MiB - 4 * MiB);
  // An aperture bigger than the memory is the memory; one that is not whole MiB is cut.
  p = amd_plan_uvd6_memory(card(128, 0), 256 * MiB, block);
  CHECK(p.result == AmdUvd6Plan::Result::Ok && p.visible_bytes == 128 * MiB && p.work_offset + p.work_bytes == 112 * MiB);
  p = amd_plan_uvd6_memory(map, 256 * MiB + 12345, block);
  CHECK(p.result == AmdUvd6Plan::Result::Ok && p.visible_bytes == 256 * MiB);
  // Little room: the work area shrinks to what there is, as long as it is 8 MiB.
  p = amd_plan_uvd6_memory(map, 64 * MiB, block);
  CHECK(p.result == AmdUvd6Plan::Result::Ok && p.work_bytes == 19 * MiB && p.block_offset >= 25 * MiB && p.work_offset + p.work_bytes == 48 * MiB);
  p = amd_plan_uvd6_memory(map, 64 * MiB, 14 * MiB);   // 48 - 25 = 23 MiB of room: 14 + 1 leaves 8
  CHECK(p.result == AmdUvd6Plan::Result::Ok && p.work_bytes == 8 * MiB && p.block_offset == 25 * MiB);
  p = amd_plan_uvd6_memory(map, 64 * MiB, 15 * MiB);   // one MiB short of the minimum work area
  CHECK(p.result == AmdUvd6Plan::Result::TooSmall);
  // Refused: no memory map, a memory BAR that is too small, a block that is nothing or bigger than anything.
  CHECK(amd_plan_uvd6_memory(AmdGmc8MemoryMap(), 256 * MiB, block).result == AmdUvd6Plan::Result::NoMemoryMap);
  CHECK(amd_plan_uvd6_memory(map, 32 * MiB, block).result == AmdUvd6Plan::Result::ApertureTooSmall);
  CHECK(amd_plan_uvd6_memory(map, 0, block).result == AmdUvd6Plan::Result::ApertureTooSmall);
  CHECK(amd_plan_uvd6_memory(map, 256 * MiB, 0).result == AmdUvd6Plan::Result::TooSmall);
  CHECK(amd_plan_uvd6_memory(map, 256 * MiB, 1ull << 40).result == AmdUvd6Plan::Result::TooSmall);
  CHECK(amd_plan_uvd6_memory(map, 256 * MiB, UINT64_MAX).result == AmdUvd6Plan::Result::TooSmall);
  CHECK(amd_plan_uvd6_memory(map, 256 * MiB, block, 0).result == AmdUvd6Plan::Result::TooSmall);   // (a work area under the minimum is no plan)
  CHECK(amd_plan_uvd6_memory(map, 256 * MiB, block, UINT64_MAX).result == AmdUvd6Plan::Result::Ok);  // (an absurd request is the room there is)
  for (const AmdUvd6Plan::Result r : {AmdUvd6Plan::Result::Ok, AmdUvd6Plan::Result::NoMemoryMap, AmdUvd6Plan::Result::ApertureTooSmall, AmdUvd6Plan::Result::TooSmall})
    CHECK(amd_uvd6_plan_result_name(r)[0] != 0);

  // Whatever the inputs, a plan that says Ok is consistent: whole MiB, in order, clear of the firmware's part and of the tail guard, with GPU addresses to match.
  uint64_t checked = 0;
  const uint32_t sizes[] = {64, 256, 2056, 4096, 8192};
  const uint64_t apertures[] = {0, 63 * MiB, 64 * MiB, 100 * MiB, 256 * MiB, 4096 * MiB, 16384 * MiB};
  const uint64_t blocks[] = {1, 4096, 3'000'000, 7 * MiB, 30 * MiB, 200 * MiB};
  const uint64_t works[] = {0, 8 * MiB, 96 * MiB, 1000 * MiB};
  for (const uint32_t mib : sizes)
    for (const uint64_t aperture : apertures)
      for (const uint64_t block_bytes : blocks)
        for (const uint64_t work : works) {
          const AmdGmc8MemoryMap m = card(mib, 0x20);
          const AmdUvd6Plan q = amd_plan_uvd6_memory(m, aperture, block_bytes, work);
          if (q.result != AmdUvd6Plan::Result::Ok) continue;
          ++checked;
          const uint64_t lowest = 25 * MiB;
          CHECK(q.block_offset % MiB == 0 && q.ring_offset % MiB == 0 && q.work_offset % MiB == 0 && q.work_bytes % MiB == 0);
          CHECK(q.block_offset >= lowest && q.block_offset + block_bytes <= q.ring_offset && q.ring_offset + MiB == q.work_offset);
          CHECK(q.work_bytes >= 8 * MiB && q.work_offset + q.work_bytes + 16 * MiB <= q.visible_bytes && q.visible_bytes <= m.video_memory_bytes && q.visible_bytes <= aperture);
          CHECK(q.block_gpu_address == m.gpu_base + q.block_offset && q.work_gpu_address == m.gpu_base + q.work_offset && q.work_bytes <= (work + MiB - 1) / MiB * MiB);
        }
  CHECK(checked > 100);
}

struct FakeMapper final : IRegisterMapper {
  std::vector<std::vector<uint32_t>> windows;
  std::vector<std::pair<uint64_t, uint64_t>> asked;
  uint64_t refuse_from = UINT64_MAX;
  FakeMapper() { windows.reserve(8); }   // the windows must not move while the test holds regions onto them
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    asked.push_back({physical, length});
    if (physical >= refuse_from) return false;
    windows.emplace_back(length / 4, 0);
    *region = MmioRegion(reinterpret_cast<uint64_t>(windows.back().data()), length);
    return true;
  }
};

static void test_map() {
  const AmdGmc8MemoryMap map = card();
  const AmdUvd6Plan p = amd_plan_uvd6_memory(map, 256 * MiB, 3'000'000);
  const uint64_t aperture = 0xD000'0000ull;
  FakeMapper mapper;
  AmdUvd6Memory memory;
  CHECK(amd_map_uvd6_plan(p, mapper, aperture, &memory) && mapper.asked.size() == 3);
  CHECK(mapper.asked[0] == std::make_pair(aperture + 140 * MiB, 3 * MiB) && mapper.asked[1] == std::make_pair(aperture + 143 * MiB, MiB) && mapper.asked[2] == std::make_pair(aperture + 144 * MiB, 96 * MiB));
  CHECK(memory.engine.block_gpu_address == p.block_gpu_address && memory.engine.ring_gpu_address == p.ring_gpu_address && memory.work_gpu_address == p.work_gpu_address);
  CHECK(memory.engine.block.length() == 3 * MiB && memory.engine.ring.length() == MiB && memory.work.length() == 96 * MiB);
  // Any of the three ranges refused (it is memory the OS owns) fails the whole thing and hands back nothing usable.
  for (const uint64_t at : {p.block_offset, p.ring_offset, p.work_offset}) {
    FakeMapper refused;
    refused.refuse_from = aperture + at;
    AmdUvd6Memory m;
    CHECK(!amd_map_uvd6_plan(p, refused, aperture, &m) && !m.engine.block.valid() && !m.engine.ring.valid() && !m.work.valid());
  }
  // A plan that is not Ok is not mapped: nothing is even asked.
  FakeMapper untouched;
  AmdUvd6Memory none;
  CHECK(!amd_map_uvd6_plan(AmdUvd6Plan(), untouched, aperture, &none) && untouched.asked.empty());
  CHECK(!amd_map_uvd6_plan(p, untouched, 0, &none) && !amd_map_uvd6_plan(p, untouched, aperture, nullptr) && untouched.asked.empty());
  CHECK(!amd_map_uvd6_plan(p, untouched, UINT64_MAX - MiB, &none) && untouched.asked.empty());   // the addresses would wrap
}

int main() {
  test_read_map();
  test_flush();
  test_plan();
  test_map();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd gmc8 memory tests passed\n");
  return 0;
}
