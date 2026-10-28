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

// AmdAux against a simulated AUX channel: an AtomBIOS image whose ProcessAuxChannelTransaction table hands the request in the scratch memory and the parameters to the card (a doorbell
// register), and a sink behind it written independently of the code under test from the DisplayPort AUX wire format (address, command and length bytes; native and I2C-over-AUX
// requests with the middle-of-transaction flag; replies as a status byte and payload) holding a DPCD and a monitor's EDID with its segment pointer. Checks the requests on the wire, the
// table's parameters, the native reads, the EDID read, defers and retries (native and I2C), timeouts, busy and error statuses, NACKs, short replies, a reply longer than asked for, an
// image without the table, and random replies under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdAux.hpp"
#include "AtomBiosImage.hpp"
#include "FakeAuxSink.hpp"
#include "FakeI2cMonitor.hpp"

using namespace hardware;
using namespace atomimage;

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

// The card behind the interpreter, and the sink behind the card.
struct World final : IAtomHost, FakeAuxSink {
  static constexpr uint32_t kParams0 = 0x7101, kDoorbell = 0x7100, kReplyStatus = 0x7102, kReplyLength = 0x7103;

  AtomBios* bios = nullptr;
  uint32_t params1 = 0;
  uint64_t delayed_us = 0;
  uint32_t reg_status = 0, reg_length = 0;

  uint8_t scratch_byte(uint32_t at) { return static_cast<uint8_t>(bios->scratch_word(at / 4) >> (at % 4 * 8)); }
  void put_scratch_byte(uint32_t at, uint8_t v) {
    const uint32_t shift = at % 4 * 8;
    bios->set_scratch_word(at / 4, (bios->scratch_word(at / 4) & ~(0xFFu << shift)) | (uint32_t{v} << shift));
  }

  uint32_t read_register(uint32_t reg) override { return reg == kReplyStatus ? reg_status : reg == kReplyLength ? reg_length : 0; }
  void write_register(uint32_t reg, uint32_t value) override {
    if (reg == kParams0) params1 = value;
    if (reg == kDoorbell) serve(value);
  }
  uint32_t read_mc(uint32_t) override { return 0; }
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t n) override { delayed_us += n; }
  void delay_ms(uint32_t n) override { delayed_us += uint64_t{n} * 1000; }
  uint64_t now_us() override { return ++clock_; }

  // the doorbell: the request is at the scratch byte the first parameter word names (low half), the reply goes to the high half
  void serve(uint32_t params0) {
    const uint32_t pointer = params0 & 0xFFFF, out_pointer = params0 >> 16;
    uint8_t request[20];
    for (uint32_t i = 0; i < 20; ++i) request[i] = scratch_byte(pointer + i);
    const Answer a = handle(request, params1, pointer, out_pointer);
    reg_status = a.status;
    reg_length = a.length;
    for (size_t i = 0; i < a.data.size(); ++i) put_scratch_byte(out_pointer + static_cast<uint32_t>(i), a.data[i]);
  }

 private:
  uint64_t clock_ = 0;
};

struct Rig {
  TestAllocator alloc;
  World world;
  std::vector<uint8_t> image;
  std::unique_ptr<AtomBios> bios;
  explicit Rig(bool with_table = true) {
    Image img;
    if (with_table) {
      Code c;
      c.move(REG, World::kParams0, PS, 1);
      c.move(REG, World::kDoorbell, PS, 0);
      c.move(PS, 1, REG, World::kReplyStatus, BYTE0, 1);     // the reply status into byte 1 of the second parameter word
      c.move(PS, 1, REG, World::kReplyLength, BYTE0, 2);     // the data length into byte 2
      c.eot();
      img.tables.push_back({78, 8, 8, c});
    } else {
      Code c;
      c.eot();
      img.tables.push_back({0, 0, 0, c});
    }
    image = img.build();
    bios.reset(new AtomBios(world, ConstByteView(image.data(), image.size()), alloc));
    world.bios = bios.get();
    world.edid = make_edid(0);
  }
};

static bool same(ConstByteView a, const std::vector<uint8_t>& b) { return a.size() == b.size() && (b.empty() || memcmp(a.data(), b.data(), b.size()) == 0); }
static std::vector<uint8_t> bytes(std::initializer_list<uint8_t> l) { return std::vector<uint8_t>(l); }

static void test_native_requests() {
  Rig r;
  AmdAux aux(r.world, *r.bios, 0x92, 3);
  // A DPCD read is split in requests of 16 bytes
  uint8_t out[40] = {};
  CHECK(aux.dpcd_read(0x100, ByteView(out, sizeof out)));
  bool ok = true;
  for (size_t i = 0; i < sizeof out; ++i) ok = ok && out[i] == static_cast<uint8_t>((0x100 + i) ^ 0x5A);
  CHECK(ok);
  CHECK(r.world.requests.size() == 3);
  CHECK(r.world.requests[0].bytes == bytes({0x00, 0x01, 0x90, 0x4F}));      // address 0x100, native read (9), 4 bytes on the wire, 16 bytes
  CHECK(r.world.requests[1].bytes == bytes({0x10, 0x01, 0x90, 0x4F}));
  CHECK(r.world.requests[2].bytes == bytes({0x20, 0x01, 0x90, 0x47}));      // 8 bytes
  // the table's parameters: request at scratch byte 4, data out at 20, the channel, no delay, the hot plug pin
  CHECK(r.world.requests[0].pointer == 4 && r.world.requests[0].out_pointer == 20 && r.world.requests[0].channel == 0x92 && r.world.requests[0].delay == 0 && r.world.requests[0].hpd == 3);
  // a native write: 12 bytes at most; address bits 19:16 go to the low nibble of the third byte
  AmdAux::Msg w;
  uint8_t payload[17];
  for (size_t i = 0; i < sizeof payload; ++i) payload[i] = static_cast<uint8_t>(0xA0 + i);
  w.request = AmdAux::kNativeWrite;
  w.address = 0x20102;
  w.buffer = ByteView(payload, 12);
  r.world.dpcd.resize(0x30000);
  CHECK(aux.do_msg(&w) == 12);
  const FakeAuxSink::Request& last = r.world.requests.back();
  CHECK(last.bytes.size() == 16 && last.bytes[0] == 0x02 && last.bytes[1] == 0x01 && last.bytes[2] == 0x82 && last.bytes[3] == 0x0B && last.bytes[4] == 0xA0);
  CHECK(r.world.dpcd[0x20102] == 0xA0 && r.world.dpcd[0x2010D] == 0xAB);
  const size_t before = r.world.requests.size();
  w.buffer = ByteView(payload, 13);
  CHECK(aux.aux_transfer(&w) == AmdAux::kInvalid);       // more than the table takes
  w.buffer = ByteView(payload, 17);
  w.request = AmdAux::kNativeRead;
  CHECK(aux.aux_transfer(&w) == AmdAux::kInvalid);
  w.request = 0xA;                                        // not a request
  w.buffer = ByteView(payload, 1);
  CHECK(aux.aux_transfer(&w) == AmdAux::kInvalid);
  CHECK(r.world.requests.size() == before);               // none of them ran the table
  // a read beyond the DPCD is NACKed by the sink: remote I/O error
  uint8_t more[4];
  CHECK(!aux.dpcd_read(0x30000, ByteView(more, 4)));
  CHECK(r.world.malformed == 0);
}

static void test_edid_on_the_wire() {
  Rig r;
  AmdAux aux(r.world, *r.bios, 0x91, AmdAux::kNoHpd);
  ByteBuffer edid(r.alloc);
  CHECK(aux.read_edid(&edid));
  CHECK(same(edid.view(), r.world.edid));
  // the exact traffic: bare write start, the offset, bare read start, 8 reads of 16, the closing bare read without the flag
  const auto& q = r.world.requests;
  CHECK(q.size() == 12);
  if (q.size() == 12) {
    CHECK(q[0].bytes == bytes({0x50, 0x00, 0x40}) && q[0].length_byte == 0x30);              // I2C write (0) with MOT (4): the bare address start
    CHECK(q[1].bytes == bytes({0x50, 0x00, 0x40, 0x50, 0x00}));        // one byte: the offset
    CHECK(q[2].bytes == bytes({0x50, 0x00, 0x50}) && q[2].length_byte == 0x30);              // I2C read (1) with MOT
    for (size_t i = 3; i < 11; ++i) CHECK(q[i].bytes == bytes({0x50, 0x00, 0x50, 0x4F}));
    CHECK(q[11].bytes == bytes({0x50, 0x00, 0x10}) && q[11].length_byte == 0x30);             // without MOT: the transaction is closed
  }
  for (const FakeAuxSink::Request& req : q) CHECK(req.channel == 0x91 && req.hpd == 0xFF && req.delay == 0);
  CHECK(r.world.malformed == 0 && r.world.outside_transaction == 0 && r.world.delayed_us == 0);
}

static void test_extensions_and_segments() {
  Rig r;
  r.world.edid = make_edid(3);
  AmdAux aux(r.world, *r.bios, 0x91, 0);
  ByteBuffer edid(r.alloc);
  CHECK(aux.read_edid(&edid) && same(edid.view(), r.world.edid));
  // block 2 is addressed through the segment pointer at 0x30 (one byte, 1), then the offset 0 of the segment; every message in its own transaction
  bool segment_write = false;
  for (const FakeAuxSink::Request& q : r.world.requests) if (q.bytes.size() == 5 && q.bytes[0] == 0x30 && q.bytes[2] == 0x40 && q.bytes[4] == 1) segment_write = true;
  CHECK(segment_write);
  CHECK(r.world.malformed == 0 && r.world.outside_transaction == 0 && r.world.segment == 0);   // the close resets the segment
}

static void test_defers() {
  // Native defers are waited out (500 us each) and the request repeated, up to 7 runs
  {
    Rig r;
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.native_defers = 6;
    uint8_t out[4];
    CHECK(aux.dpcd_read(0, ByteView(out, 4)));
    CHECK(r.world.requests.size() == 7 && r.world.delayed_us == 6 * 500);
  }
  {
    Rig r;
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.native_defers = 100;
    uint8_t out[4];
    CHECK(!aux.dpcd_read(0, ByteView(out, 4)));
    CHECK(r.world.requests.size() == 7);
  }
  // An I2C defer allows one more run each, up to 7 more
  {
    Rig r;
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.i2c_defers = 9;
    ByteBuffer edid(r.alloc);
    CHECK(aux.read_edid(&edid) && same(edid.view(), r.world.edid));
    CHECK(r.world.delayed_us == 9 * 500);
  }
  {
    Rig r;
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.i2c_defers = 1000;
    AmdAux::Message m;
    uint8_t offset = 0;
    m.address = 0x50;
    m.data = ByteView(&offset, 1);
    CHECK(!aux.transfer(ArrayView<AmdAux::Message>(&m, 1)));
    CHECK(r.world.requests.size() == 28);     // the start and the close each give up after 14 runs
  }
}

static void test_statuses() {
  uint8_t out[4];
  {
    Rig r;   // a timeout: no retry
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.timeouts = 1;
    AmdAux::Msg m;
    m.request = AmdAux::kNativeRead;
    m.buffer = ByteView(out, 4);
    CHECK(aux.do_msg(&m) == AmdAux::kTimedOut && r.world.requests.size() == 1);
  }
  {
    Rig r;   // busy ("flags not zero"): retried
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.busy = 3;
    CHECK(aux.dpcd_read(0, ByteView(out, 4)) && r.world.requests.size() == 4);
  }
  {
    Rig r;
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.busy = 100;
    CHECK(!aux.dpcd_read(0, ByteView(out, 4)) && r.world.requests.size() == 7);
  }
  {
    Rig r;   // an error status
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.errors = 1;
    AmdAux::Msg m;
    m.request = AmdAux::kNativeRead;
    m.buffer = ByteView(out, 4);
    CHECK(aux.aux_transfer(&m) == AmdAux::kIo && m.reply == 0);
  }
  {
    Rig r;   // a native NACK is final
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.native_nacks = 1;
    CHECK(!aux.dpcd_read(0, ByteView(out, 4)) && r.world.requests.size() == 1);
  }
  {
    Rig r;   // nobody at the I2C address: NACK, final; the EDID read tries the block four times, each transfer with its close
    AmdAux aux(r.world, *r.bios, 1, 0);
    r.world.monitor_present = false;
    ByteBuffer edid(r.alloc);
    CHECK(!aux.read_edid(&edid));
    CHECK(r.world.requests.size() == 4 * 2);
  }
  {
    Rig bad(false);
    AmdAux aux(bad.world, *bad.bios, 1, 0);
    ByteBuffer edid(bad.alloc);
    CHECK(!aux.read_edid(&edid) && !aux.dpcd_read(0, ByteView(out, 4)) && bad.world.requests.empty());
  }
  {
    Rig r;   // an empty transfer
    AmdAux aux(r.world, *r.bios, 1, 0);
    CHECK(!aux.transfer(ArrayView<AmdAux::Message>()));
  }
}

static void test_short_and_long_replies() {
  {
    Rig r;   // a sink that answers 5 bytes at most: the rest is asked for again
    r.world.max_read = 5;
    AmdAux aux(r.world, *r.bios, 1, 0);
    ByteBuffer edid(r.alloc);
    CHECK(aux.read_edid(&edid) && same(edid.view(), r.world.edid));
  }
  {
    Rig r;   // a table that claims more bytes than were asked for: only what was asked is taken
    r.world.claim_extra = 200;
    AmdAux aux(r.world, *r.bios, 1, 0);
    uint8_t out[8] = {};
    uint8_t guard[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    AmdAux::Msg m;
    m.request = AmdAux::kNativeRead;
    m.buffer = ByteView(out, 8);
    CHECK(aux.aux_transfer(&m) == 8 && guard[0] == 1);
  }
}

static void test_replies_and_sizes() {
  uint8_t out[8];
  {
    Rig r;   // the I2C reply bits mean nothing in the answer to a native request
    r.world.native_reply_extra = 0x40;
    AmdAux aux(r.world, *r.bios, 1, 0);
    CHECK(aux.dpcd_read(0x10, ByteView(out, 8)) && out[0] == (0x10 ^ 0x5A));
    AmdAux::Msg w;
    uint8_t payload[2] = {1, 2};
    w.request = AmdAux::kNativeWrite;
    w.address = 0x200;
    w.buffer = ByteView(payload, 2);
    CHECK(aux.do_msg(&w) == 2);
  }
  {
    Rig r;   // a native reply that is none of ACK, NACK and DEFER fails
    r.world.native_odd_reply = true;
    AmdAux aux(r.world, *r.bios, 1, 0);
    CHECK(!aux.dpcd_read(0x10, ByteView(out, 8)) && r.world.requests.size() == 1);
  }
  {
    Rig r;   // a sink that acknowledges a read and sends nothing: a protocol error, not an endless loop
    r.world.zero_reads = true;
    AmdAux aux(r.world, *r.bios, 1, 0);
    CHECK(!aux.dpcd_read(0x10, ByteView(out, 8)));
    ByteBuffer edid(r.alloc);
    CHECK(!aux.read_edid(&edid));
    AmdAux::Msg m;
    m.request = AmdAux::kNativeRead;
    m.buffer = ByteView(out, 8);
    CHECK(aux.do_msg(&m) == 0);
  }
  {
    Rig r;   // a write of more than the table takes at once is sent in requests of 12 bytes
    AmdAux aux(r.world, *r.bios, 1, 0);
    uint8_t data[30];
    for (size_t i = 0; i < sizeof data; ++i) data[i] = static_cast<uint8_t>(i + 1);
    AmdAux::Message m;
    m.address = 0x50;
    m.data = ByteView(data, sizeof data);
    CHECK(aux.transfer(ArrayView<AmdAux::Message>(&m, 1)));
    // the start, the data in 12, 12 and 6 bytes, the close
    const auto& q = r.world.requests;
    CHECK(q.size() == 5);
    if (q.size() == 5) CHECK(q[1].bytes.size() == 16 && q[1].length_byte == 0x0B && q[2].bytes.size() == 16 && q[3].bytes.size() == 10 && q[3].length_byte == 0xA5);
    CHECK(r.world.malformed == 0);
  }
}

static void test_random_replies() {
  // The sink answers with junk (statuses, lengths, payloads): nothing may be read or written outside its buffers, and what is returned as an EDID must check
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    Rig r;
    r.world.rng = seed * 7919;
    r.world.random_replies = true;
    AmdAux aux(r.world, *r.bios, 1, 0);
    ByteBuffer edid(r.alloc);
    if (aux.read_edid(&edid)) {
      uint8_t sum = 0;
      for (uint8_t b : edid.view()) sum = static_cast<uint8_t>(sum + b);
      CHECK(edid.size() >= 128 && edid.size() % 128 == 0 && sum == 0);
    }
    uint8_t dpcd[20];
    aux.dpcd_read(0, ByteView(dpcd, sizeof dpcd));
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_native_requests();
  test_edid_on_the_wire();
  test_extensions_and_segments();
  test_defers();
  test_statuses();
  test_short_and_long_replies();
  test_replies_and_sizes();
  test_random_replies();
  if (g_failed == 0) printf("AmdAuxTest: all passed\n");
  return g_failed ? 1 : 0;
}
