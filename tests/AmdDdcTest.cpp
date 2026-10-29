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

// AmdDdc against a simulated I2C bus: the card's GPIO registers (mask, enable, output and readback of a clock and a data pad) wired to a monitor's EDID memory that is a
// bit-level I2C slave written independently of the master (start and stop detected on the lines, bits sampled on the rising clock, acknowledges and data driven on the falling
// clock, clock stretching). Checks the transfers on the wire, the register choreography, the EDID read with its extension blocks and segment pointer, the retries, a monitor that is
// absent, one that stretches the clock within the limit and past it, and damaged data under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDdc.hpp"
#include "FakeI2cMonitor.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// A clock that moves by one microsecond whenever it is looked at.
struct TickClock final : IClock {
  mutable uint64_t now = 1000;
  const char* name() const override { return "tick"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now++; }
};

using Bus = FakeI2cMonitor;

static bool same(ConstByteView a, const std::vector<uint8_t>& b, size_t n) {
  return a.size() == n && memcmp(a.data(), b.data(), n) == 0;
}

static void test_single_block() {
  TestAllocator alloc;
  TickClock clock;
  Bus bus;
  bus.edid = make_edid(0);
  AmdDdc ddc(bus, clock, bus.record());
  ByteBuffer out(alloc);
  CHECK(ddc.read_edid(&out) && same(out.view(), bus.edid, 128));
  // On the wire: the offset write (address 0x50, write), a repeated start, the read (0x50, read); one stop; the offset was 0.
  CHECK(bus.addresses.size() == 2 && bus.addresses[0] == 0xA0 && bus.addresses[1] == 0xA1);
  CHECK(bus.written.size() == 1 && bus.written[0] == 0);
  CHECK(bus.starts == 2 && bus.repeated_starts_seen == 1 && bus.stops == 1 && bus.stop_seen_last);
  // The lines are left released, the pads given back, the other register bits untouched, the outputs cleared.
  CHECK(bus.scl && bus.sda);
  CHECK(!(bus.get(Bus::kEnClk) & Bus::kEnClkBit) && !(bus.get(Bus::kEnData) & Bus::kEnDataBit));
  CHECK(!(bus.get(Bus::kMaskClk) & Bus::kMaskClkBit) && !(bus.get(Bus::kMaskData) & Bus::kMaskDataBit));
  CHECK(!(bus.get(Bus::kAClk) & Bus::kAClkBit) && !(bus.get(Bus::kAData) & Bus::kADataBit));
  CHECK((bus.get(Bus::kEnClk) & ~Bus::kEnClkBit) == (0x55555555u & ~Bus::kEnClkBit) && (bus.get(Bus::kMaskClk) & ~Bus::kMaskClkBit) == (0x55555555u & ~Bus::kMaskClkBit));
  CHECK((bus.get(Bus::kAClk) & ~Bus::kAClkBit) == (0x55555555u & ~Bus::kAClkBit) && (bus.get(Bus::kAData) & ~Bus::kADataBit) == (0x55555555u & ~Bus::kADataBit));
  CHECK((bus.get(Bus::kEnData) & ~Bus::kEnDataBit) == (0x55555555u & ~Bus::kEnDataBit) && (bus.get(Bus::kMaskData) & ~Bus::kMaskDataBit) == (0x55555555u & ~Bus::kMaskDataBit));
}

static void test_hw_capable_pads() {
  // A hardware capable line (the id's bit 7) has its pads switched to DDC mode first: bit 16 of the clock mask register is cleared (amdgpu_i2c_pre_xfer); other lines leave it.
  TestAllocator alloc;
  TickClock clock;
  for (int hw = 0; hw < 2; ++hw) {
    Bus bus;
    bus.edid = make_edid(0);
    AtomI2c::Bus record = bus.record();
    record.hw_capable = hw != 0;
    AmdDdc ddc(bus, clock, record);
    ByteBuffer out(alloc);
    const uint32_t before = bus.get(Bus::kMaskClk);
    CHECK(ddc.read_edid(&out));
    CHECK(((bus.get(Bus::kMaskClk) >> 16) & 1) == (hw ? 0u : ((before >> 16) & 1)));
  }
}

static void test_masked_while_busy() {
  // During the transfer the pads are masked to software (mask bits set), as they must be for the enable bits to matter.
  TestAllocator alloc;
  TickClock clock;
  struct Watch final : IVcnRegisters {
    Bus& bus;
    bool seen_enable_without_mask = false, seen_enable = false;
    explicit Watch(Bus& b) : bus(b) {}
    uint32_t read(uint32_t r) override { return bus.read(r); }
    void write(uint32_t r, uint32_t v) override {
      bus.write(r, v);
      if ((r == Bus::kEnClk && (v & Bus::kEnClkBit)) || (r == Bus::kEnData && (v & Bus::kEnDataBit))) {
        seen_enable = true;
        if (!(bus.get(Bus::kMaskClk) & Bus::kMaskClkBit) || !(bus.get(Bus::kMaskData) & Bus::kMaskDataBit)) seen_enable_without_mask = true;
      }
    }
  };
  Bus bus;
  bus.edid = make_edid(0);
  Watch watch(bus);
  AmdDdc ddc(watch, clock, bus.record());
  ByteBuffer out(alloc);
  CHECK(ddc.read_edid(&out) && watch.seen_enable && !watch.seen_enable_without_mask);
}

static void test_extensions() {
  TestAllocator alloc;
  TickClock clock;
  ByteBuffer out(alloc);
  // Block 1: offset 128 of segment 0. Blocks 2 and 3: the segment pointer 1 is written to 0x30 first, then offsets 0 and 128.
  {
    Bus bus;
    bus.edid = make_edid(3);
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && same(out.view(), bus.edid, 512));
    // addresses: block 0: A0 A1; block 1: A0 A1; block 2: 60 A0 A1; block 3: 60 A0 A1
    const std::vector<uint8_t> expect_addresses = {0xA0, 0xA1, 0xA0, 0xA1, 0x60, 0xA0, 0xA1, 0x60, 0xA0, 0xA1};
    CHECK(bus.addresses == expect_addresses);
    const std::vector<uint8_t> expect_written = {0, 128, 1, 0, 1, 128};
    CHECK(bus.written == expect_written);
    CHECK(bus.stops == 4);
  }
  // More extensions than the limit: three are read, the base block says so and still checks.
  {
    Bus bus;
    bus.edid = make_edid(7);
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && out.size() == 512 && out.view()[126] == 3);
    uint8_t sum = 0;
    for (size_t i = 0; i < 128; ++i) sum = static_cast<uint8_t>(sum + out.view()[i]);
    CHECK(sum == 0);
    CHECK(memcmp(out.view().data() + 128, bus.edid.data() + 128, 384) == 0);
  }
  // An extension block that never checks is left out, and the base block is made to say so.
  {
    Bus bus;
    bus.edid = make_edid(2);
    bus.edid[128 + 40] ^= 0x01;   // block 1 is bad (its checksum no longer matches)
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && out.size() == 256 && out.view()[126] == 1);
    uint8_t sum = 0;
    for (size_t i = 0; i < 128; ++i) sum = static_cast<uint8_t>(sum + out.view()[i]);
    CHECK(sum == 0 && memcmp(out.view().data() + 128, bus.edid.data() + 256, 128) == 0);
  }
}

static void test_start_state_and_time() {
  TestAllocator alloc;
  TickClock clock;
  ByteBuffer out(alloc);
  // The pins were left pulled low (by the firmware): they are let go of first, or no start condition could be made.
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.regs[Bus::kEnClk] |= Bus::kEnClkBit;
    bus.regs[Bus::kEnData] |= Bus::kEnDataBit;
    bus.update();
    CHECK(!bus.scl && !bus.sda);
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && same(out.view(), bus.edid, 128));
  }
  // The time it takes: 131 bytes on the wire (two addresses, the offset and the 128 read), nine clocks each, two half periods of 5 us at least per clock.
  {
    Bus bus;
    bus.edid = make_edid(0);
    AmdDdc ddc(bus, clock, bus.record());
    const uint64_t before = clock.now;
    CHECK(ddc.read_edid(&out));
    CHECK(clock.now - before >= 15000);   // (the bit times alone are 131 * 9 * 2 * 5 = 11790 us; the clock also moves when it is looked at)
  }
  // A damaged extension block is read again.
  {
    Bus bus;
    bus.edid = make_edid(1);
    bus.corrupt_skip = 1;
    bus.corrupt_block_reads = 2;
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && same(out.view(), bus.edid, 256) && bus.stops == 4);
  }
  // And the bus level refuses an invalid bus record by itself.
  {
    Bus bus;
    AtomI2c::Bus none;
    AmdDdc ddc(bus, clock, none);
    uint8_t byte = 0;
    AmdDdc::Message m;
    m.address = 0x50;
    m.data = ByteView(&byte, 1);
    CHECK(!ddc.transfer(ArrayView<AmdDdc::Message>(&m, 1)) && bus.writes == 0 && bus.starts == 0);
  }
}

static void test_retries_and_failures() {
  TestAllocator alloc;
  TickClock clock;
  ByteBuffer out(alloc);
  // A read that comes back damaged is read again (up to four times).
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.corrupt_block_reads = 1;
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && same(out.view(), bus.edid, 128) && bus.stops == 2);
  }
  // Damaged every time: four tries and no EDID, and the output is empty.
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.edid[40] ^= 0x04;
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(!ddc.read_edid(&out) && bus.stops == 4);
  }
  // A base block without the header, with a good checksum: no.
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.edid[1] = 0xFE;
    bus.edid[127] = static_cast<uint8_t>(bus.edid[127] + 1);
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(!ddc.read_edid(&out));
  }
  // Nothing on the other end: the address is not acknowledged (and the bus is let go).
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.present = false;
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(!ddc.read_edid(&out) && bus.scl && bus.sda);
    CHECK(!(bus.get(Bus::kMaskClk) & Bus::kMaskClkBit));
  }
  // An invalid bus record: nothing is touched.
  {
    Bus bus;
    bus.edid = make_edid(0);
    AtomI2c::Bus none;
    AmdDdc ddc(bus, clock, none);
    CHECK(!ddc.read_edid(&out) && bus.writes == 0);
  }
}

static void test_stretching() {
  TestAllocator alloc;
  TickClock clock;
  ByteBuffer out(alloc);
  // The monitor holds the clock low for a while after the address: waited for.
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.stretch_polls = 100;
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(ddc.read_edid(&out) && same(out.view(), bus.edid, 128));
  }
  // Held for ever: given up on after the limit (2.2 ms by the clock), the lines let go, and not stuck.
  {
    Bus bus;
    bus.edid = make_edid(0);
    bus.stretch_forever = true;
    const uint64_t before = clock.now;
    AmdDdc ddc(bus, clock, bus.record());
    CHECK(!ddc.read_edid(&out));
    const uint64_t spent = clock.now - before;
    CHECK(spent >= AmdDdc::kStretchTimeoutUs && spent < 8 * 100000);
    CHECK(!(bus.get(Bus::kEnClk) & Bus::kEnClkBit) && !(bus.get(Bus::kEnData) & Bus::kEnDataBit) && !(bus.get(Bus::kMaskClk) & Bus::kMaskClkBit));
  }
}

static void test_transfer() {
  TestAllocator alloc;
  TickClock clock;
  Bus bus;
  bus.edid = make_edid(0);
  for (size_t i = 0; i < 128; ++i) bus.edid[i] = static_cast<uint8_t>(i);   // (not an EDID: the bus level does not care)
  AmdDdc ddc(bus, clock, bus.record());
  uint8_t offset = 4, data[3] = {};
  AmdDdc::Message m[2];
  m[0].address = 0x50;
  m[0].data = ByteView(&offset, 1);
  m[1].address = 0x50;
  m[1].read = true;
  m[1].data = ByteView(data, 3);
  CHECK(ddc.transfer(ArrayView<AmdDdc::Message>(m, 2)) && data[0] == 4 && data[1] == 5 && data[2] == 6);
  // Several bytes written are all acknowledged; a different address is not.
  uint8_t two[2] = {7, 9};
  AmdDdc::Message w;
  w.address = 0x50;
  w.data = ByteView(two, 2);
  bus.written.clear();
  CHECK(ddc.transfer(ArrayView<AmdDdc::Message>(&w, 1)) && bus.written.size() == 2 && bus.written[0] == 7 && bus.written[1] == 9);
  w.address = 0x51;
  CHECK(!ddc.transfer(ArrayView<AmdDdc::Message>(&w, 1)) && bus.scl && bus.sda);
  CHECK(!ddc.transfer(ArrayView<AmdDdc::Message>()));
  // Pins left pulled low are let go of by the transfer itself, and the very first transfer works (no retry is there to hide it).
  Bus pulled;
  pulled.edid = bus.edid;
  pulled.regs[Bus::kEnClk] |= Bus::kEnClkBit;
  pulled.regs[Bus::kEnData] |= Bus::kEnDataBit;
  pulled.update();
  AmdDdc ddc2(pulled, clock, pulled.record());
  offset = 2;
  m[0].data = ByteView(&offset, 1);
  m[1].data = ByteView(data, 3);
  CHECK(ddc2.transfer(ArrayView<AmdDdc::Message>(m, 2)) && data[0] == 2 && data[1] == 3 && data[2] == 4);
}

// Damaged data from the monitor, every bit of the base block and of the first bytes of an extension: no crash, and what is accepted checks
static void test_damage() {
  TestAllocator alloc;
  TickClock clock;
  ByteBuffer out(alloc);
  const std::vector<uint8_t> good = make_edid(1);
  uint32_t accepted = 0;
  for (size_t at = 0; at < 160; ++at) {
    for (int bit = 0; bit < 8; ++bit) {
      Bus bus;
      bus.edid = good;
      bus.edid[at] ^= static_cast<uint8_t>(1 << bit);
      AmdDdc ddc(bus, clock, bus.record());
      if (ddc.read_edid(&out)) {
        ++accepted;
        uint8_t sum = 0;
        for (size_t i = 0; i < 128; ++i) sum = static_cast<uint8_t>(sum + out.view()[i]);
        if (sum != 0 || out.size() % 128 != 0 || out.size() < 128) { printf("  byte %zu bit %d\n", at, bit); CHECK(false); }
      }
    }
  }
  // A flip in the base block is never accepted (its checksum, or its header, fails); one in the extension block only costs the extension.
  CHECK(accepted == 32 * 8);
}

int main() {
  test_single_block();
  test_masked_while_busy();
  test_hw_capable_pads();
  test_start_state_and_time();
  test_extensions();
  test_retries_and_failures();
  test_stretching();
  test_transfer();
  test_damage();
  printf(g_failed ? "AmdDdcTest: %d FAILED\n" : "AmdDdcTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
