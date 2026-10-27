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


// Host-side tests for the Vega M interrupt handler ring (AmdIh3) against a simulated IH: the register values and write order of the start (worked out by hand from AMD's
// headers), the decoding of vectors, the read pointer through the doorbell and through the register, wrap-around, a ring overflow and its recovery, the stop, and every refused
// argument.
#include <stdio.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIh3.hpp"
#include "FakeAmdIh3.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Block {
  std::vector<uint8_t> storage;
  uint8_t* base = nullptr;
  explicit Block(size_t bytes) : storage(bytes + 4096) { base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 4095) & ~uintptr_t{4095}); }
};

constexpr uint32_t kRing = 64 * 1024;

struct Rig {
  fakeih::Card card;
  fakeih::FakeClock clock;
  Block ring, doorbells;
  uint64_t ring_gpu = 0x1'8000'0000ull;
  Rig() : ring(kRing + 8), doorbells(0x2000 * 4) {
    card.windows.push_back({ring_gpu, ring.base, kRing + 8});
    card.doorbell_host = doorbells.base;
    card.doorbell_bytes = 0x2000 * 4;
    card.clock = &clock;
  }
  AmdIh3::Memory memory() {
    AmdIh3::Memory m;
    m.ring_gpu_address = ring_gpu;
    m.ring = MmioRegion(reinterpret_cast<uint64_t>(ring.base), kRing + 8);
    m.dummy_page_gpu_address = 0x5000;
    m.doorbells = MmioRegion(reinterpret_cast<uint64_t>(doorbells.base), 0x2000 * 4);
    return m;
  }
  uint32_t doorbell(uint32_t index) { const uint8_t* p = doorbells.base + index * 4; return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }
  uint32_t ring_word(uint32_t byte) { const uint8_t* p = ring.base + byte; return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }
};

static void test_start() {
  Rig rig;
  rig.card.set_reg(fakeih::kInterruptCntl, 0xF);   // IH_DUMMY_RD_OVERRIDE (bit 0) and IH_REQ_NONSNOOP_EN (bit 3) set
  rig.card.set_reg(fakeih::kDoorbellRptr, 0x40000000);   // (CAPTURED, which stays)
  rig.card.set_reg(fakeih::kRbCntl, 0x20001);
  AmdIh3 ih(rig.card, rig.clock);
  CHECK(ih.start(rig.memory()) == AmdIh3::Result::Ok && ih.running());
  const std::pair<uint32_t, uint32_t> expected[] = {
      {fakeih::kRbCntl, 0},                         // tonga_ih_disable_interrupts: RB_ENABLE and ENABLE_INTR off
      {fakeih::kRbRptr, 0}, {fakeih::kRbWptr, 0},
      {fakeih::kInterruptCntl2, 0x50},              // the dummy page >> 8
      {fakeih::kInterruptCntl, 0x6},
      {fakeih::kRbBase, 0x1800000},                 // [39:8] of 0x1_8000_0000
      {fakeih::kRbCntl, 0x8000011C},                // WPTR_OVERFLOW_CLEAR, RB_SIZE 14 (64 KiB of dwords: log2(16384)) at bit 1, WPTR_WRITEBACK_ENABLE
      {fakeih::kWptrAddrLo, 0x80010000},            // the write pointer's write-back word follows the ring
      {fakeih::kWptrAddrHi, 0x1},
      {fakeih::kRbRptr, 0}, {fakeih::kRbWptr, 0},
      {fakeih::kDoorbellRptr, 0x500001E8},          // OFFSET 0x1E8, ENABLE
      {fakeih::kRbCntl, 0x8002011D},                // RB_ENABLE | ENABLE_INTR
  };
  const auto& w = rig.card.writes;
  CHECK(w.size() == sizeof(expected) / sizeof(expected[0]));
  for (size_t i = 0; i < w.size() && i < sizeof(expected) / sizeof(expected[0]); ++i) {
    CHECK(w[i] == expected[i]);
    if (w[i] != expected[i]) printf("  write %zu: %04x=%08x, expected %04x=%08x\n", i, w[i].first, w[i].second, expected[i].first, expected[i].second);
  }
  // With MSI the read pointer is re-armed; without the doorbell the enable bit is cleared and the offset left.
  Rig msi;
  msi.card.set_reg(fakeih::kDoorbellRptr, 0x10000055);
  AmdIh3::Config c;
  c.msi = true;
  c.use_doorbell = false;
  AmdIh3 ih2(msi.card, msi.clock, c);
  CHECK(ih2.start(msi.memory()) == AmdIh3::Result::Ok);
  CHECK(msi.card.reg(fakeih::kRbCntl) == 0x8022011D && msi.card.reg(fakeih::kDoorbellRptr) == 0x55);
  // A ring in video memory is not snooped.
  Rig vram;
  AmdIh3::Config v;
  v.ring_in_video_memory = true;
  AmdIh3 ih4(vram.card, vram.clock, v);
  CHECK(ih4.start(vram.memory()) == AmdIh3::Result::Ok && vram.card.reg(fakeih::kInterruptCntl) == 0x8);
  // A different doorbell index is programmed.
  Rig other;
  AmdIh3::Config d;
  d.doorbell_index = 0x1000;
  AmdIh3 ih3(other.card, other.clock, d);
  CHECK(ih3.start(other.memory()) == AmdIh3::Result::Ok && other.card.reg(fakeih::kDoorbellRptr) == 0x10001000);
}

static void test_drain() {
  Rig rig;
  AmdIh3 ih(rig.card, rig.clock);
  AmdIh3::Vector out[16];
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 0);   // not running
  rig.card.post(1, 2, 3, 4, 5);
  CHECK(rig.card.dropped == 1);
  CHECK(ih.start(rig.memory()) == AmdIh3::Result::Ok);
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 0);   // nothing yet
  // Five vectors, with the unused high bits set: the source data is 28 bits, ring id and VMID 8, the PASID 16.
  rig.card.post(181, 0xFABCDEF0, 0x12 | 0x300, 0x34, 0xABCD);
  rig.card.post(0xFF, 0x1, 0, 0, 0);
  rig.card.post(2, 3, 4, 5, 6);
  rig.card.post(7, 8, 9, 10, 11);
  rig.card.post(12, 13, 14, 15, 16);
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 2)) == 2 && ih.read_pointer() == 32);
  CHECK(out[0].src_id == 181 && out[0].src_data == 0x0ABCDEF0 && out[0].ring_id == 0x12 && out[0].vmid == 0x34 && out[0].pasid == 0xABCD);
  CHECK(out[1].src_id == 0xFF && out[1].src_data == 1 && out[1].ring_id == 0 && out[1].vmid == 0 && out[1].pasid == 0);
  // The read pointer goes through the doorbell and the write-back word after the ring.
  CHECK(rig.doorbell(0x1E8) == 32 && rig.ring_word(kRing + 4) == 32 && rig.card.reg(fakeih::kRbRptr) == 0);
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 3 && out[0].src_id == 2 && out[1].src_data == 8 && out[2].pasid == 16 && ih.read_pointer() == 80);
  CHECK(rig.doorbell(0x1E8) == 80 && ih.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 0 && ih.overflows() == 0);
  // A zero-size output takes nothing.
  rig.card.post(1, 1, 1, 1, 1);
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 0)) == 0 && ih.read_pointer() == 80);
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 1);
  // Wrap-around: more vectors than the ring holds in all, drained in time.
  for (uint32_t round = 0; round < 70; ++round) {
    for (uint32_t i = 0; i < 100; ++i) rig.card.post(i, round * 1000 + i, 0, 0, 0);
    uint32_t got = 0;
    while (true) {
      const uint32_t n = ih.drain(ArrayView<AmdIh3::Vector>(out, 16));
      if (n == 0) break;
      for (uint32_t i = 0; i < n; ++i) CHECK(out[i].src_id == (got + i) && out[i].src_data == round * 1000 + got + i);
      got += n;
    }
    CHECK(got == 100);
  }
  CHECK(ih.overflows() == 0 && ih.read_pointer() == (80 + 16 + 7000 * 16) % kRing);
  // The write pointer is read from the write-back word, not the register.
  Rig wb;
  wb.card.wptr_register_stuck = true;
  AmdIh3 ih3(wb.card, wb.clock);
  CHECK(ih3.start(wb.memory()) == AmdIh3::Result::Ok);
  wb.card.post(21, 22, 0, 0, 0);
  CHECK(ih3.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 1 && out[0].src_id == 21 && out[0].src_data == 22);
  // Without the doorbell the register carries the read pointer.
  Rig plain;
  AmdIh3::Config c;
  c.use_doorbell = false;
  AmdIh3 ih2(plain.card, plain.clock, c);
  CHECK(ih2.start(plain.memory()) == AmdIh3::Result::Ok);
  plain.card.post(9, 9, 9, 9, 9);
  CHECK(ih2.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 1 && plain.card.reg(fakeih::kRbRptr) == 16 && plain.doorbell(0x1E8) == 0);
}

static void test_overflow() {
  Rig rig;
  AmdIh3 ih(rig.card, rig.clock);
  AmdIh3::Vector out[4096];
  CHECK(ih.start(rig.memory()) == AmdIh3::Result::Ok);
  // 4097 vectors with nobody reading: the last one overwrites the first. The card flags the overflow when its write pointer catches the read pointer (at the 4096th vector).
  for (uint32_t i = 0; i < 4097; ++i) rig.card.post(1, i, 0, 0, 0);
  const uint32_t got = ih.drain(ArrayView<AmdIh3::Vector>(out, 4096));
  // The driver restarts at the vector after the one the write pointer is on (wptr + 16): wptr is 16, so from 32; the 4095 vectors up to the write pointer.
  CHECK(ih.overflows() == 1 && got == 4095 && out[0].src_data == 2 && out[got - 1].src_data == 4096 && ih.read_pointer() == 16);
  // The driver cleared the overflow flag through the control register: set, then cleared again at once, so a new overflow can be seen.
  const auto& w = rig.card.writes;
  CHECK(w.size() >= 2 && w[w.size() - 2].first == fakeih::kRbCntl && (w[w.size() - 2].second & 0x80000000u) && w[w.size() - 1].first == fakeih::kRbCntl && !(w[w.size() - 1].second & 0x80000000u));
  // Afterwards it works as before.
  rig.card.post(7, 77, 0, 0, 0);
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 1 && out[0].src_data == 77 && ih.overflows() == 1);
  // The write-back word still shows an overflow the register no longer has (the driver "double checks"): nothing is skipped.
  Rig stale;
  AmdIh3 ih2(stale.card, stale.clock);
  CHECK(ih2.start(stale.memory()) == AmdIh3::Result::Ok);
  stale.card.stale_writeback_overflow = true;
  stale.card.post(3, 33, 0, 0, 0);
  stale.card.post(4, 44, 0, 0, 0);
  CHECK(ih2.drain(ArrayView<AmdIh3::Vector>(out, 16)) == 2 && out[0].src_data == 33 && ih2.overflows() == 0);
}

static void test_stop() {
  Rig rig;
  AmdIh3 ih(rig.card, rig.clock);
  CHECK(ih.start(rig.memory()) == AmdIh3::Result::Ok);
  rig.card.post(1, 2, 3, 4, 5);
  AmdIh3::Vector out[4];
  CHECK(ih.drain(ArrayView<AmdIh3::Vector>(out, 4)) == 1);
  const size_t before = rig.card.writes.size();
  const uint64_t t0 = rig.clock.now;
  ih.stop();
  CHECK(!ih.running() && ih.read_pointer() == 0 && rig.card.writes.size() == before + 3);
  CHECK(rig.card.writes[before] == std::make_pair(fakeih::kRbCntl, uint32_t{0x8000011C}) && rig.card.writes[before + 1] == std::make_pair(fakeih::kRbRptr, uint32_t{0}) && rig.card.writes[before + 2] == std::make_pair(fakeih::kRbWptr, uint32_t{0}));
  CHECK(rig.clock.now - t0 >= 1000);   // mdelay(1)
  rig.card.post(5, 5, 5, 5, 5);
  CHECK(rig.card.dropped == 1 && ih.drain(ArrayView<AmdIh3::Vector>(out, 4)) == 0);
  // It can be started again, from the start of the ring.
  CHECK(ih.start(rig.memory()) == AmdIh3::Result::Ok && ih.read_pointer() == 0);
}

static void test_refusals() {
  Rig rig;
  AmdIh3 ih(rig.card, rig.clock);
  AmdIh3::Memory m = rig.memory();
  AmdIh3::Memory small = m;
  small.ring = MmioRegion(m.ring.address(), kRing + 4);
  CHECK(ih.start(small) == AmdIh3::Result::BadArgument);
  AmdIh3::Memory skew = m;
  skew.ring_gpu_address += 16;
  CHECK(ih.start(skew) == AmdIh3::Result::BadArgument);
  AmdIh3::Memory far = m;
  far.ring_gpu_address = 0xFFFFFF0000ull;
  CHECK(ih.start(far) == AmdIh3::Result::BadArgument);
  AmdIh3::Memory beyond = m;
  beyond.ring_gpu_address = 0x10000000000ull;
  CHECK(ih.start(beyond) == AmdIh3::Result::BadArgument);
  AmdIh3::Memory dummy = m;
  dummy.dummy_page_gpu_address = 0x5010;
  CHECK(ih.start(dummy) == AmdIh3::Result::BadArgument);
  dummy.dummy_page_gpu_address = 0x10000000000ull;
  CHECK(ih.start(dummy) == AmdIh3::Result::BadArgument);
  AmdIh3::Memory no_doorbell = m;
  no_doorbell.doorbells = MmioRegion();
  CHECK(ih.start(no_doorbell) == AmdIh3::Result::BadArgument);
  AmdIh3::Memory short_doorbell = m;
  short_doorbell.doorbells = MmioRegion(m.doorbells.address(), 0x1E8 * 4);
  CHECK(ih.start(short_doorbell) == AmdIh3::Result::BadArgument);
  CHECK(ih.start(AmdIh3::Memory()) == AmdIh3::Result::BadArgument);
  AmdIh3::Config big;
  big.doorbell_index = 0x200000;
  AmdIh3 bad(rig.card, rig.clock, big);
  AmdIh3::Memory huge = m;
  huge.doorbells = MmioRegion(m.doorbells.address(), 0x900000);
  CHECK(bad.start(huge) == AmdIh3::Result::BadArgument);
  // Without the doorbell it needs none.
  AmdIh3::Config plain;
  plain.use_doorbell = false;
  AmdIh3 ok(rig.card, rig.clock, plain);
  AmdIh3::Memory none = m;
  none.doorbells = MmioRegion();
  CHECK(ok.start(none) == AmdIh3::Result::Ok);
  CHECK(!ih.running() && rig.card.writes.size() > 0);
}

int main() {
  test_start();
  test_drain();
  test_overflow();
  test_stop();
  test_refusals();
  printf(g_failed ? "AmdIh3Test: %d FAILED\n" : "AmdIh3Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
