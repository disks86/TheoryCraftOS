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

// The PCI Express link of a discrete GPU against a model PCI hierarchy with real config spaces and capability lists: a root port with a
// four-function laptop GPU (VGA, HD audio, xHCI, UCSI) behind it, an integrated GPU on the root bus, a switch, and a conventional PCI device.
// Covers the bridge lookup, the function list, Link Disable and the wait for Data Link Layer Link Active (after a few polls, never, or not
// reported), the ASPM negotiation table, the order the ASPM writes go out in, and the D3 check of the bridge.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Multi/PcieLink.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

constexpr uint16_t kPmCap = 0x40, kExpCap = 0x60, kMsiCap = 0x50;

struct Function {
  PciDeviceInfo info;
  uint8_t cfg[256] = {};
  // the link of a bridge: how many Link Status reads after Link Disable is cleared until Data Link Layer Link Active sets (-1: never)
  int up_after = 0;
  int countdown = -1;
  uint32_t retrains = 0;

  void put32(uint16_t at, uint32_t v) { for (int i = 0; i < 4; ++i) cfg[at + i] = static_cast<uint8_t>(v >> (8 * i)); }
  void put16(uint16_t at, uint16_t v) { cfg[at] = static_cast<uint8_t>(v); cfg[at + 1] = static_cast<uint8_t>(v >> 8); }
  uint32_t get32(uint16_t at) const { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{cfg[at + i]} << (8 * i); return v; }
  uint16_t get16(uint16_t at) const { return static_cast<uint16_t>(cfg[at] | (cfg[at + 1] << 8)); }
  bool has_express() const { return cfg[0x34] != 0 && (cfg[kExpCap] == pcie::kCapabilityExpress); }
};

struct Write { PciAddress a; uint16_t offset; uint32_t value; };

class ModelPci final : public IPciBus {
 public:
  mutable std::vector<Function> functions;
  mutable std::vector<Write> writes;

  const char* name() const override { return "model"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(functions.size()); }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override {
    if (i >= functions.size()) return false;
    *out = functions[i].info;
    return true;
  }
  Function* find(PciAddress a) const {
    for (Function& f : functions)
      if (f.info.address.bus == a.bus && f.info.address.device == a.device && f.info.address.function == a.function) return &f;
    return nullptr;
  }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    Function* f = find(a);
    if (!f || offset > 252 || (offset & 3)) return 0xFFFFFFFF;
    if (f->has_express() && offset == kExpCap + pcie::kLinkControl) {
      const uint16_t control = f->get16(offset);
      bool active = false;
      if (!(control & pcie::kLinkCtlDisable)) {
        if (f->countdown > 0) --f->countdown;
        active = f->countdown == 0;
      }
      const uint16_t status = static_cast<uint16_t>((f->get16(offset + 2) & ~pcie::kLinkStaDllActive) | (active ? pcie::kLinkStaDllActive : 0));
      return control | (uint32_t{status} << 16);
    }
    return f->get32(offset);
  }
  void write32(PciAddress a, uint16_t offset, uint32_t value) const override {
    Function* f = find(a);
    if (!f || offset > 252 || (offset & 3)) return;
    writes.push_back({a, offset, value});
    if (f->has_express() && offset == kExpCap + pcie::kLinkControl) {
      const uint16_t before = f->get16(offset);
      uint16_t control = static_cast<uint16_t>(value);
      if (control & pcie::kLinkCtlRetrain) ++f->retrains;
      control &= static_cast<uint16_t>(~pcie::kLinkCtlRetrain);          // (reads back as 0)
      if (f->info.class_code == 0x06 && !(control & pcie::kLinkCtlDisable) && (before & pcie::kLinkCtlDisable)) f->countdown = f->up_after;
      if (control & pcie::kLinkCtlDisable) f->countdown = -1;
      f->put16(offset, control);
      // (the status half: RW1C bits, 0 written: unchanged)
    } else if (offset == kPmCap + 4) {
      f->put16(offset, static_cast<uint16_t>(value));
    }
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress a, uint8_t id) const override {
    Function* f = find(a);
    if (!f || !(f->get16(0x06) & 0x10)) return 0;          // Status: Capabilities List
    uint8_t at = f->cfg[0x34];
    for (int guard = 0; at && guard < 48; ++guard) {
      if (at < 0x40 || at > 0xFC) return 0;
      if (f->cfg[at] == id) return at;
      at = f->cfg[at + 1];
    }
    return 0;
  }

  // Builders.
  Function& add(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t vendor, uint16_t device, uint8_t cls, uint8_t sub, bool bridge) {
    Function f;
    f.info = {{bus, dev, fn}, vendor, device, cls, sub, 0};
    f.put16(0x00, vendor);
    f.put16(0x02, device);
    f.cfg[0x0B] = cls;
    f.cfg[0x0A] = sub;
    f.cfg[0x0E] = static_cast<uint8_t>((bridge ? 0x01 : 0x00) | (fn == 0 ? 0x80 : 0x00));
    functions.push_back(f);
    return functions.back();
  }
  // Capability list: PM at 0x40 -> MSI at 0x50 -> PCI Express at 0x60 (the order a GPU has them in), or only PM/MSI.
  static void capabilities(Function& f, bool express, uint8_t type, uint32_t link_caps, uint32_t device_caps = 0, bool slot = false) {
    f.put16(0x06, 0x0010);
    f.cfg[0x34] = kPmCap;
    f.cfg[kPmCap] = pcie::kCapabilityPm;
    f.cfg[kPmCap + 1] = kMsiCap;
    f.put16(kPmCap + 2, 0x0003);
    f.cfg[kMsiCap] = 0x05;
    f.cfg[kMsiCap + 1] = express ? kExpCap : 0;
    if (!express) return;
    f.cfg[kExpCap] = pcie::kCapabilityExpress;
    f.cfg[kExpCap + 1] = 0;
    f.put16(kExpCap + pcie::kCapabilities, static_cast<uint16_t>(0x0002 | (type << 4) | (slot ? pcie::kCapSlotImplemented : 0)));
    f.put32(kExpCap + pcie::kDeviceCapabilities, device_caps);
    f.put32(kExpCap + pcie::kLinkCapabilities, link_caps);
    f.put16(kExpCap + pcie::kLinkStatus, 0x1044);          // x4 at 8 GT/s... (speed 4: 16 GT/s, width 4)
  }
  static void buses(Function& f, uint8_t primary, uint8_t secondary, uint8_t subordinate) {
    f.put32(0x18, primary | (secondary << 8) | (uint32_t{subordinate} << 16));
  }
};

constexpr uint32_t link_caps(bool l0s, bool l1, uint8_t l0s_exit, uint8_t l1_exit, bool clock_pm, bool dll) {
  return 0x44 | (l0s ? pcie::kLinkCapAspmL0s : 0) | (l1 ? pcie::kLinkCapAspmL1 : 0) | (uint32_t{l0s_exit} << 12) | (uint32_t{l1_exit} << 15) |
         (clock_pm ? pcie::kLinkCapClockPm : 0) | (dll ? pcie::kLinkCapDllActiveReporting : 0);
}
constexpr uint32_t device_caps(uint8_t l0s_acceptable, uint8_t l1_acceptable) { return (uint32_t{l0s_acceptable} << 6) | (uint32_t{l1_acceptable} << 9); }

constexpr PciAddress kRootPort{0, 1, 0}, kGpu{1, 0, 0}, kHda{1, 0, 1}, kXhci{1, 0, 2}, kUcsi{1, 0, 3}, kIgpu{0, 2, 0};
constexpr PciAddress kSwitchUp{3, 0, 0}, kSwitchDown{4, 1, 0}, kBehindSwitch{5, 0, 0}, kLegacy{6, 0, 0}, kLegacyBridge{0, 0x1E, 0};

// The laptop: host bridge, iGPU, the dGPU's root port (DLL Active reported), a root port with a switch behind it, a PCI-to-PCI bridge with a
// conventional PCI card. The dGPU's functions are listed out of order (as an enumeration that walks buses breadth-first may list them).
ModelPci laptop(bool dll_reporting = true, uint32_t gpu_link = link_caps(true, true, 3, 2, true, false)) {
  ModelPci pci;
  pci.add(0, 0, 0, 0x8086, 0x9B61, 0x06, 0x00, false);                      // host bridge (type 0 header)
  Function& igpu = pci.add(0, 2, 0, 0x8086, 0x9BC4, 0x03, 0x00, false);
  ModelPci::capabilities(igpu, true, pcie::kTypeRootComplexEndpoint, 0);
  Function& rp = pci.add(0, 1, 0, 0x8086, 0x1901, 0x06, 0x04, true);
  ModelPci::capabilities(rp, true, pcie::kTypeRootPort, link_caps(true, true, 2, 3, false, dll_reporting));
  ModelPci::buses(rp, 0, 1, 1);
  rp.up_after = 3;
  const uint32_t ep = device_caps(7, 7);
  ModelPci::capabilities(pci.add(1, 0, 3, 0x10DE, 0x1AD7, 0x0C, 0x80, false), true, pcie::kTypeEndpoint, gpu_link, ep);   // UCSI
  ModelPci::capabilities(pci.add(1, 0, 0, 0x10DE, 0x1F95, 0x03, 0x00, false), true, pcie::kTypeLegacyEndpoint, gpu_link, ep);  // the GPU
  ModelPci::capabilities(pci.add(1, 0, 1, 0x10DE, 0x10FA, 0x04, 0x03, false), true, pcie::kTypeEndpoint, gpu_link, ep);   // HD audio
  ModelPci::capabilities(pci.add(1, 0, 2, 0x10DE, 0x1AD6, 0x0C, 0x03, false), true, pcie::kTypeEndpoint, gpu_link, ep);   // xHCI
  // a root port -> switch upstream port -> downstream port -> NVMe
  Function& rp2 = pci.add(0, 0x1D, 0, 0x8086, 0x02B0, 0x06, 0x04, true);
  ModelPci::capabilities(rp2, true, pcie::kTypeRootPort, link_caps(false, true, 0, 1, false, true), 0, true);
  ModelPci::buses(rp2, 0, 3, 5);
  rp2.put32(kExpCap + pcie::kSlotCapabilities, pcie::kSlotCapHotPlugCapable);
  Function& up = pci.add(3, 0, 0, 0x8086, 0x15DA, 0x06, 0x04, true);
  ModelPci::capabilities(up, true, pcie::kTypeUpstreamPort, link_caps(false, true, 0, 1, false, false));
  ModelPci::buses(up, 3, 4, 5);
  Function& down = pci.add(4, 1, 0, 0x8086, 0x15DA, 0x06, 0x04, true);
  ModelPci::capabilities(down, true, pcie::kTypeDownstreamPort, link_caps(false, true, 0, 1, false, false), 0, true);
  ModelPci::buses(down, 4, 5, 5);
  down.put32(kExpCap + pcie::kSlotCapabilities, pcie::kSlotCapHotPlugCapable);      // hot-plug, no DLL Active: no D3
  ModelPci::capabilities(pci.add(5, 0, 0, 0x144D, 0xA808, 0x01, 0x08, false), true, pcie::kTypeEndpoint, link_caps(true, true, 1, 1, true, false), device_caps(7, 7));
  // a PCIe-to-PCI bridge (no PCI Express capability modelled) with a conventional card
  Function& legacy_bridge = pci.add(0, 0x1E, 0, 0x8086, 0x244E, 0x06, 0x04, true);
  ModelPci::capabilities(legacy_bridge, false, 0, 0);
  ModelPci::buses(legacy_bridge, 0, 6, 6);
  ModelPci::capabilities(pci.add(6, 0, 0, 0x1102, 0x0002, 0x04, 0x01, false), false, 0, 0);
  return pci;
}

bool same(PciAddress a, PciAddress b) { return a.bus == b.bus && a.device == b.device && a.function == b.function; }

uint32_t g_delay_calls = 0, g_delay_ms = 0;
void count_delay(uint32_t ms) { ++g_delay_calls; g_delay_ms += ms; }

void test_topology() {
  ModelPci pci = laptop();
  PciAddress bridge{};
  CHECK(pci_is_bridge(pci, kRootPort) && !pci_is_bridge(pci, kGpu) && !pci_is_bridge(pci, {0, 0, 0}));
  CHECK(!pci_is_bridge(pci, {9, 9, 0}));                     // (nothing there)
  CHECK(pci_find_upstream_bridge(pci, kGpu, &bridge) && same(bridge, kRootPort));
  CHECK(pci_find_upstream_bridge(pci, kHda, &bridge) && same(bridge, kRootPort));
  CHECK(pci_find_upstream_bridge(pci, kBehindSwitch, &bridge) && same(bridge, kSwitchDown));   // the innermost of three that hold bus 5
  CHECK(pci_find_upstream_bridge(pci, kSwitchDown, &bridge) && same(bridge, kSwitchUp));
  CHECK(pci_find_upstream_bridge(pci, kSwitchUp, &bridge) && same(bridge, PciAddress{0, 0x1D, 0}));
  CHECK(pci_find_upstream_bridge(pci, kLegacy, &bridge) && same(bridge, kLegacyBridge));
  CHECK(!pci_find_upstream_bridge(pci, kIgpu, &bridge));     // the root bus: no bridge
  CHECK(!pci_find_upstream_bridge(pci, {7, 0, 0}, &bridge));  // a bus nobody claims
  // an unconfigured bridge (secondary 0) claims nothing
  ModelPci::buses(*pci.find(kRootPort), 0, 0, 0);
  CHECK(!pci_find_upstream_bridge(pci, kGpu, &bridge));

  ModelPci p2 = laptop();
  PciDeviceInfo functions[8];
  CHECK(pci_device_functions(p2, kGpu, ArrayView<PciDeviceInfo>(functions)) == 4);
  for (uint8_t i = 0; i < 4; ++i) CHECK(functions[i].address.bus == 1 && functions[i].address.function == i);
  CHECK(functions[0].class_code == 0x03 && functions[1].class_code == 0x04 && functions[2].subclass == 0x03 && functions[3].subclass == 0x80);
  CHECK(pci_device_functions(p2, kUcsi, ArrayView<PciDeviceInfo>(functions)) == 4);   // any of its functions finds the same set
  PciDeviceInfo two[2];
  CHECK(pci_device_functions(p2, kGpu, ArrayView<PciDeviceInfo>(two)) == 2);           // a short view: as many as fit
  CHECK(pci_device_functions(p2, kGpu, ArrayView<PciDeviceInfo>()) == 0);
  CHECK(pci_device_functions(p2, kIgpu, ArrayView<PciDeviceInfo>(functions)) == 1);
  CHECK(pci_device_functions(p2, {9, 0, 0}, ArrayView<PciDeviceInfo>(functions)) == 0);
}

void test_capabilities() {
  ModelPci pci = laptop();
  PcieLinkCapabilities c;
  CHECK(pcie_link_capabilities(pci, kRootPort, &c) && c.present && c.capability == kExpCap && c.port_type == pcie::kTypeRootPort);
  CHECK(c.aspm_l0s && c.aspm_l1 && c.l0s_exit == 2 && c.l1_exit == 3 && !c.clock_pm && c.dll_active_reporting && c.max_speed == 4 && c.max_width == 4);
  CHECK(c.l0s_acceptable == 7 && c.l1_acceptable == 7);                     // (a port has no acceptable latencies)
  CHECK(pcie_link_capabilities(pci, kGpu, &c) && c.port_type == pcie::kTypeLegacyEndpoint && c.clock_pm && !c.dll_active_reporting);
  CHECK(!pcie_link_capabilities(pci, kLegacy, &c) && !c.present);           // a PM and MSI capability, no PCI Express one
  CHECK(!pcie_link_capabilities(pci, {9, 0, 0}, &c));
  PcieLinkState s;
  CHECK(pcie_link_state(pci, kRootPort, &s) && s.width == 4 && s.speed == 4 && !s.disabled);
  CHECK(!pcie_link_state(pci, kLegacy, &s));
}

void test_link() {
  ModelPci pci = laptop();
  Function& rp = *pci.find(kRootPort);
  CHECK(pcie_link_disable(pci, kRootPort));
  PcieLinkState s;
  CHECK(pcie_link_state(pci, kRootPort, &s) && s.disabled && !s.dll_active);
  CHECK(!pcie_link_disable(pci, kGpu));                      // an endpoint: not a port that faces down
  CHECK(!pcie_link_disable(pci, kSwitchUp));
  CHECK(!pcie_link_disable(pci, kLegacyBridge));             // no PCI Express capability
  // DLL Active after three polls
  g_delay_calls = g_delay_ms = 0;
  rp.up_after = 3;
  CHECK(pcie_link_enable(pci, kRootPort, count_delay));
  CHECK(pcie_link_state(pci, kRootPort, &s) && !s.disabled && s.dll_active);
  CHECK(rp.retrains == 1);
  CHECK(g_delay_ms == 2 + 100 && g_delay_calls == 3);        // two 1 ms polls, then the 100 ms settle time
  // never: false after the timeout, bounded
  CHECK(pcie_link_disable(pci, kRootPort));
  rp.up_after = -1;
  g_delay_calls = g_delay_ms = 0;
  CHECK(!pcie_link_enable(pci, kRootPort, count_delay, 50));
  CHECK(g_delay_ms == 50);
  CHECK(!pcie_link_enable(pci, kRootPort, nullptr, 50));     // (no delay function: still ends)
  // a bridge that does not report DLL Active: a fixed 100 ms, then the settle time
  ModelPci quiet = laptop(false);
  CHECK(pcie_link_disable(quiet, kRootPort));
  g_delay_calls = g_delay_ms = 0;
  quiet.find(kRootPort)->up_after = -1;
  CHECK(pcie_link_enable(quiet, kRootPort, count_delay, 1000, 0));
  CHECK(g_delay_ms == 100 && g_delay_calls == 1);
  // the Link Control write keeps the other bits and never writes the status half
  ModelPci keep = laptop();
  Function& k = *keep.find(kRootPort);
  k.put16(kExpCap + pcie::kLinkControl, pcie::kLinkCtlCommonClock | pcie::kLinkCtlAspmL1);
  k.put16(kExpCap + pcie::kLinkStatus, 0xC044);              // the RW1C bandwidth status bits set
  keep.writes.clear();
  CHECK(pcie_link_disable(keep, kRootPort));
  CHECK(keep.writes.size() == 1 && keep.writes[0].value == (pcie::kLinkCtlCommonClock | pcie::kLinkCtlAspmL1 | pcie::kLinkCtlDisable));
  CHECK(pcie_link_enable(keep, kRootPort, nullptr));
  CHECK((k.get16(kExpCap + pcie::kLinkControl) == (pcie::kLinkCtlCommonClock | pcie::kLinkCtlAspmL1)));
  for (const Write& w : keep.writes) CHECK((w.value >> 16) == 0);
}

PcieLinkCapabilities caps(bool l0s, bool l1, uint8_t l0s_exit, uint8_t l1_exit, bool clock_pm, uint8_t type, uint8_t l0s_ok = 7, uint8_t l1_ok = 7) {
  PcieLinkCapabilities c;
  c.present = true;
  c.capability = kExpCap;
  c.port_type = type;
  c.aspm_l0s = l0s;
  c.aspm_l1 = l1;
  c.l0s_exit = l0s_exit;
  c.l1_exit = l1_exit;
  c.clock_pm = clock_pm;
  c.l0s_acceptable = l0s_ok;
  c.l1_acceptable = l1_ok;
  return c;
}

void test_negotiation() {
  struct Row { bool up_l0s, up_l1, dn_l0s, dn_l1, dn_clock; uint8_t up_l1_exit, dn_l1_exit, l1_ok, l0s_ok; bool l0s, l1, clock; };
  const Row table[] = {
      // both support both, no latency limit
      {true, true, true, true, true, 2, 2, 7, 7, true, true, true},
      // only one end has L0s / L1
      {false, true, true, true, false, 2, 2, 7, 7, false, true, false},
      {true, false, true, true, true, 2, 2, 7, 7, true, false, true},
      {true, true, false, true, true, 2, 2, 7, 7, false, true, true},
      {true, true, true, false, true, 2, 2, 7, 7, true, false, true},
      {false, false, false, false, false, 0, 0, 7, 7, false, false, false},
      // L1 exit latency: the larger end's (2^3 = 8 us) against what the endpoint accepts
      {true, true, true, true, true, 3, 1, 3, 7, true, true, true},       // 8 us <= 8 us
      {true, true, true, true, true, 3, 1, 2, 7, true, false, true},      // 8 us > 4 us
      {true, true, true, true, true, 1, 4, 3, 7, true, false, true},      // the downstream end's 16 us is the larger
      {true, true, true, true, true, 7, 7, 7, 7, true, true, true},       // "more than 64 us", accepted without a limit
      // L0s exit latency (both ends' 2^(6+3) ns) against the endpoint's acceptable L0s latency
      {true, true, true, true, true, 0, 0, 7, 2, false, true, true},
  };
  for (const Row& r : table) {
    const PcieLinkCapabilities up = caps(r.up_l0s, r.up_l1, 3, r.up_l1_exit, false, pcie::kTypeRootPort);
    const PcieLinkCapabilities dn[1] = {caps(r.dn_l0s, r.dn_l1, 3, r.dn_l1_exit, r.dn_clock, pcie::kTypeEndpoint, r.l0s_ok, r.l1_ok)};
    const PcieAspmPlan p = pcie_aspm_negotiate(up, ArrayView<const PcieLinkCapabilities>(dn));
    CHECK(p.l0s == r.l0s && p.l1 == r.l1 && p.clock_pm == r.clock);
  }
  // several functions: the weakest decides
  const PcieLinkCapabilities up = caps(true, true, 1, 1, false, pcie::kTypeRootPort);
  PcieLinkCapabilities dn[4] = {caps(true, true, 1, 1, true, pcie::kTypeLegacyEndpoint), caps(true, true, 1, 1, true, pcie::kTypeEndpoint),
                                caps(true, false, 1, 1, true, pcie::kTypeEndpoint), caps(true, true, 1, 1, false, pcie::kTypeEndpoint)};
  PcieAspmPlan p = pcie_aspm_negotiate(up, ArrayView<const PcieLinkCapabilities>(dn));
  CHECK(p.l0s && !p.l1 && !p.clock_pm && p.control() == pcie::kLinkCtlAspmL0s);
  // a function without PCI Express, no functions, no upstream capability: nothing
  dn[1].present = false;
  p = pcie_aspm_negotiate(up, ArrayView<const PcieLinkCapabilities>(dn));
  CHECK(!p.l0s && !p.l1 && !p.clock_pm);
  p = pcie_aspm_negotiate(up, ArrayView<const PcieLinkCapabilities>());
  CHECK(!p.l0s && !p.l1 && p.control() == 0);
  PcieLinkCapabilities none;
  p = pcie_aspm_negotiate(none, ArrayView<const PcieLinkCapabilities>(dn, 1));
  CHECK(!p.l0s && !p.l1);
}

// The ASPM bits of each Link Control write, in order: (is the bridge, ASPM bits).
std::vector<std::pair<bool, uint16_t>> aspm_writes(const ModelPci& pci) {
  std::vector<std::pair<bool, uint16_t>> r;
  for (const Write& w : pci.writes)
    if (w.offset == kExpCap + pcie::kLinkControl) r.push_back({same(w.a, kRootPort), static_cast<uint16_t>(w.value & pcie::kLinkCtlAspmMask)});
  return r;
}

void test_aspm_apply() {
  ModelPci pci = laptop();
  PcieAspmPlan plan;
  CHECK(pcie_aspm_configure(pci, kGpu, &plan));
  CHECK(plan.l0s && plan.l1 && plan.clock_pm);
  // enabling: the bridge first, then all four functions
  auto w = aspm_writes(pci);
  CHECK(w.size() == 5 && w[0].first && w[0].second == 3);
  for (size_t i = 1; i < w.size(); ++i) CHECK(!w[i].first && w[i].second == 3);
  for (const PciAddress a : {kGpu, kHda, kXhci, kUcsi}) {
    PcieLinkState s;
    CHECK(pcie_link_state(pci, a, &s) && s.aspm == 3 && s.clock_pm);
  }
  PcieLinkState s;
  CHECK(pcie_link_state(pci, kRootPort, &s) && s.aspm == 3 && !s.clock_pm);   // (Clock PM is not set in the port)
  // configuring again changes nothing: no writes
  pci.writes.clear();
  CHECK(pcie_aspm_configure(pci, kHda));
  CHECK(aspm_writes(pci).empty());
  // disabling: the functions first, then the bridge
  PciDeviceInfo functions[8];
  const uint32_t n = pci_device_functions(pci, kGpu, ArrayView<PciDeviceInfo>(functions));
  pci.writes.clear();
  CHECK(pcie_aspm_apply(pci, kRootPort, ArrayView<const PciDeviceInfo>(functions, n), PcieAspmPlan()));
  w = aspm_writes(pci);
  CHECK(w.size() == 5 && w.back().first && w.back().second == 0);
  for (size_t i = 0; i + 1 < w.size(); ++i) CHECK(!w[i].first && w[i].second == 0);
  CHECK(pcie_link_state(pci, kGpu, &s) && s.aspm == 0 && !s.clock_pm);
  // L1 only to L0s only: L1 goes off downstream first, then the bridge drops it, then L0s comes on in the bridge first
  PcieAspmPlan l1;
  l1.l1 = true;
  CHECK(pcie_aspm_apply(pci, kRootPort, ArrayView<const PciDeviceInfo>(functions, n), l1));
  pci.writes.clear();
  PcieAspmPlan l0s;
  l0s.l0s = true;
  CHECK(pcie_aspm_apply(pci, kRootPort, ArrayView<const PciDeviceInfo>(functions, n), l0s));
  w = aspm_writes(pci);
  CHECK(w.size() == 10);
  if (w.size() == 10) {
    for (size_t i = 0; i < 4; ++i) CHECK(!w[i].first && w[i].second == 0);
    CHECK(w[4].first && w[4].second == 0);
    CHECK(w[5].first && w[5].second == pcie::kLinkCtlAspmL0s);
    for (size_t i = 6; i < 10; ++i) CHECK(!w[i].first && w[i].second == pcie::kLinkCtlAspmL0s);
  }
  // a GPU whose functions have no L1 support, no Clock PM
  ModelPci weak = laptop(true, link_caps(true, false, 1, 1, false, false));
  CHECK(pcie_aspm_configure(weak, kGpu, &plan) && plan.l0s && !plan.l1 && !plan.clock_pm);
  CHECK(pcie_link_state(weak, kXhci, &s) && s.aspm == pcie::kLinkCtlAspmL0s && !s.clock_pm);
  // no PCI Express on one side: refused, nothing written
  ModelPci legacy = laptop();
  legacy.writes.clear();
  CHECK(!pcie_aspm_configure(legacy, kLegacy));
  CHECK(!pcie_aspm_configure(legacy, kIgpu));                 // the root bus: no bridge
  CHECK(legacy.writes.empty());
  PciDeviceInfo conventional[1] = {legacy.find(kLegacy)->info};
  CHECK(!pcie_aspm_apply(legacy, kRootPort, ArrayView<const PciDeviceInfo>(conventional), l1));
  CHECK(!pcie_aspm_apply(legacy, kLegacyBridge, ArrayView<const PciDeviceInfo>(functions, n), l1));
  CHECK(legacy.writes.empty());
}

void test_d3() {
  ModelPci pci = laptop();
  CHECK(pcie_bridge_d3_possible(pci, kRootPort));
  CHECK(pcie_bridge_d3_possible(pci, {0, 0x1D, 0}));           // a hot-plug slot that reports DLL Active
  CHECK(!pcie_bridge_d3_possible(pci, kSwitchDown));           // a hot-plug slot that does not
  CHECK(!pcie_bridge_d3_possible(pci, kSwitchUp));             // not downstream-facing
  CHECK(!pcie_bridge_d3_possible(pci, kGpu));
  CHECK(!pcie_bridge_d3_possible(pci, kLegacyBridge));
  // no power management capability: the list goes straight to PCI Express
  Function& rp = *pci.find(kRootPort);
  rp.cfg[0x34] = kExpCap;
  CHECK(!pcie_bridge_d3_possible(pci, kRootPort));
  // a capability list that loops does not hang
  rp.cfg[0x34] = kPmCap;
  rp.cfg[kPmCap + 1] = kPmCap;
  rp.cfg[kPmCap] = 0x09;
  PcieLinkCapabilities c;
  CHECK(!pcie_link_capabilities(pci, kRootPort, &c));
}

}  // namespace

int main() {
  test_topology();
  test_capabilities();
  test_link();
  test_negotiation();
  test_aspm_apply();
  test_d3();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("PcieLinkTest: ok\n");
  return 0;
}
