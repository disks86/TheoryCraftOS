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


// Host-side tests for the VCE ring (AmdVce3Ring) against a simulated ring controller: the three rings' pointer registers (banked by the instance GRBM_GFX_INDEX selects), a
// reader of the command words the driver writes (no-op, end, indirect buffer, fence, trap) that runs the indirect buffer it is pointed at and writes the fence the way the
// firmware does, and a record of every register write in order. Written from the Linux driver's description of the commands, independently of the code under test. The register
// numbers are typed in from AMD's vce_3_0_d.h, gfx_8_0_d.h and bif_5_0_d.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVce3Ring.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace reg {
constexpr uint32_t kRptr[3] = {0x8063, 0x805e, 0x80d7}, kWptr[3] = {0x8064, 0x805f, 0x80d8};
constexpr uint32_t kGrbmGfxIndex = 0xc200, kHdpFlush = 0x1520;
}  // namespace reg

constexpr uint32_t kRingDwords = 1024;

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct FakeController final : IVcnRegisters {
  std::vector<Window> windows;
  uint64_t ring_gpu[3] = {};
  std::map<uint32_t, uint32_t> regs[2][1];          // [instance]: register -> value (the pointers)
  std::vector<std::pair<uint32_t, uint32_t>> writes;  // every register write, in order
  std::vector<std::vector<uint32_t>> ibs;             // the indirect buffers run, in order
  std::vector<uint32_t> ib_vmids;
  bool stall_fence = false, dead = false;
  uint32_t protocol_errors = 0, fences_written = 0, trap_commands = 0, end_commands = 0, no_ops = 0;
  uint32_t grbm_index = 0xE0000000;
  uint32_t selected() const { return (grbm_index >> 4) & 1; }

  uint32_t read(uint32_t r) override {
    if (r == reg::kGrbmGfxIndex) return grbm_index;
    auto& m = regs[selected()][0];
    auto it = m.find(r);
    return it == m.end() ? 0 : it->second;
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r == reg::kGrbmGfxIndex) { grbm_index = v; return; }
    regs[selected()][0][r] = v;
    for (int ring = 0; ring < 3; ++ring)
      if (r == reg::kWptr[ring] && !dead) run(ring);
  }
  uint8_t* translate(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  uint32_t get32(uint64_t gpu) const {
    const uint8_t* p = translate(gpu, 4);
    return p ? p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24) : 0xDEADBEEF;
  }
  void put32(uint64_t gpu, uint32_t v) const {
    uint8_t* p = translate(gpu, 4);
    if (!p) return;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
  void set_pointers(int ring, uint32_t position) {
    for (int inst = 0; inst < 2; ++inst) { regs[inst][0][reg::kRptr[ring]] = position; regs[inst][0][reg::kWptr[ring]] = position; }
  }

 private:
  // The ring controller: reads command words from the read pointer to the write pointer of `ring` (the instance that is selected).
  void run(int ring) {
    auto& m = regs[selected()][0];
    uint32_t rptr = m[reg::kRptr[ring]];
    const uint32_t wptr = m[reg::kWptr[ring]];
    auto next = [&]() {
      const uint32_t v = get32(ring_gpu[ring] + uint64_t{rptr} * 4);
      rptr = (rptr + 1) % kRingDwords;
      m[reg::kRptr[ring]] = rptr;
      return v;
    };
    while (rptr != wptr) {
      const uint32_t cmd = next();
      switch (cmd) {
        case 0: ++no_ops; break;
        case 1: ++end_commands; break;
        case 4: ++trap_commands; break;
        case 0x102: {   // IB_VM: vmid, address low, address high, length in dwords
          const uint32_t vmid = next(), lo = next(), hi = next(), length = next();
          ib_vmids.push_back(vmid);
          std::vector<uint32_t> words;
          const uint64_t address = lo | (uint64_t{hi} << 32);
          for (uint32_t i = 0; i < length; ++i) words.push_back(get32(address + uint64_t{i} * 4));
          if (!translate(address, uint64_t{length} * 4)) ++protocol_errors;
          ibs.push_back(words);
          break;
        }
        case 3: {       // FENCE: address low, address high, sequence
          const uint32_t lo = next(), hi = next(), seq = next();
          if (!stall_fence) { put32(lo | (uint64_t{hi} << 32), seq); ++fences_written; }
          break;
        }
        default: ++protocol_errors; m[reg::kRptr[ring]] = wptr; return;
      }
    }
  }
};

static uint32_t at(FakeController& c, int instance, uint32_t r) { auto it = c.regs[instance][0].find(r); return it == c.regs[instance][0].end() ? 0 : it->second; }

struct Rig {
  FakeController ctrl;
  fakevcn::FakeClock clock;
  uint8_t* ring[3];
  uint8_t* ib;
  uint8_t* fence;
  AmdVce3Ring::Memory memory[3];
  Rig() {
    for (int r = 0; r < 3; ++r) {
      ring[r] = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
      memset(ring[r], 0xCC, 4096);
      ctrl.ring_gpu[r] = 0x6'0000'0000ull + 0x10000ull * r;
      ctrl.windows.push_back({ctrl.ring_gpu[r], ring[r], 4096});
    }
    ib = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    fence = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    memset(ib, 0xCC, 4096);
    memset(fence, 0, 4096);
    ctrl.windows.push_back({0x6'1000'0000ull, ib, 4096});
    ctrl.windows.push_back({0x6'1001'0000ull, fence, 4096});
    for (int r = 0; r < 3; ++r) {
      memory[r].ib_gpu_address = 0x6'1000'0000ull;
      memory[r].ib = MmioRegion(reinterpret_cast<uint64_t>(ib), 4096);
      memory[r].fence_gpu_address = 0x6'1001'0000ull;
      memory[r].fence = MmioRegion(reinterpret_cast<uint64_t>(fence), 4096);
      memory[r].ring = MmioRegion(reinterpret_cast<uint64_t>(ring[r]), 4096);
    }
  }
  ~Rig() { for (uint8_t* r : ring) free(r); free(ib); free(fence); }
};

static void test_commands() {
  // The values of vid.h.
  CHECK(AmdVce3Ring::kCmdNoOp == 0 && AmdVce3Ring::kCmdEnd == 1 && AmdVce3Ring::kCmdIb == 2 && AmdVce3Ring::kCmdFence == 3 && AmdVce3Ring::kCmdTrap == 4 && AmdVce3Ring::kCmdIbVm == 0x102);
  uint32_t out[16];
  const uint32_t n = AmdVce3Ring::build_job_commands(0x6'1000'0040ull, 32, 0x6'1001'0000ull, 9, ArrayView<uint32_t>(out));
  const uint32_t expect[] = {0x102, 0, 0x10000040, 6, 32, 3, 0x10010000, 6, 9, 4, 1};
  CHECK(n == 11);
  for (uint32_t i = 0; i < 11 && i < n; ++i) CHECK(out[i] == expect[i]);
  uint32_t small[10];
  CHECK(AmdVce3Ring::build_job_commands(0, 16, 0, 1, ArrayView<uint32_t>(small)) == 0);
}

static void test_ring_test() {
  Rig rig;
  rig.ctrl.set_pointers(0, 0);
  AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[0], 0, AmdVce3::kHarvestVce1);
  CHECK(ring.test());
  // END then padding to 16 dwords: the controller read all of them (the pointers meet at 16).
  CHECK(at(rig.ctrl, 0, reg::kWptr[0]) == 16 && at(rig.ctrl, 0, reg::kRptr[0]) == 16 && rig.ctrl.end_commands == 1 && rig.ctrl.no_ops == 15 && rig.ctrl.protocol_errors == 0);
  // The flush of the host data path comes before the write pointer is published.
  size_t flush = 0, wptr = 0;
  for (size_t i = 0; i < rig.ctrl.writes.size(); ++i) {
    if (rig.ctrl.writes[i].first == reg::kHdpFlush) flush = i + 1;
    if (rig.ctrl.writes[i].first == reg::kWptr[0]) wptr = i + 1;
  }
  CHECK(flush != 0 && wptr != 0 && flush < wptr);
  CHECK(ring.test() && at(rig.ctrl, 0, reg::kWptr[0]) == 32);
  // An engine that does not read its ring: the test fails (and the ring is not wedged: the test is not a job).
  Rig dead;
  dead.ctrl.dead = true;
  AmdVce3Ring dead_ring(dead.ctrl, dead.clock, dead.memory[0], 0, AmdVce3::kHarvestVce1);
  CHECK(!dead_ring.test(2000) && !dead_ring.wedged());
}

static void test_jobs() {
  Rig rig;
  rig.ctrl.set_pointers(0, 0);
  AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[0], 0, AmdVce3::kHarvestVce1);
  const uint32_t task[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
  CHECK(ring.submit(ArrayView<const uint32_t>(task), 20) == AmdVce3Ring::Result::Ok && ring.jobs_completed() == 1);
  CHECK(rig.ctrl.ibs.size() == 1 && rig.ctrl.ib_vmids[0] == 0 && rig.ctrl.fences_written == 1 && rig.ctrl.trap_commands == 1 && rig.ctrl.protocol_errors == 0);
  // The indirect buffer the engine ran: the task, then no-ops up to a multiple of 16 dwords.
  CHECK(rig.ctrl.ibs[0].size() == 16);
  for (uint32_t i = 0; i < 16; ++i) CHECK(rig.ctrl.ibs[0][i] == (i < 5 ? task[i] : 0u));
  // A job is 11 command dwords plus padding to 16: the pointers moved by 16 and the controller read them all.
  CHECK(at(rig.ctrl, 0, reg::kWptr[0]) == 16 && at(rig.ctrl, 0, reg::kRptr[0]) == 16 && ring.write_pointer() == 16 && ring.read_pointer() == 16);
  // Many jobs: the ring wraps (1024 dwords, 16 a job), the sequence numbers go up, nothing breaks.
  for (uint32_t j = 2; j <= 200; ++j) {
    uint32_t t[3] = {j, j + 1, j + 2};
    CHECK(ring.submit(ArrayView<const uint32_t>(t), 20) == AmdVce3Ring::Result::Ok && rig.ctrl.ibs.back()[0] == j);
  }
  CHECK(ring.jobs_completed() == 200 && rig.ctrl.protocol_errors == 0 && ring.write_pointer() == (200u * 16) % kRingDwords);
  CHECK(rig.ctrl.get32(0x6'1001'0000ull) == 200);
  // An indirect buffer of exactly 16 dwords gets no padding; the largest one fits.
  uint32_t sixteen[16];
  for (uint32_t i = 0; i < 16; ++i) sixteen[i] = 0xA0 + i;
  CHECK(ring.submit(ArrayView<const uint32_t>(sixteen), 20) == AmdVce3Ring::Result::Ok && rig.ctrl.ibs.back().size() == 16);
  std::vector<uint32_t> big(AmdVce3Ring::kMaxIndirectBufferDwords, 7);
  CHECK(ring.submit(ArrayView<const uint32_t>(big.data(), big.size()), 20) == AmdVce3Ring::Result::Ok && rig.ctrl.ibs.back().size() == big.size());
}

static void test_rings_and_instances() {
  for (int index = 0; index < 3; ++index) {  // each ring has registers of its own
    Rig rig;
    rig.ctrl.set_pointers(index, 0);
    AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[index], index, AmdVce3::kHarvestVce1);
    const uint32_t task[1] = {5};
    CHECK(ring.submit(ArrayView<const uint32_t>(task), 20) == AmdVce3Ring::Result::Ok);
    for (const auto& w : rig.ctrl.writes)
      for (int other = 0; other < 3; ++other)
        if (other != index) CHECK(w.first != reg::kWptr[other]);
    CHECK(at(rig.ctrl, 0, reg::kWptr[index]) == 16);
  }
  {  // The ring's pointers are those of the instance that is there: the first, or the second when only that one is.
    Rig rig;
    rig.ctrl.set_pointers(0, 0);
    AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[0], 0, AmdVce3::kHarvestVce0);
    const uint32_t task[1] = {5};
    CHECK(ring.submit(ArrayView<const uint32_t>(task), 20) == AmdVce3Ring::Result::Ok);
    CHECK(at(rig.ctrl, 1, reg::kWptr[0]) == 16 && at(rig.ctrl, 0, reg::kWptr[0]) == 0);
    CHECK(rig.ctrl.grbm_index == 0xE0000000);   // the default is selected again after each access
  }
  {  // A ring that is picked up where it was left: the start's pointers are where it begins.
    Rig rig;
    rig.ctrl.set_pointers(0, 37);
    AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[0], 0, AmdVce3::kHarvestVce1);
    CHECK(ring.write_pointer() == 37);
    const uint32_t task[1] = {5};
    CHECK(ring.submit(ArrayView<const uint32_t>(task), 20) == AmdVce3Ring::Result::Ok);
    CHECK(ring.write_pointer() % 16 == 0 && rig.ctrl.protocol_errors == 0);
  }
}

static void test_failures() {
  {  // The fence never comes: TimedOut, and the ring refuses everything afterwards without touching the engine again.
    Rig rig;
    rig.ctrl.set_pointers(0, 0);
    rig.ctrl.stall_fence = true;
    AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[0], 0, AmdVce3::kHarvestVce1);
    const uint32_t task[1] = {5};
    CHECK(ring.submit(ArrayView<const uint32_t>(task), 10) == AmdVce3Ring::Result::TimedOut && ring.wedged());
    const size_t writes = rig.ctrl.writes.size();
    CHECK(ring.submit(ArrayView<const uint32_t>(task), 10) == AmdVce3Ring::Result::Wedged && !ring.test() && rig.ctrl.writes.size() == writes);
  }
  {  // Arguments: no task, too long a task, windows that are too small. Nothing is written.
    Rig rig;
    rig.ctrl.set_pointers(0, 0);
    AmdVce3Ring ring(rig.ctrl, rig.clock, rig.memory[0], 0, AmdVce3::kHarvestVce1);
    const size_t before = rig.ctrl.writes.size();
    std::vector<uint32_t> too_long(AmdVce3Ring::kMaxIndirectBufferDwords + 1, 1);
    CHECK(ring.submit(ArrayView<const uint32_t>(), 10) == AmdVce3Ring::Result::BadArgument);
    CHECK(ring.submit(ArrayView<const uint32_t>(too_long.data(), too_long.size()), 10) == AmdVce3Ring::Result::BadArgument);
    AmdVce3Ring::Memory small = rig.memory[0];
    small.ib = MmioRegion(reinterpret_cast<uint64_t>(rig.ib), 1024);
    AmdVce3Ring small_ib(rig.ctrl, rig.clock, small, 0, AmdVce3::kHarvestVce1);
    const uint32_t task[1] = {5};
    CHECK(small_ib.submit(ArrayView<const uint32_t>(task), 10) == AmdVce3Ring::Result::BadArgument);
    small = rig.memory[0];
    small.fence = MmioRegion(reinterpret_cast<uint64_t>(rig.fence), 32);
    AmdVce3Ring small_fence(rig.ctrl, rig.clock, small, 0, AmdVce3::kHarvestVce1);
    CHECK(small_fence.submit(ArrayView<const uint32_t>(task), 10) == AmdVce3Ring::Result::BadArgument);
    small = rig.memory[0];
    small.ring = MmioRegion(reinterpret_cast<uint64_t>(rig.ring[0]), 2048);
    AmdVce3Ring small_ring(rig.ctrl, rig.clock, small, 0, AmdVce3::kHarvestVce1);
    CHECK(small_ring.submit(ArrayView<const uint32_t>(task), 10) == AmdVce3Ring::Result::BadArgument && !small_ring.test());
    // (only the constructors' pointer reads, which write the instance select, touched the registers)
    for (size_t i = before; i < rig.ctrl.writes.size(); ++i) CHECK(rig.ctrl.writes[i].first == reg::kGrbmGfxIndex);
  }
}

int main() {
  test_commands();
  test_ring_test();
  test_jobs();
  test_rings_and_instances();
  test_failures();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd vce3 ring tests passed\n");
  return 0;
}
