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


// Host-side tests for the compute queues of the Vega M graphics engine (AmdGfx8Compute, driven by AmdGfx8): the KIQ's descriptor and the hardware queue registers it is
// committed to, the packets on the KIQ ring that map the compute queues, what the simulated microcode did with them, the compute ring tests through doorbells, the unmap at
// stop, and the failures (the microcode not loaded, one queue not mapped). The register values are worked out by hand from the bit positions in AMD's headers; the descriptor's
// word for the register R of the hardware queue is word 128 + (R - 0x3245), which the generator checked against the kernel header.
#include <stdio.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Compute.hpp"
#include "FakeAmdGfx8.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Block {
  std::vector<uint8_t> storage;
  uint8_t* base = nullptr;
  explicit Block(size_t bytes) : storage(bytes + 4096) { base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 4095) & ~uintptr_t{4095}); }
};

constexpr uint64_t kSlot = AmdGfx8Compute::kSlotBytes;   // 20480

struct Rig {
  fakegfx::Card card;
  fakegfx::FakeClock clock;
  Block ring_block, wb_block, csb_block, compute_block, doorbells;
  uint64_t ring_gpu = 0x2'0010'0000ull, wb_gpu = 0x2'0020'0000ull, csb_gpu = 0x2'0030'0000ull, compute_gpu = 0x2'0100'0000ull;
  Rig() : ring_block(AmdGfx8::kRingBytes), wb_block(4096), csb_block(8192), compute_block(9 * kSlot), doorbells(4096) {
    card.windows.push_back({ring_gpu, ring_block.base, AmdGfx8::kRingBytes});
    card.windows.push_back({wb_gpu, wb_block.base, 4096});
    card.windows.push_back({csb_gpu, csb_block.base, 8192});
    card.windows.push_back({compute_gpu, compute_block.base, 9 * kSlot});
    card.doorbell_host = doorbells.base;
    card.doorbell_bytes = 4096;
    card.set_sram(card.soft_status_address(), 0x47E);   // the SMC has loaded the microcode
  }
  AmdGfx8::Memory memory() {
    AmdGfx8::Memory m;
    m.ring_gpu_address = ring_gpu;
    m.ring = MmioRegion(reinterpret_cast<uint64_t>(ring_block.base), AmdGfx8::kRingBytes);
    m.writeback_gpu_address = wb_gpu;
    m.writeback = MmioRegion(reinterpret_cast<uint64_t>(wb_block.base), 16);
    m.csb_gpu_address = csb_gpu;
    m.csb = MmioRegion(reinterpret_cast<uint64_t>(csb_block.base), 8192);
    return m;
  }
  AmdGfx8Compute::Memory compute_memory() {
    AmdGfx8Compute::Memory m;
    m.gpu_address = compute_gpu;
    m.block = MmioRegion(reinterpret_cast<uint64_t>(compute_block.base), 9 * kSlot);
    m.doorbells = MmioRegion(reinterpret_cast<uint64_t>(doorbells.base), 4096);
    return m;
  }
  uint32_t doorbell(uint32_t index) { const uint8_t* p = doorbells.base + index * 4; return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }
  uint32_t word(uint64_t gpu) { return card.get32(gpu); }
};

// The hardware queue registers of the descriptor, by register number (gfx_8_0_d.h), for a queue.
static void expected_mqd(uint64_t mqd_gpu, uint64_t ring_gpu, uint64_t eop_gpu, uint64_t wb_gpu, uint32_t doorbell, bool kiq, std::map<uint32_t, uint32_t>* regs) {
  auto& r = *regs;
  r[0x3245] = static_cast<uint32_t>(mqd_gpu) & 0xfffffffc;      // CP_MQD_BASE_ADDR
  r[0x3246] = static_cast<uint32_t>(mqd_gpu >> 32);             // _HI
  r[0x3247] = kiq ? 1 : 0;                                      // CP_HQD_ACTIVE: only the KIQ is activated by the driver
  r[0x3248] = 0;                                                // CP_HQD_VMID
  r[0x3249] = 0x5300;                                           // CP_HQD_PERSISTENT_STATE: PRELOAD_SIZE 0x53
  r[0x324c] = kiq ? 0 : 0xa11;                                  // CP_HQD_QUANTUM: QUANTUM_EN, SCALE 1, DURATION 10 (not for the KIQ)
  r[0x324d] = static_cast<uint32_t>(ring_gpu >> 8);             // CP_HQD_PQ_BASE
  r[0x324e] = static_cast<uint32_t>(ring_gpu >> 40);            // _HI
  r[0x324f] = 0;                                                // CP_HQD_PQ_RPTR
  r[0x3250] = static_cast<uint32_t>(wb_gpu) & 0xfffffffc;       // CP_HQD_PQ_RPTR_REPORT_ADDR
  r[0x3251] = static_cast<uint32_t>(wb_gpu >> 32) & 0xffff;
  r[0x3252] = static_cast<uint32_t>(wb_gpu + 8) & 0xfffffffc;   // CP_HQD_PQ_WPTR_POLL_ADDR
  r[0x3253] = static_cast<uint32_t>((wb_gpu + 8) >> 32) & 0xffff;
  r[0x3254] = 0x40000000u | doorbell << 2;                      // CP_HQD_PQ_DOORBELL_CONTROL: DOORBELL_EN, DOORBELL_OFFSET
  r[0x3255] = 0;                                                // CP_HQD_PQ_WPTR
  r[0x3256] = 0xc000090a;                                       // CP_HQD_PQ_CONTROL: QUEUE_SIZE 10 (8 KiB), RPTR_BLOCK_SIZE 9, PRIV_STATE, KMD_QUEUE
  r[0x3257] = 0;                                                // CP_HQD_IB_BASE_ADDR ...
  r[0x3258] = 0;
  r[0x3259] = 0;
  r[0x325a] = 0x18300000;                                       // CP_HQD_IB_CONTROL: MIN_IB_AVAIL_SIZE 3, MTYPE 3
  r[0x325b] = 0x18000000;                                       // CP_HQD_IQ_TIMER: MTYPE 3
  r[0x3267] = 0;                                                // CP_MQD_CONTROL: VMID 0
  r[0x326a] = static_cast<uint32_t>(eop_gpu >> 8);              // CP_HQD_EOP_BASE_ADDR
  r[0x326b] = static_cast<uint32_t>(eop_gpu >> 40);             // _HI
  r[0x326c] = 9;                                                // CP_HQD_EOP_CONTROL: EOP_SIZE 9 (4 KiB)
  r[0x3272] = 6;                                                // CP_HQD_CTX_SAVE_CONTROL: MTYPE 3
}

static void test_start() {
  Rig rig;
  AmdGfx8Compute compute(rig.card, rig.clock);
  CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok && compute.attached() && compute.queue_count() == 8);
  AmdGfx8 gfx(rig.card, rig.clock);
  gfx.attach_compute(&compute);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  CHECK(rig.card.compute_protocol_errors == 0 && rig.card.protocol_errors == 0 && compute.failed_queue() == -1);
  // The microcode mapped the eight compute queues: queues 0..7 of pipe 0 of MEC 1, doorbells 0x10 + n, descriptors and write-back words in their slots.
  CHECK(rig.card.resources_set && rig.card.queue_mask == 0xFF && rig.card.mapped.size() == 8);
  for (size_t i = 0; i < rig.card.mapped.size(); ++i) {
    const fakegfx::Mapped& m = rig.card.mapped[i];
    const uint64_t slot = rig.compute_gpu + (i + 1) * kSlot;
    CHECK(m.me == 1 && m.pipe == 0 && m.queue == i && m.doorbell == 0x10 + i && m.mqd_address == slot && m.wptr_address == slot + 4096 + 8192 + 4096 + 8);
    // The descriptor: header, fixed fields, then the registers of the hardware queue.
    CHECK(m.mqd.size() == 512 && m.mqd[0] == 0xC0310800 && m.mqd[11] == 1 && m.mqd[23] == 0xffffffff && m.mqd[24] == 0xffffffff && m.mqd[26] == 0xffffffff && m.mqd[27] == 0xffffffff && m.mqd[32] == 3);
    CHECK(m.mqd[126] == static_cast<uint32_t>(slot + 514 * 4) && m.mqd[127] == static_cast<uint32_t>((slot + 514 * 4) >> 32));   // the dynamic CU mask word's address
    std::map<uint32_t, uint32_t> regs;
    expected_mqd(slot, slot + 4096, slot + 4096 + 8192, slot + 4096 + 8192 + 4096, 0x10 + i, false, &regs);
    for (const auto& r : regs) CHECK(m.mqd[128 + r.first - 0x3245] == r.second);
    // The hardware queue the microcode set up from it is active and runs the ring.
    CHECK(rig.card.bank_active(1, 0, i) && rig.card.bank_reg(1, 0, i, 0x3254) == 0x40000000u + ((0x10 + i) << 2));
  }
  // The two words past the descriptor: the dynamic CU and render back-end masks are all ones.
  CHECK(rig.word(rig.compute_gpu + 1 * kSlot + 514 * 4) == 0xFFFFFFFF && rig.word(rig.compute_gpu + 1 * kSlot + 515 * 4) == 0xFFFFFFFF);
  // The KIQ: MEC 1, pipe 3, queue 0, doorbell 0; its hardware queue was programmed by the driver from the descriptor.
  std::map<uint32_t, uint32_t> kiq;
  expected_mqd(rig.compute_gpu, rig.compute_gpu + 4096, rig.compute_gpu + 4096 + 8192, rig.compute_gpu + 4096 + 8192 + 4096, 0, true, &kiq);
  for (const auto& r : kiq) {
    CHECK(rig.card.bank_reg(1, 3, 0, r.first) == r.second);
    CHECK(rig.word(rig.compute_gpu + (128 + r.first - 0x3245) * 4) == r.second);   // and the descriptor in video memory
  }
  CHECK(rig.card.bank_active(1, 3, 0) && rig.card.reg(0xecaa) == 0xB8);   // RLC_CP_SCHEDULERS: KIQ at me 1 (<<5), pipe 3 (<<3), queue 0, enabled (0x80)
  // The doorbell range and the doorbell enable
  CHECK(rig.card.reg(0x305c) == 0 && rig.card.reg(0x305d) == 0x17u << 2 && (rig.card.reg(0x30b8) & 2) == 2);
  // The rings: the KIQ's has SET_RESOURCES and the eight MAP_QUEUES (8 + 8 * 7 = 64 dwords, padded to 256), then its test packet (padded to 512).
  const uint64_t kiq_ring = rig.compute_gpu + 4096;
  CHECK(rig.word(kiq_ring) == 0xC006A000 && rig.word(kiq_ring + 4) == 0 && rig.word(kiq_ring + 8) == 0xFF && rig.word(kiq_ring + 12) == 0 && rig.word(kiq_ring + 16) == 0);
  for (uint32_t i = 0; i < 8; ++i) {
    const uint64_t at = kiq_ring + (8 + i * 7) * 4;
    const uint64_t slot = rig.compute_gpu + (i + 1) * kSlot;
    CHECK(rig.word(at) == 0xC005A200 && rig.word(at + 4) == 0x20000000 && rig.word(at + 8) == (((0x10u + i) << 2) | (i << 26)) && rig.word(at + 12) == static_cast<uint32_t>(slot) &&
          rig.word(at + 16) == static_cast<uint32_t>(slot >> 32) && rig.word(at + 20) == static_cast<uint32_t>(slot + 4096 + 8192 + 4096 + 8) && rig.word(at + 24) == static_cast<uint32_t>((slot + 4096 + 8192 + 4096 + 8) >> 32));
  }
  for (uint32_t i = 64; i < 256; ++i) CHECK(rig.word(kiq_ring + i * 4) == 0xFFFF1000);
  CHECK(rig.word(kiq_ring + 256 * 4) == 0xC0017900 && rig.word(kiq_ring + 257 * 4) == 0x40 && rig.word(kiq_ring + 258 * 4) == 0xDEADBEEF);
  // The doorbells carry the write pointers: the KIQ 512, each compute queue 256 (its test packet only); the write-back words too.
  CHECK(rig.doorbell(0) == 512 && compute.write_pointer(0) == 512);
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(rig.doorbell(0x10 + i) == 256 && compute.write_pointer(i + 1) == 256);
    const uint64_t slot = rig.compute_gpu + (i + 1) * kSlot;
    CHECK(rig.word(slot + 4096 + 8192 + 4096 + 8) == 256 && rig.word(slot + 4096 + 8192 + 4096) == 256);   // (the microcode wrote the read pointer back)
  }
  CHECK(rig.card.queue_executed == 8);
  // The order of the register writes of the KIQ: the scheduler register, the queue selection, the wptr poll disable, the hardware queue registers in the driver's three runs.
  const auto& w = rig.card.writes;
  size_t at = 0;
  while (at < w.size() && w[at].first != 0xecaa) ++at;
  CHECK(at + 4 < w.size() && w[at].second == 0xB8 && w[at + 1] == std::make_pair(uint32_t{0x391}, uint32_t{0x7}) && w[at + 2].first == 0x3083);
  size_t k = at + 3;
  for (uint32_t r = 0x3248; r <= 0x326c; ++r) { CHECK(k < w.size() && w[k].first == r); ++k; }
  CHECK(k + 3 < w.size() && w[k].first == 0x326d && w[k + 1].first == 0x326e && w[k + 2].first == 0x3279);
  k += 3;
  for (uint32_t r = 0x326f; r <= 0x3278; ++r) { CHECK(k < w.size() && w[k].first == r); ++k; }
  for (uint32_t r = 0x3245; r <= 0x3247; ++r) { CHECK(k < w.size() && w[k].first == r); ++k; }
  CHECK(k < w.size() && w[k] == std::make_pair(uint32_t{0x391}, uint32_t{0}));
  // gfx_v8_0_cp_resume order: the KIQ before the graphics ring is set up, the compute queues (the microcode un-halted, the doorbell range) after it.
  size_t scheduler = w.size(), ring_delay = w.size(), ring_base = w.size(), mec_run = w.size(), range = w.size();
  for (size_t i = 0; i < w.size(); ++i) {
    if (w[i].first == 0xecaa && scheduler == w.size()) scheduler = i;
    if (w[i].first == 0x21c1 && ring_delay == w.size()) ring_delay = i;        // CP_RB_WPTR_DELAY: the start of the graphics ring's programming
    if (w[i].first == 0x3040 && ring_base == w.size()) ring_base = i;          // CP_RB0_BASE: its end
    if (w[i] == std::make_pair(fakegfx::kCpMecCntl, uint32_t{0}) && mec_run == w.size()) mec_run = i;
    if (w[i].first == 0x305c && range == w.size()) range = i;
  }
  CHECK(scheduler < ring_delay && ring_base < mec_run && mec_run < range);
  // Each commit flushes the host data path first: one more flush per queue for another round of tests.
  size_t flushes = 0;
  for (const auto& write : w) flushes += write == std::make_pair(fakegfx::kHdpFlush, uint32_t{1});
  CHECK(compute.test_rings() == AmdGfx8Compute::Result::Ok);
  size_t after = 0;
  for (const auto& write : rig.card.writes) after += write == std::make_pair(fakegfx::kHdpFlush, uint32_t{1});
  CHECK(after == flushes + 9);
}

static void test_stop() {
  Rig rig;
  AmdGfx8Compute compute(rig.card, rig.clock);
  CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok);
  AmdGfx8 gfx(rig.card, rig.clock);
  gfx.attach_compute(&compute);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  gfx.stop();
  // RESET_QUEUES for each compute queue by doorbell, in order, through the KIQ; the queues are inactive, the KIQ still is until the microcode is halted.
  CHECK(rig.card.unmapped_doorbells.size() == 8);
  for (uint32_t i = 0; i < rig.card.unmapped_doorbells.size(); ++i) CHECK(rig.card.unmapped_doorbells[i] == 0x10 + i && !rig.card.bank_active(1, 0, i));
  CHECK(rig.card.bank_active(1, 3, 0) && rig.card.compute_protocol_errors == 0);
  const uint64_t kiq_ring = rig.compute_gpu + 4096;
  CHECK(rig.word(kiq_ring + 512 * 4) == 0xC004A300 && rig.word(kiq_ring + 513 * 4) == 0x20000001 && rig.word(kiq_ring + 514 * 4) == (0x10u << 2) && rig.word(kiq_ring + 515 * 4) == 0);
  CHECK(rig.word(kiq_ring + (512 + 6) * 4) == 0xC004A300 && rig.word(kiq_ring + (512 + 8) * 4) == (0x11u << 2));
  // Then the graphics and compute processors are halted.
  CHECK((rig.card.reg(fakegfx::kCpMeCntl) & (fakegfx::kMeHalt | fakegfx::kPfpHalt | fakegfx::kCeHalt)) == (fakegfx::kMeHalt | fakegfx::kPfpHalt | fakegfx::kCeHalt));
  CHECK(rig.card.reg(fakegfx::kCpMecCntl) == (fakegfx::kMecMe1Halt | fakegfx::kMecMe2Halt));
}

static void test_fewer_queues() {
  Rig rig;
  AmdGfx8Compute::Config c;
  c.queues = 3;
  AmdGfx8Compute compute(rig.card, rig.clock, c);
  CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok);
  AmdGfx8 gfx(rig.card, rig.clock);
  gfx.attach_compute(&compute);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  CHECK(rig.card.mapped.size() == 3 && rig.card.queue_mask == 0x7 && rig.card.reg(0x305d) == 0x17u << 2);   // the doorbell range still covers the driver's eight
  CHECK(rig.doorbell(0) == 256 + 256 && rig.doorbell(0x12) == 256 && rig.doorbell(0x13) == 0);              // (3 * 7 + 8 = 29 dwords: one 256 block, plus the test packet's)
}

static void test_other_pipe_and_doorbells() {
  Rig rig;
  AmdGfx8Compute::Config c;
  c.queues = 2;
  c.compute_pipe = 1;
  c.kiq_doorbell = 2;
  c.first_compute_doorbell = 0x30;
  AmdGfx8Compute compute(rig.card, rig.clock, c);
  CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok);
  AmdGfx8 gfx(rig.card, rig.clock);
  gfx.attach_compute(&compute);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  // The queue mask has the bits of pipe 1 (pipe * 8 + queue), the packets name pipe 1 and the doorbells.
  CHECK(rig.card.queue_mask == 0x300 && rig.card.mapped.size() == 2);
  for (uint32_t i = 0; i < rig.card.mapped.size(); ++i) CHECK(rig.card.mapped[i].pipe == 1 && rig.card.mapped[i].queue == i && rig.card.mapped[i].doorbell == 0x30 + i);
  // The doorbell range: from the KIQ's doorbell to the last of the driver's eight after the first compute doorbell.
  CHECK(rig.card.reg(0x305c) == 2u << 2 && rig.card.reg(0x305d) == (0x30u + 7) << 2);
  CHECK(rig.doorbell(2) == 512 && rig.doorbell(0x30) == 256 && rig.doorbell(0x31) == 256 && rig.doorbell(0) == 0);
  CHECK(rig.card.bank_reg(1, 3, 0, 0x3254) == (0x40000000u | 2u << 2));
}

static void test_failures() {
  {
    Rig rig;   // the microcode is not loaded: the KIQ never runs
    rig.card.set_sram(rig.card.soft_status_address(), 0x47E & ~(1u << 6));
    AmdGfx8Compute compute(rig.card, rig.clock);
    CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok);
    AmdGfx8 gfx(rig.card, rig.clock);
    gfx.attach_compute(&compute);
    CHECK(gfx.start(rig.memory(), 2000) == AmdGfx8::Result::ComputeFailed && compute.failed_queue() == 0);
    CHECK(compute.write_pointer(1) == 0);   // the compute queues are not tried when the KIQ is dead
    CHECK(rig.card.writes.back().first != fakegfx::kCpIntCntlRing0 || rig.card.writes.back().second == 0);   // the GUI idle interrupt stays off
  }
  {
    Rig rig;   // one queue is not mapped: its ring test fails, the others pass
    rig.card.skip_map_bit = 3;
    AmdGfx8Compute compute(rig.card, rig.clock);
    CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok);
    AmdGfx8 gfx(rig.card, rig.clock);
    gfx.attach_compute(&compute);
    CHECK(gfx.start(rig.memory(), 2000) == AmdGfx8::Result::ComputeFailed && compute.failed_queue() == 4);
    CHECK(rig.card.mapped.size() == 7 && rig.card.queue_executed == 7);
  }
  {
    Rig rig;   // the graphics ring fails first: that is what is reported
    rig.card.cp_alive = false;
    AmdGfx8Compute compute(rig.card, rig.clock);
    CHECK(compute.attach(rig.compute_memory()) == AmdGfx8Compute::Result::Ok);
    AmdGfx8 gfx(rig.card, rig.clock);
    gfx.attach_compute(&compute);
    CHECK(gfx.start(rig.memory(), 2000) == AmdGfx8::Result::RingTestFailed);
  }
  {
    Rig rig;   // no compute attached: nothing of it is touched
    AmdGfx8 gfx(rig.card, rig.clock);
    CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok && rig.card.mapped.empty() && rig.card.reg(0xecaa) == 0);
  }
}

static void test_attach() {
  Rig rig;
  AmdGfx8Compute::Memory m = rig.compute_memory();
  {
    AmdGfx8Compute c(rig.card, rig.clock);
    AmdGfx8Compute::Memory small = m;
    small.block = MmioRegion(m.block.address(), 8 * kSlot);
    CHECK(c.attach(small) == AmdGfx8Compute::Result::BadArgument && !c.attached());
    AmdGfx8Compute::Memory skew = m;
    skew.gpu_address += 256;
    CHECK(c.attach(skew) == AmdGfx8Compute::Result::BadArgument);
    AmdGfx8Compute::Memory no_doorbells = m;
    no_doorbells.doorbells = MmioRegion();
    CHECK(c.attach(no_doorbells) == AmdGfx8Compute::Result::BadArgument);
    AmdGfx8Compute::Memory short_doorbells = m;
    short_doorbells.doorbells = MmioRegion(m.doorbells.address(), 0x17 * 4);   // doorbell 0x17 is the last of eight queues
    CHECK(c.attach(short_doorbells) == AmdGfx8Compute::Result::BadArgument);
    CHECK(c.attach(AmdGfx8Compute::Memory()) == AmdGfx8Compute::Result::BadArgument);
    CHECK(c.test_rings() == AmdGfx8Compute::Result::BadArgument && !c.disable_queues());
    CHECK(c.attach(m) == AmdGfx8Compute::Result::Ok);
  }
  const uint32_t bad[][5] = {{0, 1, 3, 0, 0}, {9, 1, 3, 0, 0}, {8, 0, 3, 0, 0}, {8, 3, 3, 0, 0}, {8, 1, 4, 0, 0}, {8, 1, 3, 0, 8}, {8, 1, 0, 0, 0}};   // queues, kiq me, kiq pipe, compute pipe changed, queue
  for (const auto& b : bad) {
    AmdGfx8Compute::Config c;
    c.queues = b[0];
    c.kiq_me = b[1];
    c.kiq_pipe = b[2];
    c.kiq_queue = b[4];
    AmdGfx8Compute compute(rig.card, rig.clock, c);
    CHECK(compute.attach(m) == AmdGfx8Compute::Result::BadArgument);
  }
  AmdGfx8Compute::Config same;
  same.kiq_pipe = 0;   // the KIQ's pipe is the compute pipe
  AmdGfx8Compute collide(rig.card, rig.clock, same);
  CHECK(collide.attach(m) == AmdGfx8Compute::Result::BadArgument);
  AmdGfx8Compute::Config far;
  far.first_compute_doorbell = 0xFFFFF;
  AmdGfx8Compute far_doorbell(rig.card, rig.clock, far);
  CHECK(far_doorbell.attach(m) == AmdGfx8Compute::Result::BadArgument);
  // A doorbell BAR big enough to hold it does not make the index fit the descriptor's 20-bit field.
  AmdGfx8Compute::Memory huge = m;
  huge.doorbells = MmioRegion(m.doorbells.address(), 0x500000);
  CHECK(far_doorbell.attach(huge) == AmdGfx8Compute::Result::BadArgument);
  CHECK(rig.card.writes.empty());   // nothing was written to the card
  // The end-of-pipe buffers are cleared by the attach.
  for (size_t i = 0; i < 9 * kSlot; ++i) rig.compute_block.base[i] = 0xAA;
  AmdGfx8Compute ok(rig.card, rig.clock);
  CHECK(ok.attach(m) == AmdGfx8Compute::Result::Ok);
  bool clear = true;
  for (uint32_t slot = 0; slot < 9; ++slot)
    for (uint64_t o = 0; o < 4096; ++o) clear = clear && rig.compute_block.base[slot * kSlot + 4096 + 8192 + o] == 0;
  CHECK(clear && rig.compute_block.base[4096] == 0xAA);
}

int main() {
  test_start();
  test_stop();
  test_fewer_queues();
  test_other_pipe_and_doorbells();
  test_failures();
  test_attach();
  printf(g_failed ? "AmdGfx8ComputeTest: %d FAILED\n" : "AmdGfx8ComputeTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
