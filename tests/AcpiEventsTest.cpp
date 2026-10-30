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

// Host-side tests for the ACPI events: a pretend chipset (FADT, PM1 control, GPE registers) and a DSDT assembled byte by byte with a root bridge, a root port, the Thunderbolt controller behind it
// (with a _PRW) and the _L/_E methods of the events.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <map>
#include <vector>
#include "Hardware/Acpi/AcpiEvents.hpp"

using namespace acpi;
using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct FakeHost final : IAcpiHost {
  void configure_ec(uint16_t, uint16_t) override {}
  bool region_read(const RegionAccess&, uint64_t*) override { return false; }
  bool region_write(const RegionAccess&, uint64_t) override { return false; }
};

// The chipset: I/O ports as a map; the GPE status registers clear by writing ones; SMI_CMD with ACPI_ENABLE sets SCI_EN.
struct FakePorts final : IAcpiPorts {
  static constexpr uint16_t kPm1Control = 0x1804, kSmiCommand = 0xB2, kGpe0 = 0x1860;
  static constexpr uint32_t kGpe0Registers = 16;
  std::map<uint16_t, uint8_t> reg;
  bool scien_follows_smi = true;
  uint32_t smi_writes = 0, delays = 0;
  uint8_t in8(uint16_t port) override { return reg[port]; }
  void out8(uint16_t port, uint8_t value) override {
    if (port == kSmiCommand) {
      ++smi_writes;
      if (value == 0xA0 && scien_follows_smi) reg[kPm1Control] |= 1;
      return;
    }
    if (port >= kGpe0 && port < kGpe0 + kGpe0Registers) { reg[port] &= static_cast<uint8_t>(~value); return; }   // status: write one to clear
    reg[port] = value;
  }
  uint16_t in16(uint16_t port) override { return static_cast<uint16_t>(reg[port] | (reg[port + 1] << 8)); }
  void out16(uint16_t port, uint16_t value) override { reg[port] = static_cast<uint8_t>(value); reg[port + 1] = static_cast<uint8_t>(value >> 8); }
  void delay_us(uint32_t) override { ++delays; }
  void raise(uint32_t gpe) { reg[kGpe0 + gpe / 8] |= static_cast<uint8_t>(1u << (gpe % 8)); }
  bool status(uint32_t gpe) { return reg[kGpe0 + gpe / 8] & (1u << (gpe % 8)); }
  bool enabled(uint32_t gpe) { return reg[kGpe0 + kGpe0Registers + gpe / 8] & (1u << (gpe % 8)); }
};

// The PCI configuration space: a root port at 00:1c.4 (a bridge with secondary bus 5) and the controller at 05:00.0.
struct FakePci final : IPciBus {
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    if (a.bus == 0 && a.device == 0x1C && a.function == 4) {
      if (offset == 0x0C) return 1u << 16;
      if (offset == 0x18) return (5u << 8) | 0;
    }
    return 0;
  }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

typedef std::vector<uint8_t> Bytes;
static Bytes cat(std::initializer_list<Bytes> parts) { Bytes r; for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end()); return r; }
static Bytes b(std::initializer_list<uint8_t> v) { return Bytes(v); }
static Bytes seg(const char* s) { Bytes r(4, '_'); for (int i = 0; i < 4 && s[i]; ++i) r[i] = static_cast<uint8_t>(s[i]); return r; }
static Bytes path(const char* s) {
  Bytes r;
  if (*s == '\\') { r.push_back(0x5C); ++s; }
  std::vector<Bytes> segs;
  while (*s) { char t[5] = {}; int n = 0; while (*s && *s != '.') t[n++] = *s++; segs.push_back(seg(t)); if (*s == '.') ++s; }
  if (segs.size() == 2) r.push_back(0x2E);
  else if (segs.size() > 2) { r.push_back(0x2F); r.push_back(static_cast<uint8_t>(segs.size())); }
  for (const Bytes& sg : segs) r.insert(r.end(), sg.begin(), sg.end());
  return r;
}
static Bytes pkg(const Bytes& body) {
  Bytes r;
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (n + 1 < 64) r.push_back(static_cast<uint8_t>(n + 1));
  else { const uint32_t t = n + 2; r.push_back(static_cast<uint8_t>(0x40 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); }
  r.insert(r.end(), body.begin(), body.end());
  return r;
}
static Bytes num(uint64_t v) {
  if (v == 0) return b({0x00});
  if (v == 1) return b({0x01});
  if (v <= 0xFF) return b({0x0A, static_cast<uint8_t>(v)});
  if (v <= 0xFFFF) return b({0x0B, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
  return b({0x0C, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
}
static Bytes name_def(const char* n, const Bytes& value) { return cat({b({0x08}), path(n), value}); }
static Bytes method(const char* n, int argc, const Bytes& body) { return cat({b({0x14}), pkg(cat({path(n), b({static_cast<uint8_t>(argc)}), body}))}); }
static Bytes device(const char* n, const Bytes& body) { return cat({b({0x5B, 0x82}), pkg(cat({path(n), body}))}); }
static Bytes scope(const char* n, const Bytes& body) { return cat({b({0x10}), pkg(cat({path(n), body}))}); }
static Bytes package(uint8_t count, const Bytes& elements) { return cat({b({0x12}), pkg(cat({b({count}), elements}))}); }
static Bytes store(const Bytes& src, const Bytes& dst) { return cat({b({0x70}), src, dst}); }
static Bytes notify(const char* target, uint8_t value) { return cat({b({0x86}), path(target), num(value)}); }
static Bytes make_table(const char* signature, const Bytes& aml) {
  Bytes t(36, 0);
  memcpy(&t[0], signature, 4);
  const uint32_t length = static_cast<uint32_t>(36 + aml.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  t[8] = 2;
  t.insert(t.end(), aml.begin(), aml.end());
  return t;
}
static void put32(Bytes& t, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) t[at + i] = static_cast<uint8_t>(v >> (8 * i)); }

static Bytes make_fadt() {
  Bytes t(244, 0);
  memcpy(&t[0], "FACP", 4);
  put32(t, 4, 244);
  t[8] = 5;
  t[46] = 9;                                     // SCI_INT
  put32(t, 48, FakePorts::kSmiCommand);
  t[52] = 0xA0;                                  // ACPI_ENABLE
  t[53] = 0xA1;
  put32(t, 64, FakePorts::kPm1Control);
  put32(t, 80, FakePorts::kGpe0);
  t[92] = FakePorts::kGpe0Registers * 2;         // GPE0_BLK_LEN
  return t;
}

static Bytes make_dsdt() {
  const Bytes nhi = device("NHI0", cat({name_def("_ADR", num(0)), name_def("_PRW", package(2, cat({num(0x6D), num(3)}))),
                                        method("_PSW", 1, store(b({0x68}), path("\\_SB.PSWV")))}));
  const Bytes second = device("NHI1", cat({name_def("_ADR", num(1)), name_def("_PRW", package(2, cat({package(2, cat({path("\\_SB"), num(0x10)})), num(4)})))}));
  const Bytes plain = device("DEV2", name_def("_ADR", num(2)));
  const Bytes far = device("NHI2", cat({name_def("_ADR", num(3)), name_def("_PRW", package(2, cat({num(0x1FF), num(3)})))}));   // an event outside the register block
  const Bytes port = device("RP05", cat({name_def("_ADR", num(0x1C0004)), nhi, second, plain, far}));
  const Bytes pci = device("PCI0", cat({name_def("_HID", num(0x080AD041)), name_def("_BBN", num(0)), port}));
  return cat({scope("\\_SB", cat({name_def("PSWV", num(0xFF)), pci})),
              scope("\\_GPE", cat({method("_L6D", 0, notify("\\_SB.PCI0.RP05", 2)), method("_E10", 0, notify("\\_SB.PCI0.RP05", 0x80))}))});
}

struct Listener final : IAcpiNotifyListener {
  std::vector<std::pair<Node*, uint32_t>> seen;
  void on_notify(Node* device, uint32_t value) override { seen.push_back({device, value}); }
};

static void test_fadt() {
  const Bytes fadt = make_fadt();
  AcpiFadt f;
  CHECK(acpi_parse_fadt(ConstByteView(fadt.data(), fadt.size()), &f));
  CHECK(f.sci_interrupt == 9 && f.smi_command == FakePorts::kSmiCommand && f.acpi_enable == 0xA0 && f.acpi_disable == 0xA1 && f.pm1a_control == FakePorts::kPm1Control);
  CHECK(f.gpe0_block == FakePorts::kGpe0 && f.gpe0_length == 32 && f.gpe1_block == 0 && f.gpe1_length == 0);
  for (size_t cut = 0; cut < 96; cut += 5) CHECK(!acpi_parse_fadt(ConstByteView(fadt.data(), cut), &f));            // truncated
  Bytes wrong = fadt;
  wrong[0] = 'X';
  CHECK(!acpi_parse_fadt(ConstByteView(wrong.data(), wrong.size()), &f));
  Bytes wide = fadt;
  put32(wide, 80, 0x10000);                                                                                        // a block at an address that is not a port
  CHECK(!acpi_parse_fadt(ConstByteView(wide.data(), wide.size()), &f));
  for (size_t bit = 0; bit < 96 * 8; bit += 3) {                                                                   // bit flips: never read outside
    Bytes flipped = fadt;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    acpi_parse_fadt(ConstByteView(flipped.data(), flipped.size()), &f);
  }
}

static void test_events() {
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  FakePci pci;
  const Bytes dsdt = make_table("DSDT", make_dsdt());
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  {   // nothing to wait for: no events, no hardware touched
    AcpiEvents none(host, ports, alloc);
    Bytes no_gpe = fadt;
    put32(no_gpe, 80, 0);
    CHECK(!none.initialize(tables, 1, ConstByteView(no_gpe.data(), no_gpe.size())) && !none.enable_acpi_mode() && none.poll() == 0 && ports.reg.empty());
    AcpiEvents nofadt(host, ports, alloc);
    CHECK(!nofadt.initialize(tables, 1, ConstByteView()));
  }
  AcpiEvents events(host, ports, alloc);
  Listener listener;
  events.set_listener(&listener);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())) && events.object_count() > 10);
  // ACPI mode: turned on through SMI_CMD, not when the firmware did it already
  CHECK(!events.acpi_mode() && events.enable_acpi_mode() && events.acpi_mode() && ports.smi_writes == 1);
  CHECK(events.enable_acpi_mode() && ports.smi_writes == 1);
  {
    FakePorts stuck;
    stuck.scien_follows_smi = false;
    AcpiEvents e(host, stuck, alloc);
    CHECK(e.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())) && !e.enable_acpi_mode() && stuck.smi_writes == 1 && stuck.delays >= 2000);
  }
  // finding the controller: root bridge, the port below it (by its secondary bus), the controller on bus 5
  Node* port_node = events.find_pci_device(pci, {0, 0x1C, 4});
  Node* nhi = events.find_pci_device(pci, {5, 0, 0});
  Node* second = events.find_pci_device(pci, {5, 0, 1});
  CHECK(port_node && nhi && second && port_node != nhi && events.find_pci_device(pci, {5, 0, 2}) && !events.find_pci_device(pci, {5, 1, 0}) && !events.find_pci_device(pci, {6, 0, 0}));
  CHECK(events.is_within(port_node, nhi) && events.is_within(nhi, nhi) && !events.is_within(nhi, port_node));
  // _PRW in both forms
  AcpiEvents::WakeSource source;
  CHECK(events.wake_source(nhi, &source) && source.gpe == 0x6D && source.deepest_state == 3);
  CHECK(events.wake_source(second, &source) && source.gpe == 0x10 && source.deepest_state == 4);
  CHECK(!events.wake_source(events.find_pci_device(pci, {5, 0, 2}), &source) && !events.wake_source(nullptr, &source));
  // arming: stale status cleared, the enable bit set, _PSW(1) run; an event outside the registers is refused
  ports.raise(0x6D);
  CHECK(events.enable_wake(nhi) && ports.enabled(0x6D) && !ports.status(0x6D) && events.wake_count() == 1);
  uint64_t psw = 0;
  CHECK(events.interpreter().evaluate_integer("\\_SB.PSWV", &psw) && psw == 1);
  CHECK(events.enable_wake(nhi) && events.wake_count() == 1);                                                        // (again: nothing new)
  CHECK(!events.enable_wake(events.find_pci_device(pci, {5, 0, 3})) && !events.enable_wake(events.find_pci_device(pci, {5, 0, 2})));
  // nothing pending: nothing happens
  CHECK(events.poll() == 0 && listener.seen.empty());
  // the PME: the level method runs, tells the root port, the status is cleared and the event armed again
  ports.raise(0x6D);
  CHECK(events.poll() == 1 && listener.seen.size() == 1 && listener.seen[0].first == port_node && listener.seen[0].second == 2);
  CHECK(!ports.status(0x6D) && ports.enabled(0x6D) && events.events_handled() == 1);
  CHECK(events.poll() == 0);
  // an edge event (_E10) with its own device number
  CHECK(events.enable_wake(second) && ports.enabled(0x10));
  ports.raise(0x10);
  CHECK(events.poll() == 1 && listener.seen.size() == 2 && listener.seen[1].second == 0x80 && !ports.status(0x10));
  // an event that is pending but not enabled is left alone; one that is enabled and has no method is silenced
  ports.raise(0x20);
  CHECK(events.poll() == 0 && ports.status(0x20));
  ports.reg[FakePorts::kGpe0 + FakePorts::kGpe0Registers + 4] |= 1;
  CHECK(events.poll() == 1 && events.events_without_method() == 1 && !ports.enabled(0x20) && !ports.status(0x20));
  // disarming: the bit goes, _PSW(0) runs, a pending event is no longer handled
  events.disable_wake(nhi);
  CHECK(!ports.enabled(0x6D) && events.wake_count() == 1 && events.interpreter().evaluate_integer("\\_SB.PSWV", &psw) && psw == 0);
  ports.raise(0x6D);
  CHECK(events.poll() == 0);
  events.disable_wake(nhi);                                                                                          // (twice: nothing)
  events.disable_wake(second);
  CHECK(events.wake_count() == 0 && !ports.enabled(0x10));
  // the list is bounded
  for (uint32_t i = 0; i < 20; ++i) events.enable_wake(nhi);
  CHECK(events.wake_count() == 1);
}

int main() {
  test_fadt();
  test_events();
  if (g_failed) {
    printf("AcpiEventsTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("AcpiEventsTest: all passed\n");
  return 0;
}
