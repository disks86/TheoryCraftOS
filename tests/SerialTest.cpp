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

// Host-side tests for serial ports: the byte queue, and the 16550 UART driver against a fake chip (detection, framing and divisor, FIFO and
// non-FIFO transmit, receive with line errors, flow control, modem lines, a chip that vanishes) and the system list.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <vector>
#include "Hardware/SerialPort/SerialRing.hpp"
#include "Hardware/SerialPort/SystemSerialPorts.hpp"
#include "Hardware/SerialPort/Uart16550.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  bool fail = false;
  struct Header { uint64_t size; uint64_t pad; };
  void* allocate(uint64_t size) override {
    if (fail) return nullptr;
    Header* h = static_cast<Header*>(malloc(sizeof(Header) + size));
    if (!h) return nullptr;
    h->size = size;
    ++live;
    return h + 1;
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override {
    if (!p) return;
    --live;
    free(static_cast<Header*>(p) - 1);
  }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

// --- A fake 16550 -----------------------------------------------------------------------------------

struct FakeUart final : ISerialPortIo {
  explicit FakeUart(uint16_t b) : base(b) {}
  uint16_t base;
  bool present = true, fifo = true, scratch_ok = true, gone = false;
  uint8_t ier = 0, lcr = 0, mcr = 0, scratch = 0, fcr = 0, dll = 0, dlm = 0, msr_inputs = 0;
  uint8_t pending_lsr_errors = 0;
  std::deque<uint8_t> rx;
  Bytes wire;               // what was transmitted
  size_t tx_fifo = 0;       // bytes waiting to leave
  bool thr_busy = false;    // an empty non-FIFO chip is "busy" until drained()

  void drained() { tx_fifo = 0; thr_busy = false; }   // the line caught up
  uint16_t divisor() const { return static_cast<uint16_t>(dll | (dlm << 8)); }
  size_t capacity() const { return fifo ? 16 : 1; }

  uint8_t read8(uint16_t port) override {
    if (!present || gone) return 0xFF;
    const uint16_t off = port - base;
    const bool dlab = lcr & 0x80;
    switch (off) {
      case 0: if (dlab) return dll; else { if (rx.empty()) return 0; uint8_t v = rx.front(); rx.pop_front(); return v; }
      case 1: return dlab ? dlm : ier;
      case 2: return fifo && (fcr & 1) ? 0xC1 : 0x01;
      case 3: return lcr;
      case 4: return mcr;
      case 5: {
        uint8_t v = pending_lsr_errors;
        pending_lsr_errors = 0;
        if (!rx.empty()) v |= 0x01;
        if (tx_fifo < capacity()) v |= 0x20;
        if (tx_fifo == 0) v |= 0x40;
        return v;
      }
      case 6: {
        if (mcr & 0x10) return static_cast<uint8_t>(((mcr & 2) ? 0x10 : 0) | ((mcr & 1) ? 0x20 : 0));   // loopback: RTS->CTS, DTR->DSR
        return msr_inputs;
      }
      case 7: return scratch_ok ? scratch : 0xFF;
    }
    return 0xFF;
  }
  void write8(uint16_t port, uint8_t value) override {
    if (!present || gone) return;
    const uint16_t off = port - base;
    const bool dlab = lcr & 0x80;
    switch (off) {
      case 0: if (dlab) dll = value; else { wire.push_back(value); ++tx_fifo; } break;
      case 1: if (dlab) dlm = value; else ier = value; break;
      case 2: fcr = value; if ((value & 2) && !rx.empty()) rx.clear(); break;
      case 3: lcr = value; break;
      case 4: mcr = value; break;
      case 7: scratch = value; break;
    }
  }
};

// --- The byte queue ---------------------------------------------------------------------------------

static void test_ring() {
  TestAllocator a;
  {
    SerialRing ring;
    CHECK(ring.allocate(a, 8) && ring.capacity() == 8 && ring.size() == 0 && ring.space() == 8);
    const uint8_t in[] = {1, 2, 3, 4, 5, 6};
    CHECK(ring.push(ConstByteView(in, 6)) == 6);
    uint8_t out[4];
    CHECK(ring.pop(ByteView(out, 4)) == 4 && out[0] == 1 && out[3] == 4);
    // Wraps: 2 left, 6 more fit exactly across the end.
    const uint8_t more[] = {7, 8, 9, 10, 11, 12, 13};
    CHECK(ring.push(ConstByteView(more, 7)) == 6 && ring.size() == 8 && ring.space() == 0);
    CHECK(ring.push(0xEE) == false);
    uint8_t first;
    CHECK(ring.peek(&first) && first == 5);
    uint8_t all[16];
    CHECK(ring.pop(ByteView(all, 16)) == 8);
    const uint8_t expected[] = {5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(memcmp(all, expected, 8) == 0 && ring.size() == 0 && !ring.peek(&first));
    CHECK(ring.pop(ByteView(all, 16)) == 0 && ring.push(ConstByteView()) == 0);
    ring.release(a);
    CHECK(ring.capacity() == 0 && ring.push(1) == false);
  }
  {
    SerialRing ring;
    a.fail = true;
    CHECK(!ring.allocate(a, 8));
    a.fail = false;
  }
  CHECK(a.live == 0);
}

// --- The UART ---------------------------------------------------------------------------------------

static void test_detection() {
  TestAllocator a;
  {
    FakeUart none(Uart16550::kCom1);
    none.present = false;
    Uart16550 u(a, none, Uart16550::kCom1);
    CHECK(!u.initialize() && none.wire.empty() && u.state() == SerialState::Closed);
    CHECK(!u.open(SerialConfig()));
  }
  {
    FakeUart stuck(Uart16550::kCom1);
    stuck.scratch_ok = false;
    Uart16550 u(a, stuck, Uart16550::kCom1);
    CHECK(!u.initialize());
  }
  {
    FakeUart f(Uart16550::kCom1);
    Uart16550 u(a, f, Uart16550::kCom1);
    CHECK(u.initialize() && u.has_fifo() && u.base() == 0x3F8 && u.state() == SerialState::Closed);
    CHECK(u.write(text_bytes("x")) == 0 && f.wire.empty());   // closed: nothing moves
  }
  {
    FakeUart old(Uart16550::kCom2);
    old.fifo = false;
    Uart16550 u(a, old, Uart16550::kCom2);
    CHECK(u.initialize() && !u.has_fifo());
  }
  {
    FakeUart f(Uart16550::kCom1);
    a.fail = true;
    Uart16550 u(a, f, Uart16550::kCom1);
    CHECK(!u.initialize());
    a.fail = false;
  }
  CHECK(a.live == 0);
}

static void test_open() {
  TestAllocator a;
  FakeUart f(Uart16550::kCom1);
  Uart16550 u(a, f, Uart16550::kCom1);
  CHECK(u.initialize());
  SerialConfig c;
  c.baud = 9600;
  c.data_bits = 7;
  c.parity = SerialParity::Even;
  c.stop_bits = SerialStopBits::Two;
  CHECK(u.open(c) && u.state() == SerialState::Open);
  CHECK(f.divisor() == 12 && f.lcr == (0x02 | 0x04 | 0x18) && f.ier == 0);   // 115200 / 9600
  CHECK((f.mcr & 3) == 3);   // DTR and RTS raised
  c = SerialConfig();
  CHECK(u.open(c) && f.divisor() == 1 && f.lcr == 0x03);
  SerialConfig bad;
  bad.baud = 0;
  CHECK(!u.open(bad));
  bad = SerialConfig();
  bad.data_bits = 9;
  CHECK(!u.open(bad));
  bad = SerialConfig();
  bad.baud = 230400;   // faster than the chip's divisor allows
  CHECK(!u.open(bad));
  CHECK(u.state() == SerialState::Open && u.config().baud == 115200);   // a refused open leaves the port as it was
  c.parity = SerialParity::Odd;
  CHECK(u.open(c) && (f.lcr & 0x38) == 0x08);
  c.parity = SerialParity::Mark;
  CHECK(u.open(c) && (f.lcr & 0x38) == 0x28);
  c.parity = SerialParity::Space;
  CHECK(u.open(c) && (f.lcr & 0x38) == 0x38);
  u.close();
  CHECK(u.state() == SerialState::Closed && (f.mcr & 3) == 0);
}

static void test_transmit() {
  TestAllocator a;
  {
    FakeUart f(Uart16550::kCom1);
    Uart16550 u(a, f, Uart16550::kCom1);
    CHECK(u.initialize() && u.open(SerialConfig()));
    Bytes data;
    for (int i = 0; i < 40; ++i) data.push_back(static_cast<uint8_t>(i));
    CHECK(u.write(ConstByteView(data.data(), data.size())) == 40);
    CHECK(f.wire.size() == 16 && u.transmit_pending());   // one FIFO's worth went at once
    f.drained();
    u.poll();
    CHECK(f.wire.size() == 32);
    f.drained();
    u.poll();
    CHECK(f.wire.size() == 40 && memcmp(f.wire.data(), data.data(), 40) == 0);
    f.drained();
    u.poll();
    CHECK(!u.transmit_pending() && u.statistics().bytes_sent == 40);
  }
  {
    // Without a FIFO: one byte per empty holding register.
    FakeUart f(Uart16550::kCom1);
    f.fifo = false;
    Uart16550 u(a, f, Uart16550::kCom1);
    CHECK(u.initialize() && u.open(SerialConfig()));
    CHECK(u.write(text_bytes("abc")) == 3 && f.wire.size() == 1);
    u.poll();
    CHECK(f.wire.size() == 1);   // still busy
    f.drained();
    u.poll();
    CHECK(f.wire.size() == 2);
  }
  {
    // A full send queue takes what fits and says so.
    FakeUart f(Uart16550::kCom1);
    Uart16550 u(a, f, Uart16550::kCom1);
    CHECK(u.initialize() && u.open(SerialConfig()));
    Bytes big(Uart16550::kQueueBytes + 100, 0x55);
    const uint32_t taken = u.write(ConstByteView(big.data(), big.size()));
    CHECK(taken == Uart16550::kQueueBytes && f.wire.size() == 16);   // (the first FIFO's worth has left already)
    CHECK(u.bytes_writable() == 16 && u.write(ConstByteView(big.data(), big.size())) == 16);
  }
  {
    // RTS/CTS: nothing leaves while the other end holds CTS low.
    FakeUart f(Uart16550::kCom1);
    Uart16550 u(a, f, Uart16550::kCom1);
    SerialConfig c;
    c.flow_control = SerialFlowControl::RtsCts;
    CHECK(u.initialize() && u.open(c));
    CHECK(u.write(text_bytes("hold")) == 4 && f.wire.empty());
    f.msr_inputs = 0x10;   // CTS
    u.poll();
    CHECK(f.wire.size() == 4);
  }
  CHECK(a.live == 0);
}

static void test_receive() {
  TestAllocator a;
  FakeUart f(Uart16550::kCom1);
  Uart16550 u(a, f, Uart16550::kCom1);
  CHECK(u.initialize() && u.open(SerialConfig()));
  for (uint8_t b : {1, 2, 3, 4, 5}) f.rx.push_back(b);
  u.poll();
  CHECK(u.bytes_readable() == 5 && u.statistics().bytes_received == 5);
  uint8_t out[3];
  CHECK(u.read(ByteView(out, 3)) == 3 && out[0] == 1 && out[2] == 3);
  uint8_t rest[8];
  CHECK(u.read(ByteView(rest, 8)) == 2 && rest[0] == 4 && rest[1] == 5);   // (read polls for what has come meanwhile)
  f.rx.push_back(9);
  CHECK(u.read(ByteView(rest, 8)) == 1 && rest[0] == 9);
  f.rx.push_back(7);
  u.poll();
  u.discard_input();
  CHECK(u.bytes_readable() == 0);

  // Line errors are counted.
  f.pending_lsr_errors = 0x02;
  u.poll();
  f.pending_lsr_errors = 0x04;
  u.poll();
  f.pending_lsr_errors = 0x08;
  u.poll();
  f.pending_lsr_errors = 0x10;
  u.poll();
  const SerialStatistics s = u.statistics();
  CHECK(s.overruns == 1 && s.parity_errors == 1 && s.framing_errors == 1 && s.breaks == 1);

  // Our queue full: the surplus is counted, the poll still ends.
  for (uint32_t i = 0; i < Uart16550::kQueueBytes; i += 128) {
    for (int j = 0; j < 128; ++j) f.rx.push_back(0x11);
    u.poll();
    u.poll();
  }
  const uint32_t before = u.bytes_readable();
  for (int j = 0; j < 50; ++j) f.rx.push_back(0x22);
  u.poll();
  CHECK(before == Uart16550::kQueueBytes && u.bytes_readable() == Uart16550::kQueueBytes && u.statistics().overruns >= 51);
}

static void test_lines_and_vanish() {
  TestAllocator a;
  FakeUart f(Uart16550::kCom1);
  Uart16550 u(a, f, Uart16550::kCom1);
  CHECK(u.initialize() && u.open(SerialConfig()));
  f.msr_inputs = 0x10 | 0x80;
  const uint8_t l = u.lines();
  CHECK((l & SerialLines::kCts) && (l & SerialLines::kCarrier) && !(l & SerialLines::kDsr) && (l & SerialLines::kDtr) && (l & SerialLines::kRts));
  u.set_lines(SerialLines::kDtr);
  CHECK((f.mcr & 3) == 1 && !(u.lines() & SerialLines::kRts));
  u.set_break(true);
  CHECK(f.lcr & 0x40);
  u.set_break(false);
  CHECK(!(f.lcr & 0x40));

  f.gone = true;   // the card was pulled out: the bus reads all ones
  u.poll();
  CHECK(u.state() == SerialState::Error);
  u.shutdown();
  CHECK(a.live == 0);
}

// --- The system list --------------------------------------------------------------------------------

struct FakeBus final : ISerialPortIo {
  FakeUart com1{Uart16550::kCom1}, com3{Uart16550::kCom3};
  uint8_t read8(uint16_t port) override {
    if (port >= 0x3F8 && port < 0x400) return com1.read8(port);
    if (port >= 0x3E8 && port < 0x3F0) return com3.read8(port);
    return 0xFF;   // nothing decodes the other addresses
  }
  void write8(uint16_t port, uint8_t value) override {
    if (port >= 0x3F8 && port < 0x400) com1.write8(port, value);
    else if (port >= 0x3E8 && port < 0x3F0) com3.write8(port, value);
  }
};

static void test_system() {
  TestAllocator a;
  {
    FakeBus bus;
    SystemSerialPorts ports(bus, a);
    ports.initialize();
    CHECK(ports.count() == 2 && ports.at(2) == nullptr && !ports.share(5));
    CHECK(ports.at(0) && ports.at(0)->name()[3] == '1' && ports.at(1)->name()[3] == '3');
    SharedPointer<ISerialPort> p = ports.share(1);
    CHECK(p->open(SerialConfig()) && p->write(text_bytes("hi")) == 2);
    bus.com3.rx.push_back('!');
    ports.poll();
    CHECK(p->bytes_readable() == 1);
    CHECK(!ports.add(SharedPointer<ISerialPort>()));
    ports.shutdown();
    CHECK(ports.count() == 0 && p->state() == SerialState::Closed);
  }
  CHECK(a.live == 0);
}

int main() {
  test_ring();
  test_detection();
  test_open();
  test_transmit();
  test_receive();
  test_lines_and_vanish();
  test_system();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("SerialTest: all passed\n");
  return 0;
}
