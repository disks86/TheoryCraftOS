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
#include "Hardware/Acpi/DellWmiHotkeys.hpp"
#include "Hardware/Backlight/AcpiBacklight.hpp"
#include "Hardware/Backlight/AcpiVideoKeys.hpp"
#include "Hardware/Backlight/IntelPchBacklight.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Hardware/Power/Thermal.hpp"
#include "Hardware/Sensor/AcpiAmbientLight.hpp"
#include "Hardware/Sensor/SensorHub.hpp"

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
  uint16_t ec_data = 0, ec_command = 0;
  std::vector<uint8_t> ec_events;          // the events the controller holds, oldest first
  bool ec_stuck = false;                   // it always claims an event, and says it is number 0x77
  uint32_t ec_queries = 0;
  void configure_ec(uint16_t data, uint16_t command) override { ec_data = data; ec_command = command; }
  bool ec_status(uint8_t* value) override { *value = (!ec_events.empty() || ec_stuck) ? 0x20 : 0; return true; }
  bool ec_query(uint8_t* event) override {
    ++ec_queries;
    if (ec_stuck) { *event = 0x77; return true; }
    *event = ec_events.empty() ? 0 : ec_events.front();
    if (!ec_events.empty()) ec_events.erase(ec_events.begin());
    return true;
  }
  bool region_read(const RegionAccess&, uint64_t*) override { return false; }
  bool region_write(const RegionAccess&, uint64_t) override { return false; }
};

// The chipset: I/O ports as a map; the GPE status registers clear by writing ones; SMI_CMD with ACPI_ENABLE sets SCI_EN.
struct FakePorts final : IAcpiPorts {
  static constexpr uint16_t kPm1Event = 0x1800, kPm1Control = 0x1804, kSmiCommand = 0xB2, kGpe0 = 0x1860;
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
  void out16(uint16_t port, uint16_t value) override {
    if (port == kPm1Event) {                           // PM1 status: write one to clear
      reg[port] &= static_cast<uint8_t>(~value);
      reg[port + 1] &= static_cast<uint8_t>(~(value >> 8));
      return;
    }
    reg[port] = static_cast<uint8_t>(value);
    reg[port + 1] = static_cast<uint8_t>(value >> 8);
  }
  void delay_us(uint32_t) override { ++delays; }
  // memory registers: a block of GPE status/enable bytes (status ones clear when written) at kMemGpe, and PM1 at kMemPm1 as 16 bit registers
  static constexpr uint64_t kMemGpe = 0x80000000ull, kMemPm1 = 0x80001000ull;
  std::map<uint64_t, uint8_t> mem;
  uint8_t mem_in8(uint64_t a) override { return mem[a]; }
  void mem_out8(uint64_t a, uint8_t v) override {
    if (a >= kMemGpe && a < kMemGpe + kGpe0Registers) mem[a] &= static_cast<uint8_t>(~v);
    else mem[a] = v;
  }
  uint16_t mem_in16(uint64_t a) override { return static_cast<uint16_t>(mem[a] | (mem[a + 1] << 8)); }
  void mem_out16(uint64_t a, uint16_t v) override {
    if (a == kMemPm1) { mem[a] &= static_cast<uint8_t>(~v); mem[a + 1] &= static_cast<uint8_t>(~(v >> 8)); return; }
    mem[a] = static_cast<uint8_t>(v);
    mem[a + 1] = static_cast<uint8_t>(v >> 8);
  }
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
  put32(t, 56, FakePorts::kPm1Event);
  t[88] = 4;                                     // PM1_EVT_LEN
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
  const Bytes ec = device("EC0", cat({name_def("_HID", num(0x090CD041)), name_def("_GPE", num(0x23)),
                                      method("_Q50", 0, notify("\\_SB.LID0", 0x80)),
                                      method("_Q51", 0, notify("\\_SB.PWRB", 0x80)),
                                      method("_Q52", 0, cat({store(num(0), path("\\_SB.LIDV")), notify("\\_SB.LID0", 0x80)}))}));
  const Bytes lid = device("LID0", cat({name_def("_HID", num(0x0D0CD041)), method("_LID", 0, cat({b({0xA4}), path("\\_SB.LIDV")}))}));
  const Bytes power = device("PWRB", name_def("_HID", num(0x0C0CD041)));
  return cat({scope("\\_SB", cat({name_def("PSWV", num(0xFF)), name_def("LIDV", num(1)), pci, ec, lid, power})),
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
  CHECK(f.sci_interrupt == 9 && f.smi_command == FakePorts::kSmiCommand && f.acpi_enable == 0xA0 && f.acpi_disable == 0xA1 && f.pm1a_control.address == FakePorts::kPm1Control && !f.pm1a_control.memory);
  CHECK(f.gpe0_block.address == FakePorts::kGpe0 && f.gpe0_length == 32 && f.gpe1_block.address == 0 && f.gpe1_length == 0);
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

struct ButtonListener final : IAcpiButtonListener {
  std::vector<std::pair<AcpiButton, bool>> seen;
  void on_button(AcpiButton button, bool open) override { seen.push_back({button, open}); }
};

static void put64(Bytes& t, size_t at, uint64_t v) { put32(t, at, static_cast<uint32_t>(v)); put32(t, at + 4, static_cast<uint32_t>(v >> 32)); }

// A FADT revision 5 that describes its PM1 event and GPE0 blocks as memory (generic address structures at 148 and 220) and PM1a_CNT as a port.
static Bytes make_memory_fadt() {
  Bytes t = make_fadt();
  put32(t, 56, 0);
  put32(t, 80, 0);
  t[148] = 0; put64(t, 152, FakePorts::kMemPm1);          // X_PM1a_EVT_BLK: memory
  t[220] = 0; put64(t, 224, FakePorts::kMemGpe);          // X_GPE0_BLK: memory
  return t;
}

static void test_memory_blocks() {
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  const Bytes dsdt = make_table("DSDT", make_dsdt());
  const Bytes fadt = make_memory_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  AcpiFadt parsed;
  CHECK(acpi_parse_fadt(ConstByteView(fadt.data(), fadt.size()), &parsed) && parsed.gpe0_block.memory && parsed.gpe0_block.address == FakePorts::kMemGpe && parsed.pm1a_event.memory && !parsed.pm1a_control.memory);
  // an extended field that names a port, or nothing, leaves the old field; a space that is neither is ignored
  Bytes port_form = make_fadt();
  port_form[220] = 1; put64(port_form, 224, 0x1900);
  CHECK(acpi_parse_fadt(ConstByteView(port_form.data(), port_form.size()), &parsed) && !parsed.gpe0_block.memory && parsed.gpe0_block.address == 0x1900);
  Bytes odd = make_fadt();
  odd[220] = 7; put64(odd, 224, 0x1234);
  CHECK(acpi_parse_fadt(ConstByteView(odd.data(), odd.size()), &parsed) && parsed.gpe0_block.address == FakePorts::kGpe0);
  AcpiEvents events(host, ports, alloc);
  Listener listener;
  events.set_listener(&listener);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())));
  FakePci pci;
  Node* port_node = events.find_pci_device(pci, {0, 0x1C, 4});
  Node* nhi = events.find_pci_device(pci, {5, 0, 0});
  CHECK(port_node && nhi && events.enable_wake(nhi));
  CHECK((ports.mem[FakePorts::kMemGpe + 16 + 13] & (1u << 5)) && ports.reg.find(FakePorts::kGpe0 + 16 + 13) == ports.reg.end());       // armed in the memory block, not in the port one
  ports.mem[FakePorts::kMemGpe + 13] |= 1u << 5;                                                                                        // GPE 0x6D
  CHECK(events.poll() == 1 && listener.seen.size() == 1 && listener.seen[0].first == port_node && !(ports.mem[FakePorts::kMemGpe + 13] & (1u << 5)));
  // PM1 in memory: the power button, the RTC alarm (only once asked for), and the bits that are just cleared
  ButtonListener buttons;
  events.set_button_listener(&buttons);
  CHECK(events.enable_buttons() >= 2);
  ports.mem[FakePorts::kMemPm1 + 1] |= 1;                                                                                                // PWRBTN_STS
  events.poll();
  CHECK(buttons.seen.size() == 1 && buttons.seen[0].first == AcpiButton::Power && !(ports.mem[FakePorts::kMemPm1 + 1] & 1));
  ports.mem[FakePorts::kMemPm1 + 1] |= 4;                                                                                                // RTC_STS (bit 10), not enabled
  events.poll();
  CHECK(events.rtc_alarms() == 0 && (ports.mem[FakePorts::kMemPm1 + 1] & 4));
  events.enable_rtc_event(true);
  CHECK(!(ports.mem[FakePorts::kMemPm1 + 1] & 4));                                                                                       // (the stale status was cleared)
  ports.mem[FakePorts::kMemPm1 + 1] |= 4;
  events.poll();
  CHECK(events.rtc_alarms() == 1 && !(ports.mem[FakePorts::kMemPm1 + 1] & 4));
  events.enable_rtc_event(false);
  ports.mem[FakePorts::kMemPm1 + 1] |= 4;
  events.poll();
  CHECK(events.rtc_alarms() == 1);
  ports.mem[FakePorts::kMemPm1] |= 0x20;                                                                                                 // GBL_STS (bit 5)
  ports.mem[FakePorts::kMemPm1 + 1] |= 0x80;                                                                                             // WAK_STS (bit 15)
  events.poll();
  CHECK(events.pm1_other() == 1 && !(ports.mem[FakePorts::kMemPm1] & 0x20) && !(ports.mem[FakePorts::kMemPm1 + 1] & 0x80));
}


static void test_buttons() {
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  const Bytes dsdt = make_table("DSDT", make_dsdt());
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  AcpiEvents events(host, ports, alloc);
  ButtonListener listener;
  events.set_button_listener(&listener);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())));
  // the controller: ports from the tables' defaults, its event 0x23 armed
  CHECK(!events.embedded_controller_armed() && events.enable_embedded_controller() && events.embedded_controller_armed());
  CHECK(host.ec_data == 0x62 && host.ec_command == 0x66 && ports.enabled(0x23));
  CHECK(events.enable_embedded_controller());                                                                        // (again: nothing)
  // two devices (lid, power button) and the two fixed buttons
  CHECK(events.enable_buttons() == 4);
  bool open = false;
  CHECK(events.lid_open(&open) && open);
  // an EC event: the controller holds 0x50, the line goes up; _Q50 says Notify(lid, 0x80); the lid is open
  host.ec_events = {0x50};
  ports.raise(0x23);
  CHECK(events.poll() == 1 && events.embedded_events() == 1 && listener.seen.size() == 1);
  CHECK(listener.seen[0].first == AcpiButton::Lid && listener.seen[0].second && !ports.status(0x23) && ports.enabled(0x23));
  // the lid closes (the method changes the state and tells), the power button is pressed: two events in one go
  host.ec_events = {0x52, 0x51};
  ports.raise(0x23);
  CHECK(events.poll() == 1 && events.embedded_events() == 3 && listener.seen.size() == 3);
  CHECK(listener.seen[1].first == AcpiButton::Lid && !listener.seen[1].second && listener.seen[2].first == AcpiButton::Power);
  CHECK(events.lid_open(&open) && !open);
  // an event with no method is counted and ignored; a controller that never stops claiming events is asked a bounded number of times
  host.ec_events = {0x99};
  ports.raise(0x23);
  CHECK(events.poll() == 1 && events.embedded_events() == 4 && listener.seen.size() == 3);
  host.ec_stuck = true;
  host.ec_queries = 0;
  ports.raise(0x23);
  CHECK(events.poll() == 1 && host.ec_queries <= 16 && ports.enabled(0x23));
  host.ec_stuck = false;
  // the fixed buttons: the status bit is read, cleared and reported
  ports.reg[FakePorts::kPm1Event + 1] |= 1;                                                                           // PWRBTN_STS (bit 8)
  CHECK(events.poll() == 0 && listener.seen.size() == 4 && listener.seen[3].first == AcpiButton::Power && !(ports.reg[FakePorts::kPm1Event + 1] & 1));
  ports.reg[FakePorts::kPm1Event + 1] |= 2;                                                                           // SLPBTN_STS (bit 9)
  events.poll();
  CHECK(listener.seen.size() == 5 && listener.seen[4].first == AcpiButton::Sleep);
  {   // buttons that are devices: the FADT says so, nothing is enabled in PM1
    FakePorts other_ports;
    Bytes method_fadt = fadt;
    put32(method_fadt, 112, 0x30);
    AcpiEvents e(host, other_ports, alloc);
    ButtonListener l;
    e.set_button_listener(&l);
    CHECK(e.initialize(tables, 1, ConstByteView(method_fadt.data(), method_fadt.size())) && e.enable_buttons() == 2);
    other_ports.reg[FakePorts::kPm1Event + 1] |= 1;
    e.poll();
    CHECK(l.seen.empty() && (other_ports.reg[FakePorts::kPm1Event + 1] & 1));
  }
  {   // no controller device
    TestAllocator a2;
    FakePorts p2;
    const Bytes plain = make_table("DSDT", scope("\\_SB", device("DEV0", name_def("_ADR", num(0)))));
    const ConstByteView t2[1] = {ConstByteView(plain.data(), plain.size())};
    AcpiEvents e(host, p2, a2);
    CHECK(e.initialize(t2, 1, ConstByteView(fadt.data(), fadt.size())) && !e.enable_embedded_controller() && e.enable_buttons() == 2 && !e.lid_open(&open));
  }
}


// A panel with the video extensions: _BCL, _BCM (stores the level) and _BQC; a second display without _BCM that must not be taken; GPE 0x10 notifies "brighter".
static Bytes make_video_dsdt(bool with_query) {
  const Bytes levels = package(9, cat({num(80), num(40), num(10), num(20), num(40), num(60), num(80), num(100), num(20)}));   // (unsorted, one twice)
  Bytes lcd_body = cat({name_def("_ADR", num(0x400)), name_def("_BCL", levels), method("_BCM", 1, store(b({0x68}), path("\\_SB.BRTV")))});
  if (with_query) lcd_body = cat({lcd_body, method("_BQC", 0, cat({b({0xA4}), path("\\_SB.BRTV")}))});
  const Bytes lcd = device("LCD0", lcd_body);
  const Bytes crt = device("CRT0", cat({name_def("_ADR", num(0x100)), name_def("_BCL", levels)}));
  const Bytes video = device("VID0", cat({name_def("_HID", num(0x0A0AD041)), lcd, crt}));
  const Bytes ec = device("EC0", cat({name_def("_HID", num(0x090CD041)), name_def("_GPE", num(0x23)), method("_Q60", 0, notify("\\_SB.VID0.LCD0", 0x86))}));   // (the keys come through the controller)
  return cat({scope("\\_SB", cat({name_def("BRTV", num(40)), video, ec}))});
}

struct StepCounter {
  int up = 0, down = 0;
  static void step(void* self, bool brighter) { auto* c = static_cast<StepCounter*>(self); (brighter ? c->up : c->down)++; }
};

struct FakeBank final : intel_gt::IRegisterBank {
  std::map<uint64_t, uint32_t> r;
  uint32_t read32(uint64_t o) override { return r[o]; }
  void write32(uint64_t o, uint32_t v) override { r[o] = v; }
};

static void test_backlight() {
  for (bool with_query : {true, false}) {
    TestAllocator alloc;
    FakeHost host;
    FakePorts ports;
    const Bytes dsdt = make_table("DSDT", make_video_dsdt(with_query));
    const Bytes fadt = make_fadt();
    const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
    AcpiEvents events(host, ports, alloc);
    CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())) && events.enable_acpi_mode());
    Node* found[4];
    CHECK(AcpiBacklight::find_devices(events.interpreter(), found, 4) == 1);                  // (the display without _BCM is not a backlight)
    AcpiBacklight light(events.interpreter(), found[0]);
    CHECK(light.initialize() && light.steps() == 6);
    uint8_t now = 0;
    if (with_query) {
      CHECK(light.percent(&now) && now == 33);                                               // level 40 of 10..100
    } else {
      CHECK(!light.percent(&now));                                                           // nobody told us yet
    }
    CHECK(light.set_percent(100) && light.percent(&now) && now == 100);
    uint64_t stored = 0;
    CHECK(events.interpreter().evaluate_integer("\\_SB.BRTV", &stored) && stored == 100);
    CHECK(light.set_percent(0) && light.percent(&now) && now == 0);
    CHECK(events.interpreter().evaluate_integer("\\_SB.BRTV", &stored) && stored == 10);
    CHECK(light.set_percent(50) && events.interpreter().evaluate_integer("\\_SB.BRTV", &stored) && stored == 60);   // nearest of 10 20 40 60 80 100 to 55
    CHECK(light.set_percent(250) && events.interpreter().evaluate_integer("\\_SB.BRTV", &stored) && stored == 100);
    // the firmware's own brightness keys
    StepCounter counter;
    AcpiVideoKeys keys(light, &StepCounter::step, &counter);
    CHECK(events.add_listener(&keys));
    CHECK(events.enable_embedded_controller());
    host.ec_events.push_back(0x60);
    ports.raise(0x23);
    events.poll();
    CHECK(counter.up == 1 && counter.down == 0);
    keys.on_notify(found[0], 0x87);
    keys.on_notify(found[0], 0x85);
    keys.on_notify(nullptr, 0x86);
    CHECK(counter.up == 1 && counter.down == 1);
  }
  {   // a table without any panel, and a list that is not a list
    TestAllocator alloc;
    FakeHost host;
    FakePorts ports;
    const Bytes bad = make_table("DSDT", scope("\\_SB", device("LCD0", cat({name_def("_BCL", num(5)), method("_BCM", 1, b({0xA4, 0x00}))}))));
    const Bytes fadt = make_fadt();
    const ConstByteView tables[1] = {ConstByteView(bad.data(), bad.size())};
    AcpiEvents events(host, ports, alloc);
    CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())));
    Node* found[2];
    const uint32_t n = AcpiBacklight::find_devices(events.interpreter(), found, 2);
    CHECK(n == 1);
    AcpiBacklight light(events.interpreter(), found[0]);
    uint8_t now;
    CHECK(!light.initialize() && !light.set_percent(10) && !light.percent(&now));
  }
  {   // the PCH PWM: refuses to start one, and scales the duty to the period the firmware chose
    FakeBank bank;
    IntelPchBacklight off(bank);
    CHECK(!off.initialize());
    bank.r[IntelPchBacklight::kControl2] = (1000u << 16) | 500;
    CHECK(!off.initialize());                                                                // (not enabled)
    uint8_t now;
    CHECK(!off.percent(&now) && !off.set_percent(10) && bank.r[IntelPchBacklight::kControl2] == ((1000u << 16) | 500));
    bank.r[IntelPchBacklight::kControl1] = IntelPchBacklight::kEnable;
    IntelPchBacklight on(bank);
    CHECK(on.initialize() && on.steps() == 256 && on.percent(&now) && now == 50);
    CHECK(on.set_percent(25) && bank.r[IntelPchBacklight::kControl2] == ((1000u << 16) | 250) && on.percent(&now) && now == 25);
    CHECK(on.set_percent(200) && (bank.r[IntelPchBacklight::kControl2] & 0xFFFF) == 1000);
    bank.r[IntelPchBacklight::kControl2] = (0u << 16) | 5;
    IntelPchBacklight noperiod(bank);
    CHECK(!noperiod.initialize());
  }
}

static Bytes words(std::initializer_list<uint16_t> w) { Bytes r; for (uint16_t v : w) { r.push_back(static_cast<uint8_t>(v)); r.push_back(static_cast<uint8_t>(v >> 8)); } return r; }
static Bytes buffer_obj(const Bytes& data) { return cat({b({0x11}), pkg(cat({num(data.size()), data}))}); }

static void test_dell_wmi() {
  namespace K = hardware::key;
  uint8_t keys[8];
  // type 0: one code; type 0x10: every code of the entry; the unknown ones are skipped
  CHECK(dell_wmi_decode(ConstByteView(words({2, 0, 0x109}).data(), 6), keys, 8) == 1 && keys[0] == K::kMute);
  const Bytes two = words({3, 0x10, 0x57, 0x150, 2, 0, 0xE006});
  CHECK(dell_wmi_decode(ConstByteView(two.data(), two.size()), keys, 8) == 3 && keys[0] == K::kBrightnessDown && keys[1] == kDellMicMute && keys[2] == K::kBrightnessUp);
  CHECK(dell_wmi_decode(ConstByteView(two.data(), two.size()), keys, 2) == 2);                      // (the capacity)
  const Bytes unknown = words({2, 0, 0xE0F0, 0, 0});
  CHECK(dell_wmi_decode(ConstByteView(unknown.data(), unknown.size()), keys, 8) == 0);             // (a zero length stops)
  const Bytes toolong = words({9, 0x10, 0x57});
  CHECK(dell_wmi_decode(ConstByteView(toolong.data(), toolong.size()), keys, 8) == 0);             // (the entry does not fit)
  CHECK(dell_wmi_decode(ConstByteView(), keys, 8) == 0);
  for (size_t n = 0; n <= two.size(); ++n) dell_wmi_decode(ConstByteView(two.data(), n), keys, 8);   // cut anywhere
  for (size_t bit = 0; bit < two.size() * 8; ++bit) { Bytes f = two; f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8)); dell_wmi_decode(ConstByteView(f.data(), f.size()), keys, 8); }

  // through the tables: a WMI device with the event block, an event from the embedded controller
  const Bytes wdg = cat({b({0x94, 0x59, 0xBB, 0x9D, 0x97, 0xA9, 0xDA, 0x11, 0xB0, 0x12, 0xB6, 0x22, 0xA1, 0xEF, 0x54, 0x92, 0xD0, 0x00, 0x01, 0x08}),
                         b({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 'A', 'A', 1, 0})});
  const Bytes wmi = device("WMI0", cat({name_def("_HID", num(0x140CD041)), name_def("_WDG", buffer_obj(wdg)), method("_WED", 1, cat({b({0xA4}), buffer_obj(two)}))}));
  const Bytes plain = device("WMI1", cat({name_def("_HID", num(0x140CD041)), name_def("_WDG", buffer_obj(wdg.size() > 20 ? Bytes(wdg.begin() + 20, wdg.end()) : wdg))}));
  const Bytes ec = device("EC0", cat({name_def("_HID", num(0x090CD041)), name_def("_GPE", num(0x23)), method("_Q70", 0, notify("\\_SB.WMI0", 0xD0)), method("_Q71", 0, notify("\\_SB.WMI0", 0xD1))}));
  const Bytes dsdt = make_table("DSDT", scope("\\_SB", cat({wmi, plain, ec})));
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  AcpiEvents events(host, ports, alloc);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())) && events.enable_acpi_mode() && events.enable_embedded_controller());
  std::vector<uint8_t> got;
  auto sink = [](void* c, uint8_t k) { static_cast<std::vector<uint8_t>*>(c)->push_back(k); };
  DellWmiHotkeys hotkeys(events.interpreter(), sink, &got, true);
  CHECK(hotkeys.scan() == 1);                                                                       // (the second device has no event block of Dell's)
  CHECK(events.add_listener(&hotkeys));
  host.ec_events.push_back(0x70);
  ports.raise(0x23);
  events.poll();
  CHECK(got.size() == 3 && got[0] == K::kBrightnessDown && got[1] == kDellMicMute && got[2] == K::kBrightnessUp);
  got.clear();
  host.ec_events.push_back(0x71);                                                                   // (another notify id: not ours)
  ports.raise(0x23);
  events.poll();
  CHECK(got.empty());
  DellWmiHotkeys no_brightness(events.interpreter(), sink, &got, false);
  CHECK(no_brightness.scan() == 1);
  no_brightness.on_notify(events.interpreter().find("\\_SB.WMI0"), 0xD0);
  CHECK(got.size() == 1 && got[0] == kDellMicMute);
  no_brightness.on_notify(nullptr, 0xD0);
}

static Bytes str(const char* s) { Bytes r = b({0x0D}); while (*s) r.push_back(static_cast<uint8_t>(*s++)); r.push_back(0); return r; }

static void test_ambient_light() {
  const Bytes light = device("ALS0", cat({name_def("_HID", str("ACPI0008")), name_def("LUX", num(250)), method("_ALI", 0, cat({b({0xA4}), path("\\_SB.ALS0.LUX")}))}));
  const Bytes broken = device("ALS1", cat({name_def("_HID", str("ACPI0008"))}));                       // (no _ALI: not a sensor)
  const Bytes other = device("DEV3", cat({name_def("_HID", str("PNP0C0A")), method("_ALI", 0, cat({b({0xA4}), num(7)}))}));
  const Bytes dsdt = make_table("DSDT", scope("\\_SB", cat({light, broken, other})));
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  AcpiEvents events(host, ports, alloc);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())));
  Node* found[4];
  CHECK(AcpiAmbientLight::find_devices(events.interpreter(), found, 4) == 1);
  AcpiAmbientLight sensor(events.interpreter(), found[0]);
  SensorHub hub;
  SensorReading r;
  CHECK(sensor.initialize() && sensor.kind() == SensorKind::AmbientLight && hub.add(&sensor));
  CHECK(hub.read(SensorKind::AmbientLight, &r) && r.x == 250.0f && r.sequence == 1);
  CHECK(sensor.read(&r) && r.sequence == 2);
  AcpiAmbientLight none(events.interpreter(), nullptr);
  CHECK(!none.initialize());
}

static Bytes thermal_zone(const char* n, const Bytes& body) { return cat({b({0x5B, 0x85}), pkg(cat({path(n), body}))}); }

static void test_thermal_zones() {
  // 3231 = 50.0 C, 3581 = 85.0 C, 3731 = 100.0 C (tenths of a Kelvin)
  const Bytes tz0 = thermal_zone("TZ00", cat({name_def("TMPV", num(3231)), method("_TMP", 0, cat({b({0xA4}), path("TMPV")})), name_def("_PSV", num(3581)), name_def("_CRT", num(3731))}));
  const Bytes tz1 = thermal_zone("TZ01", cat({method("_TMP", 0, cat({b({0xA4}), num(0)}))}));                  // (a reading that is not a temperature)
  const Bytes no_tmp = thermal_zone("TZ02", name_def("_PSV", num(3581)));
  const Bytes dsdt = make_table("DSDT", scope("\\_TZ", cat({tz0, tz1, no_tmp})));
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  AcpiEvents events(host, ports, alloc);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())));
  Node* zones[4];
  CHECK(AcpiThermalZone::find_zones(events.interpreter(), zones, 4) == 2);
  AcpiThermalZone a(events.interpreter(), zones[0]), bad(events.interpreter(), zones[1]);
  int32_t t = 0;
  CHECK(a.read(&t) && t == 50000 && a.limits().passive_mc == 85000 && a.limits().critical_mc == 100000);
  CHECK(!bad.read(&t) && bad.limits().passive_mc == -1 && bad.limits().critical_mc == -1);
  struct Cooler : IThermalActuator { uint32_t last = 100; bool set_ceiling_percent(uint32_t p) override { last = p; return true; } } cooler;
  ThermalManager manager(&cooler);
  CHECK(manager.add(&a) && manager.add(&bad));
  manager.poll(0);
  CHECK(manager.hottest_millicelsius() == 50000 && cooler.last == 100 && !manager.critical());
}

int main() {
  test_thermal_zones();
  test_ambient_light();
  test_dell_wmi();
  test_backlight();
  test_fadt();
  test_buttons();
  test_memory_blocks();
  test_events();
  if (g_failed) {
    printf("AcpiEventsTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("AcpiEventsTest: all passed\n");
  return 0;
}
