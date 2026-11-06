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

// Host-side tests for the chipset devices: the Intel GPIO community layouts (consistency of the generated tables and the pad a GPIO number reaches),
// the PCH thermal sensor, the TCO watchdog against a register model, and the AMD nodes (SMN through the right root complex, Data Fabric indirect
// reads and revision, k10temp's temperature decoding) - including several threads sharing the SMN pair through a Synchronized.
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>
#include "Hardware/Bus/I2c/IntelGpio.hpp"
#include "Hardware/Chipset/AMD/AmdNodes.hpp"
#include "Hardware/Chipset/Intel/IntelGpioLayouts.hpp"
#include "Hardware/Chipset/Intel/IntelPchThermal.hpp"
#include "Hardware/Chipset/Intel/IntelTcoWatchdog.hpp"

using namespace hardware;

extern "C" {
void tcos_halt() {}
void tcos_pause() {}
void tcos_breakpoint() {}
void tcos_disable_interrupts() {}
void tcos_enable_interrupts() {}
uint64_t tcos_rdtsc() { return 0; }
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A window of plain memory standing in for device registers. The one cast in the test: the buffer is static, aligned and outlives every region.
template <size_t N>
static MmioRegion region_of(uint32_t (&buffer)[N]) {
  return MmioRegion(reinterpret_cast<uint64_t>(&buffer[0]), N * sizeof(uint32_t));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// GPIO layouts.

static void test_gpio_layouts() {
  CHECK(intel_gpio_layout_count() >= 40 && intel_gpio_layout_at(intel_gpio_layout_count()) == nullptr);
  for (uint32_t i = 0; i < intel_gpio_layout_count(); ++i) {
    const IntelGpioLayout& l = *intel_gpio_layout_at(i);
    CHECK(l.hid && l.name && !l.communities.empty());
    CHECK(intel_gpio_layout(l.hid, l.uid) != nullptr);
    uint32_t previous_end = 0;
    bool first = true;
    for (const IntelGpioCommunityLayout& c : l.communities) {
      CHECK(c.first_pin <= c.last_pin && !c.groups.empty() && c.bar < 8);
      CHECK(first || c.first_pin > previous_end);   // communities in pin order, no overlap
      first = false;
      previous_end = c.last_pin;
      uint32_t expected_first = c.first_pin;
      for (const IntelGpioGroup& g : c.groups) {
        CHECK(g.first_pin == expected_first && g.last_pin >= g.first_pin && g.last_pin <= c.last_pin);   // groups tile the community
        CHECK(g.last_pin - g.first_pin < 32 && g.gpio_base >= -1);
        expected_first = g.last_pin + 1u;
      }
      CHECK(expected_first == c.last_pin + 1u);
    }
    // GPIO numbers never collide inside one controller.
    for (const IntelGpioCommunityLayout& a : l.communities)
      for (const IntelGpioGroup& g : a.groups)
        for (const IntelGpioCommunityLayout& b : l.communities)
          for (const IntelGpioGroup& h : b.groups) {
            if (&g == &h || g.gpio_base < 0 || h.gpio_base < 0) continue;
            const int ga = g.gpio_base, gb = g.gpio_base + (g.last_pin - g.first_pin), ha = h.gpio_base, hb = h.gpio_base + (h.last_pin - h.first_pin);
            if (gb >= ha && hb >= ga) {
              printf("FAILED: %s %s: GPIO ranges %d-%d and %d-%d overlap\n", l.hid, l.uid ? l.uid : "", ga, gb, ha, hb);
              ++g_failed;
            }
          }
  }
  // Lookups.
  const IntelGpioLayout* spt = intel_gpio_layout("INT344B", nullptr);
  CHECK(spt && spt->communities.size() == 3 && spt->communities[1].first_pin == 48 && spt->communities[2].last_pin == 151);
  CHECK(intel_gpio_layout("INT3450", nullptr) && strcmp(intel_gpio_layout("INT3450", nullptr)->name, "Cannon Point-H") == 0);
  CHECK(intel_gpio_layout("INTC1055", "7") != nullptr);   // no _UID in the table: any matches
  CHECK(intel_gpio_layout("INT3452", nullptr) == nullptr && intel_gpio_layout("INT3452", "3") && intel_gpio_layout("INT3452", "9") == nullptr);
  CHECK(intel_gpio_layout("INT9999", nullptr) == nullptr && intel_gpio_layout(nullptr, nullptr) == nullptr);
  CHECK(intel_gpio_layout("INTC1056", nullptr)->communities.data() == intel_gpio_layout("INTC1085", nullptr)->communities.data());   // ADL-S = RPL-S
}

alignas(16) static uint32_t g_community[3][0x1000 / 4];

static void set_pad(int community, uint32_t pad_offset, uint32_t index, uint32_t stride, uint32_t value) {
  g_community[community][(pad_offset + index * stride) / 4] = value;
}

static void test_gpio_pads() {
  // Cannon Point-LP: GPP_B starts at GPIO 32 but is pad 25 of community 0; the SPI group has no GPIO numbers. REVID 0x94: 16-byte pads.
  memset(g_community, 0, sizeof g_community);
  for (int c = 0; c < 3; ++c) {
    g_community[c][0] = 0x00940000;   // REVID
    g_community[c][0x0C / 4] = 0x600; // PADBAR
  }
  const IntelGpioLayout* cnl = intel_gpio_layout("INT34BB", nullptr);
  CHECK(cnl != nullptr);
  MmioRegion bars[3] = {region_of(g_community[0]), region_of(g_community[1]), region_of(g_community[2])};
  IntelPchGpio gpio;
  CHECK(gpio.attach(*cnl, ArrayView<const MmioRegion>(bars, 3)) && gpio.community_count() == 3);
  set_pad(0, 0x600, 25, 16, 0x2);       // GPP_B0: rx high
  set_pad(0, 0x600, 24, 16, 0x400);     // GPP_A24: native function 1
  bool high = false;
  CHECK(gpio.has_pin(32) && gpio.level(32, &high) && high);
  IntelGpioPad pad;
  CHECK(gpio.pad(24, &pad) && !pad.gpio() && pad.mode() == 1);
  CHECK(!gpio.has_pin(25) && !gpio.has_pin(31));   // the gap after GPP_A (25 pins) up to GPP_B's 32
  CHECK(gpio.has_pin(64 + 7) && !gpio.has_pin(64 + 8));   // GPP_G: 8 pins at 64
  // Community 2 (GPP_C from GPIO 256) is pad 0 of the third range.
  set_pad(2, 0x600, 0, 16, 0x2);
  CHECK(gpio.level(256, &high) && high);
  set_pad(2, 0x600, 0, 16, 0x1);
  CHECK(gpio.pad(256, &pad) && pad.tx_state() && !pad.rx_state());

  // A missing range skips its communities; none at all fails.
  IntelPchGpio partial;
  MmioRegion only_first[1] = {region_of(g_community[0])};
  CHECK(partial.attach(*cnl, ArrayView<const MmioRegion>(only_first, 1)) && partial.community_count() == 1 && !partial.has_pin(256));
  IntelPchGpio none;
  CHECK(!none.attach(*cnl, ArrayView<const MmioRegion>()));
  // A PADBAR that points past the window, and an old REVID: 8-byte pads.
  g_community[1][0x0C / 4] = 0x0FF0;
  IntelPchGpio bad;
  MmioRegion second[2] = {MmioRegion(), region_of(g_community[1])};
  CHECK(!bad.attach(*cnl, ArrayView<const MmioRegion>(second, 2)));
  g_community[0][0] = 0x00100000;
  IntelPchGpio old;
  CHECK(old.add_community(region_of(g_community[0]), 0, 48));
  set_pad(0, 0x600, 3, 8, 0x2);
  CHECK(old.level(3, &high) && high);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// PCH thermal sensor.

alignas(16) static uint32_t g_thermal[0x100 / 4];

static void test_pch_thermal() {
  CHECK(IntelPchThermal::millicelsius_of(0) == -50000 && IntelPchThermal::millicelsius_of(181) == 40500 && IntelPchThermal::millicelsius_of(0xFFFF) == 205500);
  CHECK(intel_pch_thermal_device(0xA379) && intel_pch_thermal_device(0x9D31) && !intel_pch_thermal_device(0xA0FC));
  IntelPchThermal sensor;
  int32_t mc = 0;
  CHECK(!sensor.read(&mc) && sensor.limits().critical_mc == -1);
  memset(g_thermal, 0xFF, sizeof g_thermal);
  CHECK(!sensor.attach(region_of(g_thermal)));   // nothing behind the BAR
  uint32_t small[4] = {};
  CHECK(!sensor.attach(region_of(small)));
  memset(g_thermal, 0, sizeof g_thermal);
  g_thermal[0] = 190;   // 45 C
  CHECK(sensor.attach(region_of(g_thermal)) && !sensor.enabled() && !sensor.read(&mc));   // left off by the firmware
  g_thermal[2] = 0x81;  // enabled and locked
  g_thermal[4] = 0x0F0; // CTT 70 C
  CHECK(sensor.enabled() && sensor.locked() && sensor.read(&mc) && mc == 45000 && sensor.limits().critical_mc == 70000 && sensor.limits().passive_mc == -1);
  CHECK(strcmp(sensor.name(), "chipset") == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// TCO watchdog.

class TcoModel final : public ITcoPorts {
 public:
  uint16_t reload = 0x3FF, status1 = 0, status2 = 0, control1 = tco_reg::kControl1Halt | tco_reg::kControl1NoReboot, timer = 0x0032;
  bool no_reboot_locked = false;   // the firmware locked NO_REBOOT on
  bool nmi_now_toggled = false;
  uint32_t reloads = 0;

  uint16_t read16(uint8_t reg) override {
    switch (reg) {
      case tco_reg::kReload: return reload;
      case tco_reg::kStatus1: return status1;
      case tco_reg::kStatus2: return status2;
      case tco_reg::kControl1: return control1;
      case tco_reg::kTimer: return timer;
      default: return 0;
    }
  }
  void write16(uint8_t reg, uint16_t value) override {
    switch (reg) {
      case tco_reg::kReload: ++reloads; reload = timer & tco_reg::kTimerMask; break;
      case tco_reg::kStatus1: status1 &= static_cast<uint16_t>(~value); break;
      case tco_reg::kStatus2: status2 &= static_cast<uint16_t>(~value); break;
      case tco_reg::kControl1: {
        if (value & tco_reg::kControl1NmiNow) nmi_now_toggled = true;   // writing 1 toggles NMI_NOW: the driver must never do it
        uint16_t next = static_cast<uint16_t>(value & ~tco_reg::kControl1NmiNow);
        if (no_reboot_locked) next |= tco_reg::kControl1NoReboot;
        control1 = static_cast<uint16_t>(next | (control1 & tco_reg::kControl1NmiNow));
        break;
      }
      case tco_reg::kTimer: timer = value; break;
      default: break;
    }
  }
};

static void test_tco_watchdog() {
  CHECK(IntelTcoWatchdog::ticks_of(30) == 50 && IntelTcoWatchdog::seconds_of(50) == 30);
  TcoModel model;
  model.control1 |= tco_reg::kControl1NmiNow;   // NMI_NOW set: reads come back with it
  IntelTcoWatchdog w;
  CHECK(!w.attach(&model, IntelTcoKind::None) && !w.attach(nullptr, IntelTcoKind::Version6) && !w.running() && !w.start());
  CHECK(w.attach(&model, IntelTcoKind::Version6) && w.can_arm() && !w.running() && w.timeout_seconds() == 30);

  CHECK(!w.set_timeout(2) && !w.set_timeout(700) && w.set_timeout(60) && (model.timer & 0x3FF) == 100);
  CHECK(w.start() && w.running() && !(model.control1 & tco_reg::kControl1NoReboot) && model.reloads == 1);
  w.ping();
  CHECK(model.reloads == 2 && w.remaining_seconds() == 60);
  CHECK(w.stop() && !w.running() && (model.control1 & tco_reg::kControl1NoReboot));
  CHECK(!model.nmi_now_toggled);

  // The last reset was the watchdog; clearing writes 1s to the status bits.
  model.status2 = tco_reg::kStatus2SecondTimeout | tco_reg::kStatus2Boot;
  model.status1 = tco_reg::kStatus1Timeout;
  CHECK(w.caused_last_reset());
  w.clear_status();
  CHECK(!w.caused_last_reset() && model.status1 == 0 && model.status2 == tco_reg::kStatus2Boot);

  // NO_REBOOT locked by the firmware: the watchdog cannot be armed.
  model.no_reboot_locked = true;
  CHECK(!w.start() && !w.running());
  model.no_reboot_locked = false;

  // Version 4: can be halted (the firmware left it running), cannot be armed.
  TcoModel v4;
  v4.control1 = tco_reg::kControl1NoReboot;   // counting
  IntelTcoWatchdog w4;
  CHECK(w4.attach(&v4, IntelTcoKind::Version4) && !w4.can_arm() && w4.running() && !w4.start());
  CHECK(w4.stop() && !w4.running());

  // Ports that decode nothing.
  TcoModel absent;
  absent.control1 = 0xFFFF;
  CHECK(!w.attach(&absent, IntelTcoKind::Version6));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// AMD nodes.

// Root complexes with an SMN index/data pair, and Data Fabric functions with FICAA/FICAD, over a sparse SMN space.
class AmdBus final : public IPciBus {
 public:
  struct Root {
    PciAddress address;
    uint32_t index = 0;
    uint32_t node;   // which node's SMN this root reaches
  };
  std::vector<PciDeviceInfo> devices;
  mutable std::vector<Root> roots;   // the index registers change on config writes
  // SMN contents per node: address -> value (missing: all ones).
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> smn;
  // Data Fabric registers of node 0: (function, reg) -> value.
  std::vector<std::pair<uint32_t, uint32_t>> df;
  bool legacy_ficaa = true;
  mutable uint32_t ficaa[8] = {};
  mutable std::vector<std::pair<uint32_t, uint32_t>> smn_writes;

  // Interleaving detection: an index write must be followed by its data access from the same thread.
  mutable std::atomic<int> owner{-1};
  mutable std::atomic<bool> interleaved{false};
  static int thread_id() {
    static std::atomic<int> next{0};
    thread_local int id = next++;
    return id;
  }

  void add(uint8_t bus, uint8_t device, uint8_t function, uint16_t vendor, uint16_t id, uint8_t cls, uint8_t subclass) {
    devices.push_back({{bus, device, function}, vendor, id, cls, subclass, 0});
  }
  void add_root(uint8_t bus, uint32_t node) {
    add(bus, 0, 0, 0x1022, 0x14D8, 0x06, 0x00);
    roots.push_back({{bus, 0, 0}, 0, node});
  }
  void set_smn(uint32_t node, uint32_t address, uint32_t value) {
    if (smn.size() <= node) smn.resize(node + 1);
    smn[node].push_back({address, value});
  }

  const char* name() const override { return "amd"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(devices.size()); }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override {
    if (i >= devices.size()) return false;
    *out = devices[i];
    return true;
  }
  Root* root(PciAddress a) const {
    for (Root& r : roots)
      if (r.address.bus == a.bus && r.address.device == a.device && r.address.function == a.function) return &r;
    return nullptr;
  }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    if (Root* r = root(a); r && offset == AmdNodes::kSmnData) {
      if (owner.exchange(-1) != thread_id()) interleaved = true;
      if (r->node < smn.size())
        for (const auto& [address, value] : smn[r->node])
          if (address == r->index) return value;
      return 0xFFFFFFFF;
    }
    if (a.bus == 0 && a.device == 0x18 && a.function == 4 && offset == (legacy_ficaa ? 0x98 : 0xB8)) {
      const uint32_t request = ficaa[4];
      const uint32_t function = (request >> 11) & 7;
      const uint32_t reg = legacy_ficaa ? (request & 0x7FC) : ((request & 0x7FE) >> 1) << 2;
      for (const auto& [key, value] : df)
        if (key == ((function << 16) | reg)) return value;
      return 0;
    }
    return 0xFFFFFFFF;
  }
  void write32(PciAddress a, uint16_t offset, uint32_t value) const override {
    if (Root* r = root(a)) {
      if (offset == AmdNodes::kSmnIndex) {
        if (owner.exchange(thread_id()) != -1) interleaved = true;
        r->index = value;
      } else if (offset == AmdNodes::kSmnData) {
        if (owner.exchange(-1) != thread_id()) interleaved = true;
        smn_writes.push_back({r->index, value});
      }
      return;
    }
    if (a.bus == 0 && a.device == 0x18 && a.function == 4 && offset == (legacy_ficaa ? 0x5C : 0x8C)) ficaa[4] = value;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

static void test_amd_decoding() {
  // Tctl: the reading in eighths of a degree from bit 21; the -49 C range when bit 19 is set.
  CHECK(amd_tctl_millicelsius(400u << 21) == 50000 && amd_tctl_millicelsius((400u << 21) | (1u << 19)) == 1000);
  CHECK(amd_tctl_millicelsius((400u << 21) | (3u << 16)) == 1000);
  CHECK(amd_ccd_millicelsius(0x800 | 712) == 40000);
  uint32_t offset = 0, count = 0;
  CHECK(amd_ccd_layout(0x19, 0x61, &offset, &count) && offset == 0x308 && count == 8);
  CHECK(amd_ccd_layout(0x1A, 0x44, &offset, &count) && offset == 0x308 && count == 8);
  CHECK(amd_ccd_layout(0x1A, 0x11, &offset, &count) && offset == 0x1F0 && count == 16);
  CHECK(amd_ccd_layout(0x19, 0x11, &offset, &count) && offset == 0x300 && count == 12);
  CHECK(amd_ccd_layout(0x17, 0x71, &offset, &count) && offset == 0x154 && count == 8);
  CHECK(!amd_ccd_layout(0x1A, 0x24, &offset, &count) && !amd_ccd_layout(0x15, 0x30, &offset, &count));   // Strix Point: Tctl only
  CHECK(amd_df_legacy_ficaa(0x17, 0x71) && amd_df_legacy_ficaa(0x19, 0x21) && !amd_df_legacy_ficaa(0x19, 0x61) && !amd_df_legacy_ficaa(0x1A, 0x44));
  CHECK(strcmp(amd_data_fabric_revision_name(AmdDataFabricRevision::Df3p5), "DF3.5") == 0);
}

static void test_amd_nodes() {
  // A Ryzen 5000 (Zen 3, family 19h model 21h): one node, one root, two CCDs, legacy FICAA, DF3.
  AmdBus bus;
  bus.add_root(0, 0);
  bus.add(0, 0x18, 0, 0x1022, 0x1650, 0x06, 0x00);
  bus.add(0, 0x18, 4, 0x1022, 0x1654, 0x06, 0x00);
  bus.add(0, 0x14, 0, 0x1022, 0x790B, 0x0C, 0x05);
  bus.set_smn(0, 0x59800, 360u << 21);                  // Tctl 45 C
  bus.set_smn(0, 0x59800 + 0x154, 0x800 | 704);         // CCD0 39 C
  bus.set_smn(0, 0x59800 + 0x158, 0x800 | 720);         // CCD1 41 C
  bus.set_smn(0, 0x59800 + 0x15C, 704);                 // CCD2: not valid
  bus.df.push_back({(0u << 16) | 0x40, 0x0000001B});    // 27 fabric blocks, no major revision
  bus.df.push_back({(1u << 16) | 0x208, 0x00000203});   // DF3 component mask
  bus.df.push_back({(0u << 16) | 0x104, 0xC0000003});   // hole at 3 GiB, valid
  AmdNodes nodes;
  nodes.discover(bus, 0x19, 0x21);
  CHECK(nodes.node_count() == 1 && nodes.root_count() == 1 && nodes.root_of(0).bus == 0);
  uint32_t value = 0;
  CHECK(nodes.smn_read(0, 0x59800, &value) && value == (360u << 21) && !nodes.smn_read(0, 0x12345678, &value) && !nodes.smn_read(1, 0, &value));
  const AmdTemperatures t = nodes.temperatures(0);
  CHECK(t.tctl_valid && t.tctl_mc == 45000 && t.ccd_count == 2 && t.ccd_index[1] == 1 && t.ccd_mc[0] == 39000 && t.ccd_mc[1] == 41000);
  AmdDataFabricInfo df = nodes.data_fabric();
  CHECK(df.revision == AmdDataFabricRevision::Df3 && df.block_instances == 27 && df.dram_hole && df.dram_hole_base == 0xC0000000u);
  CHECK(nodes.smn_write(0, 0x1000, 7) && bus.smn_writes.size() == 1 && bus.smn_writes[0].first == 0x1000 && bus.smn_writes[0].second == 7);
  CHECK(!nodes.df_read(0, 8, 0, &value) && !nodes.df_read(0, 0, 0x41, &value) && !nodes.df_config(0, 0, 0x1000, &value));
  CHECK(nodes.df_config(0, 0, 0, &value) == false);   // the model answers no direct reads of F0

  // DF3.5 and DF2 by the mask registers.
  bus.df[1] = {(1u << 16) | 0x150, 0x00000007};
  CHECK(nodes.data_fabric().revision == AmdDataFabricRevision::Df3p5);
  bus.df[1] = {(1u << 16) | 0x999, 0};
  CHECK(nodes.data_fabric().revision == AmdDataFabricRevision::Df2);

  // Zen 4 (family 19h model 61h): the new FICAA offsets, DF4 by its revision fields, the hole in function 7.
  AmdBus zen4;
  zen4.legacy_ficaa = false;
  zen4.add_root(0, 0);
  zen4.add(0, 0x18, 0, 0x1022, 0x14E0, 0x06, 0x00);
  zen4.df.push_back({(0u << 16) | 0x40, 0x04010010});
  zen4.df.push_back({(7u << 16) | 0x104, 0xE0000001});
  AmdNodes n4;
  n4.discover(zen4, 0x19, 0x61);
  df = n4.data_fabric();
  CHECK(df.revision == AmdDataFabricRevision::Df4 && df.major == 4 && df.minor == 1 && df.block_instances == 16 && df.dram_hole_base == 0xE0000000u);
  zen4.df[0] = {(0u << 16) | 0x40, 0x04050010};
  CHECK(n4.data_fabric().revision == AmdDataFabricRevision::Df4p5);
  zen4.df[0] = {(0u << 16) | 0x40, 0x05000010};
  CHECK(n4.data_fabric().revision == AmdDataFabricRevision::Later && !n4.data_fabric().dram_hole);

  // An EPYC with two sockets: four roots per node; node 1's SMN is behind the fifth root.
  AmdBus epyc;
  for (uint8_t i = 0; i < 8; ++i) epyc.add_root(static_cast<uint8_t>(i * 0x20), i / 4u);
  epyc.add(0, 0x18, 0, 0x1022, 0x14AD, 0x06, 0x00);
  epyc.add(0, 0x19, 0, 0x1022, 0x14AD, 0x06, 0x00);
  epyc.set_smn(1, 0x59800, 400u << 21);
  AmdNodes en;
  en.discover(epyc, 0x19, 0x11);
  CHECK(en.node_count() == 2 && en.root_count() == 8 && en.root_of(1).bus == 0x80);
  CHECK(en.temperatures(1).tctl_valid && en.temperatures(1).tctl_mc == 50000 && !en.temperatures(0).tctl_valid);

  // No SMN before Zen, and nothing without nodes or roots.
  AmdNodes old;
  old.discover(bus, 0x15, 0x30);
  CHECK(old.node_count() == 0 && !old.smn_read(0, 0x59800, &value) && old.data_fabric().revision == AmdDataFabricRevision::Unknown);
  AmdBus empty;
  AmdNodes nothing;
  nothing.discover(empty, 0x19, 0x21);
  CHECK(nothing.node_count() == 0 && !nothing.temperatures(0).tctl_valid);
  CHECK(!bus.interleaved);
}

// Several threads reading SMN through one shared AmdNodes, as SystemChipset and cpu_temperature_millicelsius do: every access is serialized and no
// index/data pair may be split by another thread's. The kernel serializes with Synchronized (a SpinLock, which disables interrupts and so cannot
// run in a user-mode test); a std::mutex stands in for it here. The bus model flags any interleaving it sees.
static void test_amd_nodes_threads() {
  AmdBus bus;
  bus.add_root(0, 0);
  bus.add(0, 0x18, 0, 0x1022, 0x14E0, 0x06, 0x00);
  for (uint32_t i = 0; i < 64; ++i) bus.set_smn(0, 0x1000 + i * 4, 0xA0000000u | i);
  AmdNodes nodes;
  nodes.discover(bus, 0x19, 0x61);
  std::mutex lock;
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 2000; ++i) {
        const uint32_t slot = static_cast<uint32_t>((t * 7 + i) % 64);
        uint32_t value = 0;
        bool ok;
        {
          std::lock_guard<std::mutex> guard(lock);
          ok = nodes.smn_read(0, 0x1000 + slot * 4, &value);
        }
        if (!ok || value != (0xA0000000u | slot)) ++wrong;
      }
    });
  }
  for (std::thread& t : threads) t.join();
  CHECK(wrong == 0 && !bus.interleaved);

  // The detector itself: a write of the index from one thread and the data read from another is caught.
  AmdBus probe;
  probe.add_root(0, 0);
  probe.write32({0, 0, 0}, AmdNodes::kSmnIndex, 0x1000);
  std::thread([&] { (void)probe.read32({0, 0, 0}, AmdNodes::kSmnData); }).join();
  CHECK(probe.interleaved);
}

int main() {
  test_gpio_layouts();
  test_gpio_pads();
  test_pch_thermal();
  test_tco_watchdog();
  test_amd_decoding();
  test_amd_nodes();
  test_amd_nodes_threads();
  if (g_failed) {
    printf("ChipsetDevicesTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("ChipsetDevicesTest: all tests passed\n");
  return 0;
}
