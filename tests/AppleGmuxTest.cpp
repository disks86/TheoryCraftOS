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

// Apple's gmux against a model of its port window: the classic layout (registers are ports) and the indexed one (a mailbox with a handshake
// bit that sets only after a few polls, or never), the switch registers, the discrete power sequence, the backlight, a window where nothing
// answers, and finding the APP000B device and its I/O range in AML (with truncated and bit-flipped resource templates).
#include <map>
#include <vector>
#include "AcpiTestSupport.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/AppleGmux.hpp"

namespace {

constexpr uint16_t kBase = 0x700;

// The gmux's port window. Classic: register N is port base+N. Indexed: the registers are 32-bit values behind the mailbox at 0xC2 (value),
// 0xD0 (read index; reading it acknowledges), 0xD4 (write index; bit 0 of a read: a transaction finished).
struct FakeGmuxPorts final : IGmuxPorts {
  enum class Mode { Classic, Indexed, Dead } mode;
  uint16_t base = kBase, window = 0x100;
  std::map<uint8_t, uint8_t> classic;          // classic register bytes
  std::map<uint8_t, uint32_t> indexed;         // indexed registers
  uint8_t latch[4] = {};                       // the indexed value port's bytes
  uint8_t scratch[3] = {};
  bool done = false;                           // bit 0 of the write-index port
  int pending = -1;                            // polls of the status port until `done` sets
  uint32_t complete_after = 3;                 // how many status reads a transaction takes
  bool never_complete = false;
  std::vector<std::pair<uint8_t, uint8_t>> writes;   // (register, low byte) of every register write, both layouts
  uint32_t outside = 0;                        // accesses outside the window
  uint32_t transactions = 0;

  explicit FakeGmuxPorts(Mode m) : mode(m) {}

  bool in_window(uint16_t p) {
    if (p < base || p - base >= window) { ++outside; return false; }
    return true;
  }
  void reg_written(uint8_t reg, uint32_t value) {
    writes.push_back({reg, static_cast<uint8_t>(value)});
    // the switch: what the panel is connected to follows the display register
    if (reg == AppleGmux::kSwitchDisplay) set(AppleGmux::kSwitchGetDisplay, (value & 0xFF) == 2 ? 2 : 3);
    if (reg == AppleGmux::kSwitchExternal) set(AppleGmux::kSwitchGetExternal, (value & 0xFF) == 2 ? 2 : 3);
  }
  void set(uint8_t reg, uint32_t value) {
    if (mode == Mode::Indexed) indexed[reg] = value;
    else classic[reg] = static_cast<uint8_t>(value);
  }
  void set32(uint8_t reg, uint32_t value) {
    if (mode == Mode::Indexed) { indexed[reg] = value; return; }
    for (int i = 0; i < 4; ++i) classic[static_cast<uint8_t>(reg + i)] = static_cast<uint8_t>(value >> (8 * i));
  }
  uint32_t get32(uint8_t reg) {
    if (mode == Mode::Indexed) return indexed[reg];
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t{classic[static_cast<uint8_t>(reg + i)]} << (8 * i);
    return v;
  }
  void start() { ++transactions; pending = static_cast<int>(complete_after); done = false; }

  uint8_t read8(uint16_t p) override {
    if (!in_window(p) || mode == Mode::Dead) return 0xFF;
    const uint16_t off = static_cast<uint16_t>(p - base);
    if (mode == Mode::Classic) return classic.count(static_cast<uint8_t>(off)) ? classic[static_cast<uint8_t>(off)] : 0;
    if (off >= 0xCC && off <= 0xCE) return scratch[off - 0xCC];
    if (off >= AppleGmux::kIndexValue && off < AppleGmux::kIndexValue + 4) return latch[off - AppleGmux::kIndexValue];
    if (off == AppleGmux::kIndexRead) { done = false; return 0; }
    if (off == AppleGmux::kIndexWrite) {
      if (pending > 0 && !never_complete && --pending == 0) { done = true; pending = -1; }
      return done ? 0x01 : 0x00;
    }
    return 0xFF;                               // (the classic version registers read all ones on an indexed gmux)
  }
  void write8(uint16_t p, uint8_t value) override {
    if (!in_window(p) || mode == Mode::Dead) return;
    const uint16_t off = static_cast<uint16_t>(p - base);
    if (mode == Mode::Classic) {
      classic[static_cast<uint8_t>(off)] = value;
      reg_written(static_cast<uint8_t>(off), value);
      return;
    }
    if (off >= 0xCC && off <= 0xCE) {
      scratch[off - 0xCC] = value;
    } else if (off >= AppleGmux::kIndexValue && off < AppleGmux::kIndexValue + 4) {
      latch[off - AppleGmux::kIndexValue] = value;
    } else if (off == AppleGmux::kIndexRead) {
      const uint32_t v = indexed[value];
      for (int i = 0; i < 4; ++i) latch[i] = static_cast<uint8_t>(v >> (8 * i));
      start();
    } else if (off == AppleGmux::kIndexWrite) {
      const uint32_t v = latch[0] | (latch[1] << 8) | (latch[2] << 16) | (uint32_t{latch[3]} << 24);
      indexed[value] = v;
      reg_written(value, v);
      start();
    }
  }
  uint32_t read32(uint16_t p) override {
    if (!in_window(p) || !in_window(static_cast<uint16_t>(p + 3)) || mode == Mode::Dead) return 0xFFFFFFFF;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const uint16_t off = static_cast<uint16_t>(p - base + i);
      const uint8_t byte = mode == Mode::Classic ? (classic.count(static_cast<uint8_t>(off)) ? classic[static_cast<uint8_t>(off)] : 0)
                                                 : (off >= AppleGmux::kIndexValue && off < AppleGmux::kIndexValue + 4 ? latch[off - AppleGmux::kIndexValue] : 0xFF);
      v |= uint32_t{byte} << (8 * i);
    }
    return v;
  }
};

uint32_t g_delays = 0;
void count_delay(uint32_t us) { if (us == AppleGmux::kHandshakeDelayUs) ++g_delays; }

void model_registers(FakeGmuxPorts& io) {
  if (io.mode == FakeGmuxPorts::Mode::Classic) {
    io.set(AppleGmux::kVersionMajor, 1);
    io.set(AppleGmux::kVersionMinor, 9);
    io.set(AppleGmux::kVersionRelease, 23);
  } else {
    io.set32(AppleGmux::kVersionMajor, (4u << 24) | (2u << 16) | (5u << 8));
  }
  io.set(AppleGmux::kSwitchDdc, 1);
  io.set(AppleGmux::kSwitchGetDisplay, 2);
  io.set(AppleGmux::kSwitchGetExternal, 2);
  io.set32(AppleGmux::kMaxBrightness, 0x1E000);
  io.set32(AppleGmux::kBrightness, 0xFF00F000);  // (the top byte is not part of the level)
}

std::vector<uint8_t> written(const FakeGmuxPorts& io, uint8_t reg) {
  std::vector<uint8_t> r;
  for (const auto& w : io.writes) if (w.first == reg) r.push_back(w.second);
  return r;
}

void test_layout(FakeGmuxPorts::Mode mode) {
  FakeGmuxPorts io(mode);
  model_registers(io);
  g_delays = 0;
  AppleGmux gmux(io, kBase, 0x100, count_delay);
  CHECK(gmux.initialize());
  const bool indexed = mode == FakeGmuxPorts::Mode::Indexed;
  CHECK(gmux.layout() == (indexed ? AppleGmuxLayout::Indexed : AppleGmuxLayout::Classic));
  if (indexed) CHECK(gmux.version_major() == 4 && gmux.version_minor() == 2 && gmux.version_release() == 5);
  else CHECK(gmux.version_major() == 1 && gmux.version_minor() == 9 && gmux.version_release() == 23);
  CHECK(gmux.mechanism() == GpuPowerMechanism::AppleGmux);
  if (indexed) CHECK(io.transactions >= 2 && g_delays > 0);    // (each transaction needed a few polls)

  // the switch
  GpuMuxTarget t = GpuMuxTarget::Discrete;
  CHECK(gmux.active(&t) && t == GpuMuxTarget::Integrated);
  CHECK(gmux.active_ddc(&t) && t == GpuMuxTarget::Integrated);
  io.writes.clear();
  CHECK(gmux.switch_to(GpuMuxTarget::Discrete));
  CHECK(io.writes.size() == 2 && io.writes[0] == std::make_pair(uint8_t{AppleGmux::kSwitchDdc}, uint8_t{2}) &&
        io.writes[1] == std::make_pair(uint8_t{AppleGmux::kSwitchDisplay}, uint8_t{3}));   // DDC first, then the panel; no external port
  CHECK(gmux.active(&t) && t == GpuMuxTarget::Discrete);
  CHECK(gmux.active_ddc(&t) && t == GpuMuxTarget::Discrete);
  CHECK(gmux.active_external(&t) && t == GpuMuxTarget::Integrated);
  CHECK(gmux.switch_to(GpuMuxTarget::Integrated));
  CHECK(gmux.active(&t) && t == GpuMuxTarget::Integrated);
  CHECK(written(io, AppleGmux::kSwitchDdc).back() == 1 && written(io, AppleGmux::kSwitchDisplay).back() == 2);
  // DDC alone: the classic gmux only
  io.writes.clear();
  CHECK(gmux.switch_ddc(GpuMuxTarget::Discrete) == !indexed);
  CHECK(io.writes.size() == (indexed ? 0u : 1u));
  if (!indexed) CHECK(gmux.active_ddc(&t) && t == GpuMuxTarget::Discrete && gmux.active(&t) && t == GpuMuxTarget::Integrated);

  // the power sequence: 1 then 0 off, 1 then 3 on
  io.writes.clear();
  CHECK(gmux.discrete_powered());
  CHECK(gmux.power_off());
  CHECK(!gmux.discrete_powered());
  CHECK(gmux.power_on());
  CHECK(gmux.discrete_powered());
  CHECK((written(io, AppleGmux::kDiscretePower) == std::vector<uint8_t>{1, 0, 1, 3}));
  CHECK(gmux.power_on_delay_ms() == 100);

  // the backlight
  uint32_t v = 0;
  CHECK(gmux.max_brightness(&v) && v == 0x1E000);
  CHECK(gmux.steps() == 0x1E001);
  CHECK(gmux.brightness(&v) && v == 0xF000);
  uint8_t pct = 0;
  CHECK(gmux.percent(&pct) && pct == 50);
  CHECK(gmux.set_percent(25));
  CHECK(io.get32(AppleGmux::kBrightness) == 0x7800);
  CHECK(gmux.percent(&pct) && pct == 25);
  CHECK(gmux.set_brightness(0x123456));           // above the maximum: the maximum
  CHECK(io.get32(AppleGmux::kBrightness) == 0x1E000);
  CHECK(gmux.set_percent(200) && io.get32(AppleGmux::kBrightness) == 0x1E000);
  CHECK(gmux.set_percent(0) && io.get32(AppleGmux::kBrightness) == 0);
  CHECK(io.outside == 0);

  // a model whose external port goes through the gmux
  FakeGmuxPorts io2(mode);
  model_registers(io2);
  AppleGmux ext(io2, kBase, 0x100, nullptr, true);
  CHECK(ext.detect() && ext.external_switchable());
  CHECK(ext.switch_to(GpuMuxTarget::Discrete));
  CHECK(ext.active_external(&t) && t == GpuMuxTarget::Discrete);
  CHECK(written(io2, AppleGmux::kSwitchExternal) == std::vector<uint8_t>{3});
}

void test_indexed_handshake() {
  // a transaction that never finishes: every access fails (bounded), nothing hangs
  FakeGmuxPorts io(FakeGmuxPorts::Mode::Indexed);
  model_registers(io);
  io.never_complete = true;
  g_delays = 0;
  AppleGmux gmux(io, kBase, 0x100, count_delay);
  CHECK(!gmux.detect());
  CHECK(gmux.layout() == AppleGmuxLayout::None);
  CHECK(gmux.handshake_timeouts() >= 1);
  CHECK(g_delays <= 2 * AppleGmux::kHandshakePolls);
  GpuMuxTarget t;
  CHECK(!gmux.active(&t) && !gmux.switch_to(GpuMuxTarget::Discrete) && !gmux.power_off());

  // a transaction that finishes late but within the polls
  FakeGmuxPorts slow(FakeGmuxPorts::Mode::Indexed);
  model_registers(slow);
  slow.complete_after = AppleGmux::kHandshakePolls - 5;
  AppleGmux late(slow, kBase, 0x100);
  CHECK(late.detect() && late.version_major() == 4);

  // a finished transaction nobody acknowledged: wait_ready drains it first
  FakeGmuxPorts left(FakeGmuxPorts::Mode::Indexed);
  model_registers(left);
  left.done = true;
  AppleGmux drain(left, kBase, 0x100);
  CHECK(drain.detect() && drain.version_minor() == 2);
  uint32_t v = 0;
  CHECK(drain.brightness(&v) && v == 0xF000);
  CHECK(drain.handshake_timeouts() == 0);

  // the indexed scratch registers lie outside a classic-sized window: not probed there
  FakeGmuxPorts small(FakeGmuxPorts::Mode::Indexed);
  model_registers(small);
  small.window = AppleGmux::kMinIoLength;
  AppleGmux narrow(small, kBase, AppleGmux::kMinIoLength);
  CHECK(!narrow.detect() && small.outside == 0);
}

void test_absent() {
  // nothing at the window: every port reads all ones
  FakeGmuxPorts dead(FakeGmuxPorts::Mode::Dead);
  AppleGmux gmux(dead, kBase, 0x100);
  CHECK(!gmux.initialize());
  CHECK(gmux.layout() == AppleGmuxLayout::None);
  GpuMuxTarget t;
  uint32_t v;
  uint8_t pct;
  CHECK(!gmux.active(&t) && !gmux.switch_to(GpuMuxTarget::Discrete) && !gmux.switch_ddc(GpuMuxTarget::Discrete));
  CHECK(!gmux.power_off() && !gmux.power_on());
  CHECK(!gmux.max_brightness(&v) && !gmux.brightness(&v) && !gmux.set_brightness(5));
  CHECK(!gmux.percent(&pct) && !gmux.set_percent(50) && gmux.steps() == 0);
  uint8_t b8;
  CHECK(!gmux.read8(0x10, &b8) && !gmux.write8(0x10, 1) && !gmux.read32(0x70, &v) && !gmux.write32(0x74, 1));
  // a window too small, or one that would run past port 0xFFFF
  FakeGmuxPorts io(FakeGmuxPorts::Mode::Classic);
  model_registers(io);
  AppleGmux tiny(io, kBase, AppleGmux::kMinIoLength - 1);
  CHECK(!tiny.detect());
  AppleGmux wrap(io, 0xFFC0, 0x100);
  CHECK(!wrap.detect());
  AppleGmux ok(io, kBase, AppleGmux::kMinIoLength);
  CHECK(ok.detect() && ok.layout() == AppleGmuxLayout::Classic);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// _CRS and the ACPI namespace

const Bytes kIoCrs = {0x47, 0x01, 0x00, 0x07, 0x00, 0x07, 0x01, 0xFF, 0x79, 0x00};      // IO (Decode16, 0x700, 0x700, 1, 0xFF)

void test_resources() {
  AppleGmuxResource r;
  CHECK(apple_gmux_parse_resources(ConstByteView(kIoCrs.data(), kIoCrs.size()), &r));
  CHECK(r.has_io && r.io_base == 0x700 && r.io_length == 0xFF && !r.has_memory);
  // fixed I/O
  const Bytes fixed = {0x4B, 0x20, 0x03, 0x80, 0x79, 0x00};
  CHECK(apple_gmux_parse_resources(ConstByteView(fixed.data(), fixed.size()), &r) && r.io_base == 0x320 && r.io_length == 0x80);
  // a Word I/O address space
  const Bytes word = {0x88, 0x0D, 0x00, 0x01, 0x0C, 0x03, 0, 0, 0x00, 0x07, 0xFF, 0x07, 0, 0, 0x00, 0x01, 0x79, 0x00};
  CHECK(apple_gmux_parse_resources(ConstByteView(word.data(), word.size()), &r) && r.io_base == 0x700 && r.io_length == 0x100);
  // the T2 gmux's memory range: reported, no I/O
  const Bytes mem = {0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x10, 0x00, 0x00, 0x79, 0x00};
  CHECK(apple_gmux_parse_resources(ConstByteView(mem.data(), mem.size()), &r) && !r.has_io && r.has_memory && r.memory_base == 0xFE000000 && r.memory_length == 0x1000);
  // an I/O range running past port 0xFFFF is not taken
  const Bytes past = {0x47, 0x01, 0xF0, 0xFF, 0xF0, 0xFF, 0x01, 0x20, 0x79, 0x00};
  CHECK(!apple_gmux_parse_resources(ConstByteView(past.data(), past.size()), &r));
  // empty, nothing but the end tag
  CHECK(!apple_gmux_parse_resources(ConstByteView(), &r));
  const Bytes end = {0x79, 0x00};
  CHECK(!apple_gmux_parse_resources(ConstByteView(end.data(), end.size()), &r));
  // every truncation, every single bit flipped: never out of bounds, and an accepted I/O range lies inside the port space
  for (const Bytes* t : {&kIoCrs, &word, &mem, &fixed}) {
    for (size_t n = 0; n < t->size(); ++n) {
      AppleGmuxResource x;
      if (apple_gmux_parse_resources(ConstByteView(t->data(), n), &x) && x.has_io) CHECK(uint32_t{x.io_base} + x.io_length <= 0x10000);
    }
    for (size_t i = 0; i < t->size() * 8; ++i) {
      Bytes f = *t;
      f[i / 8] ^= static_cast<uint8_t>(1u << (i % 8));
      AppleGmuxResource x;
      if (apple_gmux_parse_resources(ConstByteView(f.data(), f.size()), &x) && x.has_io) CHECK(uint32_t{x.io_base} + x.io_length <= 0x10000);
    }
  }
}

Bytes str(const char* s) {
  Bytes r = {0x0D};
  while (*s) r.push_back(static_cast<uint8_t>(*s++));
  r.push_back(0);
  return r;
}
Bytes buffer(const Bytes& bytes) { return cat({b({0x11}), pkg(cat({num(bytes.size()), bytes}))}); }

Bytes gmux_dsdt(bool with_sta, uint8_t sta) {
  Bytes body = cat({name_def("_HID", str("APP000B")), name_def("_CRS", buffer(kIoCrs))});
  if (with_sta) body = cat({body, method("_STA", 0, cat({b({0xA4}), num(sta)}))});
  const Bytes other = device("EC0_", name_def("_HID", str("PNP0C09")));
  return make_table("DSDT", scope("\\_SB", device("PCI0", device("LPCB", cat({other, device("GMUX", body)})))));
}

void test_acpi() {
  TestAllocator allocator;
  FakeHost host;
  {
    AmlInterpreter vm(allocator, host);
    const Bytes dsdt = gmux_dsdt(false, 0);
    CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
    Node* device = AppleGmux::find_device(vm);
    CHECK(device && device == vm.find("\\_SB.PCI0.LPCB.GMUX"));
    AppleGmuxResource r;
    CHECK(AppleGmux::find_resource(vm, device, &r) && r.has_io && r.io_base == 0x700 && r.io_length == 0xFF);
    CHECK(!AppleGmux::find_resource(vm, nullptr, &r));
    CHECK(!AppleGmux::find_resource(vm, vm.find("\\_SB.PCI0.LPCB.EC0_"), &r));   // (no _CRS of its own; the gmux's is not looked at)
    // and the window it names drives a gmux
    FakeGmuxPorts io(FakeGmuxPorts::Mode::Indexed);
    model_registers(io);
    io.window = r.io_length;
    AppleGmux gmux(io, r.io_base, r.io_length);
    CHECK(gmux.detect() && gmux.layout() == AppleGmuxLayout::Indexed && io.outside == 0);
  }
  {
    AmlInterpreter vm(allocator, host);
    const Bytes dsdt = gmux_dsdt(true, 0x0F);
    CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
    CHECK(AppleGmux::find_device(vm) != nullptr);
  }
  {
    AmlInterpreter vm(allocator, host);
    const Bytes dsdt = gmux_dsdt(true, 0x00);                // _STA: not present
    CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
    CHECK(AppleGmux::find_device(vm) == nullptr);
  }
  {
    AmlInterpreter vm(allocator, host);                        // a namespace without a gmux
    const Bytes dsdt = make_table("DSDT", scope("\\_SB", device("EC0_", name_def("_HID", str("PNP0C09")))));
    CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
    CHECK(AppleGmux::find_device(vm) == nullptr);
  }
}

}  // namespace

int main() {
  test_layout(FakeGmuxPorts::Mode::Classic);
  test_layout(FakeGmuxPorts::Mode::Indexed);
  test_indexed_handshake();
  test_absent();
  test_resources();
  test_acpi();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AppleGmuxTest: ok\n");
  return 0;
}
