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

// Host-side tests for the UVD 6 decode ring (AmdUvd6Ring) against a simulated ring buffer controller: it reads register-write packets from the ring and from the
// indirect buffer the ring points at, the way the hardware does, and runs the firmware's side of the fence. It is written from the Linux driver's description of the
// packets, independently of the code under test. The register numbers are typed in from AMD's uvd_6_0_d.h and bif_5_0_d.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace reg {
constexpr uint32_t kIbVmid = 0x3da1, kIbBarLow = 0x3c67, kIbBarHigh = 0x3c66, kIbSize = 0x3da2, kContextId = 0x3dbd;
constexpr uint32_t kData0 = 0x3bc4, kData1 = 0x3bc5, kCmd = 0x3bc3, kEngineCntl = 0x3bc6, kNoOp = 0x3bff, kWptr = 0x3da5, kRptr = 0x3da4, kHdpFlush = 0x1520;
}  // namespace reg

struct Window {
  uint64_t gpu;
  uint8_t* host;
  uint64_t size;
};

struct Command {
  uint32_t command;
  uint64_t address;
  bool in_ib;
};

class FakeRing final : public IVcnRegisters {
 public:
  std::vector<Window> windows;
  uint64_t ring_gpu = 0;
  uint32_t ring_dwords = 1024;
  std::vector<Command> commands;                          // every GPCOM command, in order
  std::vector<std::pair<uint32_t, uint32_t>> writes;      // every register write the host made
  std::vector<std::pair<uint32_t, uint32_t>> executed;    // every register write the engine made from packets
  bool stall_fence = false, dead = false;
  uint32_t engine_starts = 0, protocol_errors = 0, ib_runs = 0;
  uint32_t flushes_before_wptr = 0;

  uint32_t read(uint32_t r) override { return r == reg::kRptr ? rptr_ : regs_[r]; }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r == reg::kHdpFlush) flushed_ = true;
    if (r == reg::kWptr) {
      if (flushed_) ++flushes_before_wptr;
      flushed_ = false;
      wptr_ = v;
      if (!dead) run_ring();
      return;
    }
    regs_[r] = v;
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
  void set_pointers(uint32_t position) { rptr_ = wptr_ = position; }

 private:
  void run_ring() {
    while (rptr_ != wptr_) {
      const uint32_t packet = get32(ring_gpu + uint64_t{rptr_} * 4);
      rptr_ = (rptr_ + 1) % ring_dwords;
      if (!execute(packet, true, 0)) { rptr_ = wptr_; return; }
    }
  }
  bool execute(uint32_t packet, bool from_ring, uint64_t ib_at) {
    if ((packet >> 30) != 0) { ++protocol_errors; return false; }
    const uint32_t r = packet & 0xFFFF, count = (packet >> 16) & 0x3FFF;
    for (uint32_t i = 0; i <= count; ++i) {
      uint32_t value;
      if (from_ring) {
        value = get32(ring_gpu + uint64_t{rptr_} * 4);
        rptr_ = (rptr_ + 1) % ring_dwords;
      } else {
        value = get32(ib_at + uint64_t{i} * 4);
      }
      register_write(r + i, value, !from_ring);
    }
    return true;
  }
  void register_write(uint32_t r, uint32_t value, bool in_ib) {
    executed.push_back({r, value});
    regs_[r] = value;
    if (r == reg::kCmd) {
      const uint32_t command = value >> 1;
      const uint64_t address = regs_[reg::kData0] | (uint64_t{regs_[reg::kData1]} << 32);
      commands.push_back({command, address, in_ib});
      if (!in_ib && command == 0 && !stall_fence) put32(address, regs_[reg::kContextId]);
    } else if (r == reg::kEngineCntl && value == 1) {
      ++engine_starts;
    } else if (r == reg::kIbSize) {
      // Fetch and run the indirect buffer the previous registers named: packets of two dwords (register, value).
      const uint64_t ib = regs_[reg::kIbBarLow] | (uint64_t{regs_[reg::kIbBarHigh]} << 32);
      ++ib_runs;
      uint32_t at = 0;
      while (at < value) {
        const uint32_t p = get32(ib + uint64_t{at} * 4);
        at += 1;
        if (!execute_ib_packet(p, ib + uint64_t{at} * 4)) return;
        at += 1;
      }
    }
  }
  bool execute_ib_packet(uint32_t packet, uint64_t values_at) {
    if ((packet >> 30) != 0 || ((packet >> 16) & 0x3FFF) != 0) { ++protocol_errors; return false; }
    register_write(packet & 0xFFFF, get32(values_at), true);
    return true;
  }
  uint32_t rptr_ = 0, wptr_ = 0;
  bool flushed_ = false;
  std::map<uint32_t, uint32_t> regs_;
};

struct Rig {
  fakevcn::FakeClock clock;
  FakeRing hw;
  uint8_t* ring = nullptr;
  uint8_t* ib = nullptr;
  uint8_t* fence = nullptr;
  AmdUvd6Ring::Memory memory;
  Rig() {
    ring = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    ib = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    fence = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    memset(ring, 0, 4096); memset(ib, 0, 4096); memset(fence, 0xEE, 4096);
    hw.ring_gpu = 0x1'0100'0000ull;
    hw.windows = {{hw.ring_gpu, ring, 4096}, {0x1'0200'0000ull, ib, 4096}, {0x1'0300'0000ull, fence, 4096}};
    memory.ib_gpu_address = 0x1'0200'0000ull;
    memory.ib = MmioRegion(reinterpret_cast<uint64_t>(ib), 4096);
    memory.fence_gpu_address = 0x1'0300'0000ull;
    memory.fence = MmioRegion(reinterpret_cast<uint64_t>(fence), 64);
    memory.ring = MmioRegion(reinterpret_cast<uint64_t>(ring), 4096);
  }
  ~Rig() { free(ring); free(ib); free(fence); }
};

static void test_indirect_buffer() {
  const AmdUvd6Ring::Buffer buffers[3] = {{0x000, 0x1'2345'6000ull}, {0x100, 0x2'0000'1000ull}, {0x002, 0x0000'0000'8000ull}};
  uint32_t out[64] = {};
  uint32_t n = AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(buffers), true, ArrayView<uint32_t>(out));
  // Each buffer: DATA0 low, DATA1 high, CMD (command << 1); then the engine start; then no-ops to a multiple of sixteen dwords.
  const uint32_t expected[] = {reg::kData0, 0x23456000, reg::kData1, 1, reg::kCmd, 0x000, reg::kData0, 0x00001000, reg::kData1, 2, reg::kCmd, 0x200,
                               reg::kData0, 0x8000, reg::kData1, 0, reg::kCmd, 0x4, reg::kEngineCntl, 1};
  // (packet words are the register numbers: type 0, count 0)
  CHECK(n == 32);
  for (uint32_t i = 0; i < 20; ++i) CHECK(out[i] == expected[i]);
  for (uint32_t i = 20; i < 32; i += 2) CHECK(out[i] == reg::kNoOp && out[i + 1] == 0);
  // Without the engine start; and the sizes it refuses.
  n = AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(buffers, 1), false, ArrayView<uint32_t>(out));
  CHECK(n == 16 && out[6] == reg::kNoOp);
  CHECK(AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(buffers), true, ArrayView<uint32_t>(out, 20)) == 0);   // does not fit
  CHECK(AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(buffers), true, ArrayView<uint32_t>(out, 32)) == 32);   // fits exactly
  AmdUvd6Ring::Buffer many[13] = {};
  uint32_t big[256];
  CHECK(AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(many), true, ArrayView<uint32_t>(big)) == 0);   // 13 buffers: too many
  CHECK(AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(many, 12), true, ArrayView<uint32_t>(big)) != 0);
}

static void test_submit() {
  Rig rig;
  AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
  const AmdUvd6Ring::Buffer buffers[2] = {{0x000, 0x1'0400'0000ull}, {0x100, 0x1'0500'0000ull}};
  CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(buffers), true, 100) == AmdUvd6Ring::Result::Ok && ring.jobs_completed() == 1);
  CHECK(rig.hw.protocol_errors == 0 && rig.hw.ib_runs == 1 && rig.hw.engine_starts == 1);
  // The ring got: IB (VMID 0, address, length), fence (context id = 1, address, command 0), trap (command 1 << 1), then padding.
  const uint32_t count = rig.memory.ib.read32(0) == reg::kData0 ? 16 : 0;   // (the IB for two buffers and a start is 16 dwords)
  CHECK(count == 16);
  std::vector<std::pair<uint32_t, uint32_t>> ring_packets;
  for (uint32_t i = 0; i < 22; i += 2) ring_packets.push_back({rig.hw.get32(rig.hw.ring_gpu + i * 4), rig.hw.get32(rig.hw.ring_gpu + (i + 1) * 4)});
  const std::vector<std::pair<uint32_t, uint32_t>> expected = {
      {reg::kIbVmid, 0}, {reg::kIbBarLow, 0x02000000}, {reg::kIbBarHigh, 1}, {reg::kIbSize, 16},
      {reg::kContextId, 1}, {reg::kData0, 0x03000000}, {reg::kData1, 1}, {reg::kCmd, 0}, {reg::kData0, 0}, {reg::kData1, 0}, {reg::kCmd, 2}};
  CHECK(ring_packets == expected);
  for (uint32_t i = 22; i < 32; i += 2) CHECK(rig.hw.get32(rig.hw.ring_gpu + i * 4) == reg::kNoOp);
  // The write pointer was published after the host data path was flushed, and is the aligned position.
  CHECK(rig.hw.flushes_before_wptr == 1 && rig.hw.writes.back() == std::make_pair(reg::kWptr, 32u));
  // The firmware saw the buffers in the order named, from the indirect buffer, then the fence and the trap from the ring.
  std::vector<Command> in_ib, from_ring;
  for (const Command& c : rig.hw.commands) (c.in_ib ? in_ib : from_ring).push_back(c);
  CHECK(in_ib.size() == 2 && in_ib[0].command == 0 && in_ib[0].address == 0x1'0400'0000ull && in_ib[1].command == 0x100 && in_ib[1].address == 0x1'0500'0000ull);
  CHECK(from_ring.size() == 2 && from_ring[0].command == 0 && from_ring[0].address == 0x1'0300'0000ull && from_ring[1].command == 1);
  CHECK(rig.hw.get32(rig.memory.fence_gpu_address) == 1);
  // The indirect buffer in memory is what build_indirect_buffer says.
  uint32_t built[64];
  const uint32_t n = AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(buffers), true, ArrayView<uint32_t>(built));
  bool same = n == 16;
  for (uint32_t i = 0; i < n; ++i) same = same && rig.hw.get32(rig.memory.ib_gpu_address + i * 4) == built[i];
  CHECK(same);
  // A second job: the next sequence number, and a picture-less message does not start the engine.
  CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(buffers, 1), false, 100) == AmdUvd6Ring::Result::Ok && ring.jobs_completed() == 2);
  CHECK(rig.hw.engine_starts == 1 && rig.hw.get32(rig.memory.fence_gpu_address) == 2);
}

static void test_wrap() {
  Rig rig;
  AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
  const AmdUvd6Ring::Buffer b = {0x000, 0x1'0400'0000ull};
  for (int i = 0; i < 200; ++i) CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(&b, 1), i % 2 == 0, 100) == AmdUvd6Ring::Result::Ok);
  CHECK(ring.jobs_completed() == 200 && rig.hw.protocol_errors == 0 && rig.hw.ib_runs == 200 && rig.hw.engine_starts == 100 && rig.hw.get32(rig.memory.fence_gpu_address) == 200);
  CHECK(rig.hw.flushes_before_wptr == 200);
}

static void test_start_position() {
  Rig rig;
  rig.hw.set_pointers(38);   // the start sequence left the pointers wherever they were
  AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
  const AmdUvd6Ring::Buffer b = {0x000, 0x1'0400'0000ull};
  CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(&b, 1), false, 100) == AmdUvd6Ring::Result::Ok);
  CHECK(rig.hw.get32(rig.hw.ring_gpu + 38 * 4) == reg::kIbVmid && rig.hw.writes.back() == std::make_pair(reg::kWptr, 64u));
}

static void test_failures() {
  {  // The engine never writes the fence: TimedOut, wedged, and nothing more is accepted.
    Rig rig;
    rig.hw.stall_fence = true;
    AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
    const AmdUvd6Ring::Buffer b = {0x000, 0x1'0400'0000ull};
    CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(&b, 1), true, 5) == AmdUvd6Ring::Result::TimedOut && ring.wedged() && ring.jobs_completed() == 0);
    const size_t writes = rig.hw.writes.size();
    CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(&b, 1), true, 5) == AmdUvd6Ring::Result::Wedged && rig.hw.writes.size() == writes && !ring.test());
  }
  {  // Bad arguments touch nothing.
    Rig rig;
    AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
    const AmdUvd6Ring::Buffer b = {0x000, 0x1'0400'0000ull};
    CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(), true, 5) == AmdUvd6Ring::Result::BadArgument);
    AmdUvd6Ring::Buffer many[13] = {};
    CHECK(ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(many), true, 5) == AmdUvd6Ring::Result::BadArgument);
    for (int which = 0; which < 3; ++which) {
      AmdUvd6Ring::Memory m = rig.memory;
      if (which == 0) m.ib = MmioRegion(m.ib.address(), 2048);
      if (which == 1) m.fence = MmioRegion(m.fence.address(), 32);
      if (which == 2) m.ring = MmioRegion();
      AmdUvd6Ring bad(rig.hw, rig.clock, m);
      CHECK(bad.submit(ArrayView<const AmdUvd6Ring::Buffer>(&b, 1), true, 5) == AmdUvd6Ring::Result::BadArgument);
    }
    CHECK(rig.hw.writes.empty() && !ring.wedged() && rig.memory.ib.read32(0) == 0);
  }
}

static void test_ring_test() {
  {
    Rig rig;
    AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
    CHECK(ring.test() && rig.hw.protocol_errors == 0);
    // The context register was primed with 0xCAFEDEAD, then written through the ring.
    CHECK(rig.hw.writes.front() == std::make_pair(reg::kContextId, 0xCAFEDEADu) && rig.hw.get32(rig.hw.ring_gpu) == reg::kContextId && rig.hw.get32(rig.hw.ring_gpu + 4) == 0xDEADBEEFu);
    CHECK(rig.hw.read(reg::kContextId) == 0xDEADBEEFu && rig.hw.flushes_before_wptr == 1);
    // It works again, and a job still works after it.
    const AmdUvd6Ring::Buffer b = {0x000, 0x1'0400'0000ull};
    CHECK(ring.test() && ring.submit(ArrayView<const AmdUvd6Ring::Buffer>(&b, 1), false, 100) == AmdUvd6Ring::Result::Ok);
  }
  {  // An engine that is not running does not take the packet.
    Rig rig;
    rig.hw.dead = true;
    AmdUvd6Ring ring(rig.hw, rig.clock, rig.memory);
    CHECK(!ring.test(1000));
  }
}

int main() {
  test_indirect_buffer();
  test_submit();
  test_wrap();
  test_start_position();
  test_failures();
  test_ring_test();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd uvd6 ring tests passed\n");
  return 0;
}
