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


// Host-side tests for the Vega M SDMA engines (AmdSdma3) against simulated engines: the register writes of the start in the driver's order with values worked out by hand,
// the rings' contents (the ring test packet, the padding with and without the burst NOP), copies and fills through both engines, chunking of big copies, ring wrap-around, an
// engine that stalls and is wedged, the stop, and every refused argument.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.hpp"
#include "FakeAmdSdma3.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Block {
  std::vector<uint8_t> storage;
  uint8_t* base = nullptr;
  explicit Block(size_t bytes) : storage(bytes + 4096) { base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 4095) & ~uintptr_t{4095}); }
};

constexpr uint64_t kSlot = AmdSdma3::kSlotBytes;   // 12288
constexpr uint32_t kBig = 5u << 20;

struct Rig {
  fakesdma::Card card;
  fakesdma::FakeClock clock;
  Block block, doorbells, data;
  uint64_t gpu = 0x1'4000'0000ull, data_gpu = 0x1'8000'0000ull, dst_gpu = 0x2'8000'0000ull;   // (the destination is in another 4 GiB of the address space)
  Rig() : block(2 * kSlot), doorbells(0x400 * 4), data(2 * kBig) {
    card.windows.push_back({gpu, block.base, 2 * kSlot});
    card.windows.push_back({data_gpu, data.base, kBig});
    card.windows.push_back({dst_gpu, data.base + kBig, kBig});
    card.doorbell_host = doorbells.base;
    card.doorbell_bytes = 0x400 * 4;
    clock.on_tick = [this] { card.poll(); };
  }
  AmdSdma3::Memory memory() {
    AmdSdma3::Memory m;
    m.gpu_address = gpu;
    m.block = MmioRegion(reinterpret_cast<uint64_t>(block.base), 2 * kSlot);
    m.doorbells = MmioRegion(reinterpret_cast<uint64_t>(doorbells.base), 0x400 * 4);
    return m;
  }
  uint32_t ring_word(uint32_t instance, uint32_t index) { return card.get32(gpu + instance * kSlot + index * 4); }
  uint32_t doorbell(uint32_t index) { const uint8_t* p = doorbells.base + index * 4; return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }
};

static uint32_t initial_value(uint32_t reg) { return 0x9E3779B1u * (reg + 1); }

static void test_start() {
  Rig rig;
  const uint32_t golden_regs[10] = {0x3405, 0x3403, 0x348a, 0x350a, 0x358a, 0x3605, 0x3603, 0x368a, 0x370a, 0x378a};
  const uint32_t ands[5] = {0xfc910007, 0xff000fff, 0x800f0111, 0x800f0111, 0x800f0111}, ors[5] = {0x00810007, 0, 0x100, 0x100, 0x100};
  for (uint32_t r : golden_regs) rig.card.set_reg(r, initial_value(r));
  AmdSdma3::Config config;
  config.burst_nop[0] = true;
  AmdSdma3 sdma(rig.card, rig.clock, config);
  CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok && sdma.running() && sdma.failed_instance() == -1);
  const auto& w = rig.card.writes;
  std::vector<std::pair<uint32_t, uint32_t>> expect;
  auto add = [&](uint32_t r, uint32_t v) { expect.push_back({r, v}); };
  // golden_settings_polaris11_a11
  for (int i = 0; i < 10; ++i) add(golden_regs[i], (initial_value(golden_regs[i]) & ~ands[i % 5]) | ors[i % 5]);
  // sdma_v3_0_start: context switching off (ATC_L1_ENABLE stays on), then the engines off: ring and IB disabled, F32 halted
  add(0x3404, 0x2); add(0x3604, 0x2);
  add(0x3480, 0); add(0x348a, 0x0 | (golden_regs[2] == 0x348a ? ((initial_value(0x348a) & ~0x800f0111u) | 0x100) & ~1u : 0));
  add(0x3680, 0); add(0x368a, ((initial_value(0x368a) & ~0x800f0111u) | 0x100) & ~1u);
  add(0x3412, 1); add(0x3612, 1);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t off = 0x200 * i;
    for (uint32_t j = 0; j < 16; ++j) { add(0x391, j << 4); add(0x34a7 + off, 0); add(0x34a8 + off, 0); }
    add(0x391, 0);
    add(0x3406 + off, 0);                                   // SDMA0_TILING_CONFIG: gb_addr_config & 0x70
    add(0x3409 + off, 0);                                   // SEM_WAIT_FAIL_TIMER_CNTL
    add(0x3480 + off, 0x16);                                // RB_SIZE 11 (2048 dwords)
    add(0x3483 + off, 0);                                   // RB_RPTR
    add(0x348b + off, 0);                                   // IB_RPTR
    add(0x348c + off, 0);                                   // IB_OFFSET
    const uint64_t ring = rig.gpu + i * kSlot, wb = ring + 8192;
    add(0x3488 + off, static_cast<uint32_t>(wb >> 32));     // RB_RPTR_ADDR_HI
    add(0x3489 + off, static_cast<uint32_t>(wb));           // _LO
    add(0x3481 + off, static_cast<uint32_t>(ring >> 8));    // RB_BASE
    add(0x3482 + off, static_cast<uint32_t>(ring >> 40));   // RB_BASE_HI
    add(0x3492 + off, 0x10000000u | (0x1E0 + i));           // DOORBELL: ENABLE, OFFSET
    add(0x3487 + off, static_cast<uint32_t>(wb + 8));       // WPTR_POLL_ADDR_LO
    add(0x3486 + off, static_cast<uint32_t>((wb + 8) >> 32));
    add(0x3485 + off, 0);                                   // WPTR_POLL_CNTL: ENABLE 0
    add(0x3480 + off, 0x1017);                              // RB_SIZE | RPTR_WRITEBACK_ENABLE | RB_ENABLE
    add(0x348a + off, ((i == 0 ? ((initial_value(0x348a) & ~0x800f0111u) | 0x100) : ((initial_value(0x368a) & ~0x800f0111u) | 0x100)) & ~1u) | 1);   // IB_ENABLE
  }
  add(0x3412, 0); add(0x3612, 0);                           // un-halted
  add(0x3404, 0x40002); add(0x3604, 0x40002);               // context switch on
  // (the ring tests follow: the HDP flush of each commit)
  size_t at = 0;
  for (const auto& e : expect) {
    CHECK(at < w.size() && w[at] == e);
    if (at < w.size() && w[at] != e) printf("  write %zu: %04x=%08x, expected %04x=%08x\n", at, w[at].first, w[at].second, e.first, e.second);
    ++at;
  }
  CHECK(at + 2 == w.size() && w[at] == std::make_pair(fakesdma::kHdpFlush, uint32_t{1}) && w[at + 1] == std::make_pair(fakesdma::kHdpFlush, uint32_t{1}));
  // The rings: the ring test packet (WRITE linear of one dword to the test word), then the padding up to 16 dwords - burst NOP on engine 0, plain NOPs on engine 1.
  for (uint32_t i = 0; i < 2; ++i) {
    const uint64_t test_word = rig.gpu + i * kSlot + 8192 + 16;
    CHECK(rig.ring_word(i, 0) == 2 && rig.ring_word(i, 1) == static_cast<uint32_t>(test_word) && rig.ring_word(i, 2) == static_cast<uint32_t>(test_word >> 32) && rig.ring_word(i, 3) == 1 && rig.ring_word(i, 4) == 0xDEADBEEF);
    CHECK(rig.ring_word(i, 5) == (i == 0 ? 0x000A0000u : 0));
    for (uint32_t k = 6; k < 16; ++k) CHECK(rig.ring_word(i, k) == 0);
    CHECK(rig.card.get32(test_word) == 0xDEADBEEF && rig.doorbell(0x1E0 + i) == 64 && sdma.write_pointer(i) == 16);
    CHECK(rig.card.get32(rig.gpu + i * kSlot + 8192) == 64 && rig.card.get32(rig.gpu + i * kSlot + 8192 + 8) == 64);   // read pointer written back, write pointer shadow (bytes)
  }
  CHECK(rig.card.protocol_errors == 0 && rig.card.writes_done == 2);
}

static void test_copy_fill() {
  Rig rig;
  AmdSdma3 sdma(rig.card, rig.clock);
  CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok);
  uint8_t* src = rig.data.base;
  uint8_t* dst = rig.data.base + kBig;
  for (uint32_t i = 0; i < kBig; ++i) src[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
  // A small copy on each engine.
  CHECK(sdma.copy(0, rig.data_gpu + 3, rig.dst_gpu + 5, 100));
  CHECK(memcmp(dst + 5, src + 3, 100) == 0 && dst[4] == 0 && dst[105] == 0);
  CHECK(sdma.copy(1, rig.data_gpu + 1000, rig.dst_gpu + 2000, 4096));
  CHECK(memcmp(dst + 2000, src + 1000, 4096) == 0);
  // 4 MiB + 100 bytes: two chunks (0x3fffe0 and the rest).
  rig.card.copy_sizes.clear();
  CHECK(sdma.copy(0, rig.data_gpu, rig.dst_gpu, 0x400000 + 100));
  CHECK(memcmp(dst, src, 0x400000 + 100) == 0 && rig.card.copy_sizes.size() == 2 && rig.card.copy_sizes[0] == 0x3fffe0 && rig.card.copy_sizes[1] == 0x400000 + 100 - 0x3fffe0);
  // A fill, in a destination that is not aligned to a dword (the engine only needs the byte count to be).
  CHECK(sdma.fill(1, rig.dst_gpu + 64, 0xA5A5A5A5, 256));
  CHECK(dst[63] == src[63] && dst[64] == 0xA5 && dst[319] == 0xA5 && dst[320] == src[320]);
  CHECK(sdma.fill(0, rig.dst_gpu + 1000, 0x01020304, 8) && dst[1000] == 4 && dst[1003] == 1 && dst[1007] == 1 && dst[1008] == src[1008]);
  // Fences count up on each engine and every operation traps once.
  CHECK(rig.card.fences == 5 && rig.card.traps[0] + rig.card.traps[1] == 5 && rig.card.protocol_errors == 0);
  // The ring wraps (2048 dwords, 16 dwords per small copy): 300 copies go round it twice.
  for (uint32_t i = 0; i < 300; ++i) {
    CHECK(sdma.copy(i & 1, rig.data_gpu + i, rig.dst_gpu + 10000 + i * 3, 3));
    if (i == 50) memset(dst + 10000, 0, 1000);
  }
  CHECK(memcmp(dst + 10000 + 299 * 3, src + 299, 3) == 0 && rig.card.protocol_errors == 0);
  CHECK(sdma.write_pointer(0) % 16 == 0 && sdma.write_pointer(0) < 2048);
}

static void test_refusals() {
  Rig rig;
  AmdSdma3 sdma(rig.card, rig.clock);
  CHECK(!sdma.copy(0, rig.data_gpu, rig.data_gpu + 64, 16) && !sdma.fill(0, rig.data_gpu, 0, 16));   // not running
  CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok);
  const size_t before = rig.card.writes.size();
  CHECK(!sdma.copy(2, rig.data_gpu, rig.data_gpu + 64, 16) && !sdma.fill(2, rig.data_gpu, 0, 16));        // no such engine
  CHECK(!sdma.copy(0, rig.data_gpu, rig.data_gpu + 64, 0) && !sdma.fill(0, rig.data_gpu, 0, 0));
  CHECK(!sdma.fill(0, rig.data_gpu, 0, 6));                                                              // not a whole number of dwords
  CHECK(!sdma.copy(0, 0x10000000000ull, rig.data_gpu, 16) && !sdma.copy(0, rig.data_gpu, 0x10000000000ull, 16));   // beyond 40 bits
  CHECK(!sdma.copy(0, 0xFFFFFFFFF0ull, rig.data_gpu, 64) && !sdma.copy(0, rig.data_gpu, 0xFFFFFFFFF0ull, 64));   // runs past the end of the address space
  CHECK(!sdma.fill(0, 0xFFFFFFFFF0ull, 0, 64) && !sdma.fill(0, 0x10000000000ull, 0, 16));
  // More chunks than the ring could hold: 2048 dwords / 7 = 292 chunks at most, here 400.
  CHECK(!sdma.copy(0, rig.data_gpu, rig.data_gpu, uint64_t{0x3fffe0} * 400));
  CHECK(rig.card.writes.size() == before);   // nothing was queued
  // Start refusals
  Rig bad;
  AmdSdma3 s2(bad.card, bad.clock);
  AmdSdma3::Memory m = bad.memory();
  AmdSdma3::Memory small = m;
  small.block = MmioRegion(m.block.address(), 2 * kSlot - 4096);
  CHECK(s2.start(small) == AmdSdma3::Result::BadArgument);
  AmdSdma3::Memory skew = m;
  skew.gpu_address += 256;
  CHECK(s2.start(skew) == AmdSdma3::Result::BadArgument);
  AmdSdma3::Memory beyond = m;
  beyond.gpu_address = 0xFFFFFFF000ull;
  CHECK(s2.start(beyond) == AmdSdma3::Result::BadArgument);
  AmdSdma3::Memory no_doorbells = m;
  no_doorbells.doorbells = MmioRegion();
  CHECK(s2.start(no_doorbells) == AmdSdma3::Result::BadArgument);
  AmdSdma3::Memory short_doorbells = m;
  short_doorbells.doorbells = MmioRegion(m.doorbells.address(), 0x1E1 * 4);
  CHECK(s2.start(short_doorbells) == AmdSdma3::Result::BadArgument);
  CHECK(s2.start(AmdSdma3::Memory()) == AmdSdma3::Result::BadArgument);
  AmdSdma3::Config far;
  far.doorbell[1] = 0x200000;
  AmdSdma3 s3(bad.card, bad.clock, far);
  AmdSdma3::Memory huge = m;
  huge.doorbells = MmioRegion(m.doorbells.address(), 0x900000);
  CHECK(s3.start(huge) == AmdSdma3::Result::BadArgument);
  CHECK(bad.card.writes.empty());
}

static void test_failures() {
  {
    Rig rig;   // the second engine's microcode is not loaded
    rig.card.loaded[1] = false;
    AmdSdma3 sdma(rig.card, rig.clock);
    CHECK(sdma.start(rig.memory(), 2000) == AmdSdma3::Result::RingTestFailed && sdma.failed_instance() == 1 && !sdma.running());
  }
  {
    Rig rig;
    rig.card.stall[0] = true;
    AmdSdma3 sdma(rig.card, rig.clock);
    CHECK(sdma.start(rig.memory(), 2000) == AmdSdma3::Result::RingTestFailed && sdma.failed_instance() == 0);
  }
  {
    Rig rig;   // an engine that stops answering is wedged; starting again clears it
    AmdSdma3 sdma(rig.card, rig.clock);
    CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok);
    rig.card.stall[1] = true;
    CHECK(!sdma.copy(1, rig.data_gpu, rig.data_gpu + 64, 16, 2000) && sdma.wedged(1) && !sdma.wedged(0));
    rig.card.stall[1] = false;
    CHECK(!sdma.copy(1, rig.data_gpu, rig.data_gpu + 64, 16) && sdma.copy(0, rig.data_gpu, rig.data_gpu + 64, 16));
    CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok && !sdma.wedged(1) && sdma.copy(1, rig.data_gpu, rig.data_gpu + 64, 16));
  }
}

static void test_fences() {
  // A fence word left at 1 by an earlier life of the memory must not complete the first operation, and a finished operation's number must not complete the next one.
  Rig rig;
  const uint64_t fence = kSlot * 0 + 8192 + 24;
  rig.block.base[fence] = 1;
  AmdSdma3 sdma(rig.card, rig.clock);
  CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok);
  rig.card.stall[0] = true;
  CHECK(!sdma.copy(0, rig.data_gpu, rig.dst_gpu, 16, 2000) && sdma.wedged(0));
  Rig rig2;
  AmdSdma3 s2(rig2.card, rig2.clock);
  CHECK(s2.start(rig2.memory()) == AmdSdma3::Result::Ok && s2.copy(0, rig2.data_gpu, rig2.dst_gpu, 16));
  rig2.card.stall[0] = true;
  CHECK(!s2.copy(0, rig2.data_gpu, rig2.dst_gpu, 16, 2000));
  // The fence number counts up on the engine that did the work: three copies, the third fence is 3.
  Rig rig3;
  AmdSdma3 s3(rig3.card, rig3.clock);
  CHECK(s3.start(rig3.memory()) == AmdSdma3::Result::Ok);
  for (int i = 0; i < 3; ++i) CHECK(s3.copy(1, rig3.data_gpu, rig3.dst_gpu, 16));
  CHECK(rig3.card.get32(rig3.gpu + kSlot + 8192 + 24) == 3 && rig3.card.get32(rig3.gpu + 8192 + 24) == 0);
}

static void test_ring_space() {
  Rig rig;
  AmdSdma3 sdma(rig.card, rig.clock);
  CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok);
  const uint32_t wptr = sdma.write_pointer(0);
  // The packets, the fence (7 dwords) and the padding (up to 15) must fit the 2048 dwords with one to spare: 290 copy chunks (7 dwords each) and 405 fill chunks (5) do not.
  CHECK(!sdma.copy(0, rig.data_gpu, rig.dst_gpu, uint64_t{0x3fffe0} * 290) && sdma.write_pointer(0) == wptr);
  CHECK(!sdma.fill(0, rig.dst_gpu, 0, uint64_t{0x3fffe0} * 405) && sdma.write_pointer(0) == wptr);
  // (one fewer chunk is queued: the memory behind it is not there, so the simulated engine objects, but the ring took the packets)
  sdma.copy(0, rig.data_gpu, rig.dst_gpu, uint64_t{0x3fffe0} * 289, 2000);
  CHECK(sdma.write_pointer(0) != wptr && rig.card.protocol_errors > 0);
}

static void test_stop() {
  Rig rig;
  AmdSdma3 sdma(rig.card, rig.clock);
  CHECK(sdma.start(rig.memory()) == AmdSdma3::Result::Ok);
  const size_t before = rig.card.writes.size();
  sdma.stop();
  const auto& w = rig.card.writes;
  CHECK(!sdma.running() && w.size() == before + 2 + 4 + 2);
  CHECK(w[before] == std::make_pair(uint32_t{0x3404}, uint32_t{0x2}) && w[before + 1] == std::make_pair(uint32_t{0x3604}, uint32_t{0x2}));   // context switching off
  CHECK(w[before + 2].first == 0x3480 && !(w[before + 2].second & 1) && w[before + 3].first == 0x348a && !(w[before + 3].second & 1));          // ring and IB disabled, engine 0
  CHECK(w[before + 4].first == 0x3680 && !(w[before + 4].second & 1) && w[before + 5].first == 0x368a && !(w[before + 5].second & 1));          // engine 1
  CHECK(w[before + 6] == std::make_pair(uint32_t{0x3412}, uint32_t{1}) && w[before + 7] == std::make_pair(uint32_t{0x3612}, uint32_t{1}));       // halted
  CHECK(!sdma.copy(0, rig.data_gpu, rig.data_gpu + 64, 16));
}

int main() {
  test_start();
  test_copy_fill();
  test_refusals();
  test_failures();
  test_fences();
  test_ring_space();
  test_stop();
  printf(g_failed ? "AmdSdma3Test: %d FAILED\n" : "AmdSdma3Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
