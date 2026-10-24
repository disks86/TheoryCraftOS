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


// Host-side tests for the VCN 1.0 decode ring submission (AmdVcn1Ring) against a simulated ring buffer controller (FakeAmdVcn.hpp): the packets on the ring and in the
// indirect buffer, the order of the commands, the fence, wrap-around of the ring, a job that never finishes, and bad arguments.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "FakeAmdVcn.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1Ring.hpp"

using namespace hardware;
using namespace fakevcn;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// Plain memory standing in for video memory: a ring page, an indirect buffer, a fence word, each with a made-up GPU address.
struct Setup {
  std::vector<uint8_t> ring, ib, fence;
  Vcn vcn;
  FakeClock clock;
  AmdVcn1Ring::Memory memory;
  static constexpr uint64_t kRingGpu = 0x1'0000'0000ull, kIbGpu = 0x1'0010'0000ull, kFenceGpu = 0x1'0020'0000ull;
  Setup() : ring(AmdVcn1::kRingBytes, 0), ib(AmdVcn1Ring::kIndirectBufferBytes, 0), fence(AmdVcn1Ring::kFenceBytes, 0xFF) {
    vcn.windows = {{kRingGpu, ring.data(), ring.size()}, {kIbGpu, ib.data(), ib.size()}, {kFenceGpu, fence.data(), fence.size()}};
    vcn.ring_gpu = kRingGpu;
    memory.ib_gpu_address = kIbGpu;
    memory.ib = MmioRegion(reinterpret_cast<uint64_t>(ib.data()), ib.size());
    memory.fence_gpu_address = kFenceGpu;
    memory.fence = MmioRegion(reinterpret_cast<uint64_t>(fence.data()), fence.size());
    memory.ring = MmioRegion(reinterpret_cast<uint64_t>(ring.data()), ring.size());
  }
};

static void test_one_job() {
  Setup s;
  AmdVcn1Ring ring(s.vcn, s.clock, s.memory);
  const AmdVcn1Ring::Buffer buffers[] = {{0x5, 0x1'2345'6000ull}, {0x0, 0x1'0000'1000ull}, {0x2, 0x2'ABCD'E000ull}};
  std::vector<Command> seen;
  s.vcn.on_command = [&](uint32_t c, uint64_t a) { seen.push_back({c, a}); };
  int engine = 0;
  s.vcn.on_engine = [&] { ++engine; };
  CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(buffers), true, 100) == AmdVcn1Ring::Result::Ok);
  CHECK(ring.jobs_completed() == 1 && !ring.wedged());
  // The firmware saw the three buffers in order with the right addresses, and the engine was started once after them.
  CHECK(seen.size() == 3 && seen[0].command == 0x5 && seen[0].address == 0x1'2345'6000ull && seen[1].command == 0 + 0 && seen[1].address == 0x1'0000'1000ull);
  CHECK(seen[2].command == 0x2 && seen[2].address == 0x2'ABCD'E000ull && engine == 1);
  // The whole command stream, including the ring's own: start, fence (command 0 at the fence address), trap, end.
  CHECK(s.vcn.commands.size() == 3 + 4 && s.vcn.jobs_started == 1 && s.vcn.protocol_errors == 0);
  CHECK(s.vcn.commands.front().command == 0xa && s.vcn.commands.back().command == 0xb);
  // The fence: the job's number written to the fence word, at the address the ring gave (40 bits).
  CHECK(s.vcn.get32(Setup::kFenceGpu) == 1);
  // Second job: the next number.
  CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(buffers, 1), false, 100) == AmdVcn1Ring::Result::Ok && s.vcn.get32(Setup::kFenceGpu) == 2 && engine == 1);
}

static void test_indirect_buffer_words() {
  const AmdVcn1Ring::Buffer buffers[] = {{0x206, 0x0000'0001'2345'6789ull}};
  uint32_t words[64];
  const uint32_t n = AmdVcn1Ring::build_indirect_buffer(ArrayView<const AmdVcn1Ring::Buffer>(buffers), true, ArrayView<uint32_t>(words));
  // 4 register writes (data0, data1, cmd, engine start) = 8 dwords, padded to a multiple of 16 with no-op writes.
  CHECK(n == 16);
  CHECK(words[0] == kRegGpcomData0 && words[1] == 0x23456789 && words[2] == kRegGpcomData1 && words[3] == 1 && words[4] == kRegGpcomCmd && words[5] == (0x206u << 1));
  CHECK(words[6] == kRegEngineCntl && words[7] == 1);
  for (uint32_t i = 8; i < 16; i += 2) CHECK(words[i] == kRegNoOp && words[i + 1] == 0);
  // The values the real registers have (Mesa's byte addresses RDECODE_VCN1_*, divided by four).
  CHECK(kRegGpcomCmd * 4 == 0x2070c && kRegGpcomData0 * 4 == 0x20710 && kRegGpcomData1 * 4 == 0x20714 && kRegEngineCntl * 4 == 0x20718);
  // An output array that is too small gives nothing, never a write past it.
  uint32_t small[15];
  CHECK(AmdVcn1Ring::build_indirect_buffer(ArrayView<const AmdVcn1Ring::Buffer>(buffers), true, ArrayView<uint32_t>(small)) == 0);
  // Too many buffers.
  AmdVcn1Ring::Buffer many[AmdVcn1Ring::kMaxBuffersPerJob + 1] = {};
  CHECK(AmdVcn1Ring::build_indirect_buffer(ArrayView<const AmdVcn1Ring::Buffer>(many), false, ArrayView<uint32_t>(words)) == 0);
  CHECK(AmdVcn1Ring::build_indirect_buffer(ArrayView<const AmdVcn1Ring::Buffer>(many, AmdVcn1Ring::kMaxBuffersPerJob), false, ArrayView<uint32_t>(words)) == 0);   // 12 x 6 dwords does not fit 64
  uint32_t big[256];
  CHECK(AmdVcn1Ring::build_indirect_buffer(ArrayView<const AmdVcn1Ring::Buffer>(many, AmdVcn1Ring::kMaxBuffersPerJob), false, ArrayView<uint32_t>(big)) == 80);
}

static void test_ring_wraps() {
  Setup s;
  s.vcn.set_pointers(1000);   // close to the end of the 1024 dword ring, and not aligned
  AmdVcn1Ring ring(s.vcn, s.clock, s.memory);
  const AmdVcn1Ring::Buffer b[] = {{0x0, 0x1'0000'4000ull}};
  for (int job = 1; job <= 100; ++job) {
    CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(b), false, 100) == AmdVcn1Ring::Result::Ok);
    CHECK(s.vcn.get32(Setup::kFenceGpu) == uint32_t(job));
  }
  CHECK(s.vcn.protocol_errors == 0 && s.vcn.jobs_started == 100);
  // The write pointer stays aligned to 16 dwords after each job (the ring's align mask), whatever it started at... from the first job on.
  CHECK(s.vcn.read(kRegRptr) % 16 == 0);
}

static void test_timeout_wedges() {
  Setup s;
  s.vcn.stall_fence = true;
  AmdVcn1Ring ring(s.vcn, s.clock, s.memory);
  const AmdVcn1Ring::Buffer b[] = {{0x0, 0x1'0000'4000ull}};
  CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(b), false, 5) == AmdVcn1Ring::Result::TimedOut);
  CHECK(ring.wedged());
  s.vcn.stall_fence = false;
  const uint32_t writes = uint32_t(s.vcn.writes.size());
  CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(b), false, 5) == AmdVcn1Ring::Result::Wedged);
  CHECK(s.vcn.writes.size() == writes);   // a wedged ring writes nothing
}

static void test_bad_arguments() {
  Setup s;
  AmdVcn1Ring ring(s.vcn, s.clock, s.memory);
  const AmdVcn1Ring::Buffer b[] = {{0x0, 0x1'0000'4000ull}};
  CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(), false, 5) == AmdVcn1Ring::Result::BadArgument);
  AmdVcn1Ring::Buffer many[AmdVcn1Ring::kMaxBuffersPerJob + 1] = {};
  CHECK(ring.submit(ArrayView<const AmdVcn1Ring::Buffer>(many), false, 5) == AmdVcn1Ring::Result::BadArgument);
  CHECK(s.vcn.writes.empty() && !ring.wedged());
  // A window that is too small for what the ring uses.
  AmdVcn1Ring::Memory small = s.memory;
  small.ib = MmioRegion(s.memory.ib.address(), 1024);
  AmdVcn1Ring r2(s.vcn, s.clock, small);
  CHECK(r2.submit(ArrayView<const AmdVcn1Ring::Buffer>(b), false, 5) == AmdVcn1Ring::Result::BadArgument);
  small = s.memory;
  small.ring = MmioRegion();
  AmdVcn1Ring r3(s.vcn, s.clock, small);
  CHECK(r3.submit(ArrayView<const AmdVcn1Ring::Buffer>(b), false, 5) == AmdVcn1Ring::Result::BadArgument);
  CHECK(s.vcn.writes.empty());
}

int main() {
  test_one_job();
  test_indirect_buffer_words();
  test_ring_wraps();
  test_timeout_wedges();
  test_bad_arguments();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdVcn1RingTest passed\n");
  return 0;
}
