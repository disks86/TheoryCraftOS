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

#include "Hardware/GraphicsProcessingUnit/Multi/PcieLink.hpp"

namespace hardware {

namespace {

constexpr uint16_t kHeaderTypeOffset = 0x0C, kBusNumbersOffset = 0x18;
constexpr uint32_t kMaxDeviceFunctions = 8;

uint16_t read16(const IPciBus& pci, PciAddress a, uint16_t offset) {
  return static_cast<uint16_t>(pci.read32(a, static_cast<uint16_t>(offset & ~3u)) >> ((offset & 2u) * 8));
}

// Link Control is the low half of the dword whose high half is Link Status. Its RW1C bits (Link Bandwidth / Autonomous Bandwidth Management
// Status) are written as 0, which leaves them alone; the rest of Link Status is read-only.
void write_link_control(const IPciBus& pci, PciAddress a, uint16_t capability, uint16_t value) {
  pci.write32(a, static_cast<uint16_t>(capability + pcie::kLinkControl), value);
}

uint16_t link_control(const IPciBus& pci, PciAddress a, uint16_t capability) {
  return read16(pci, a, static_cast<uint16_t>(capability + pcie::kLinkControl));
}

bool same_address(PciAddress a, PciAddress b) { return a.bus == b.bus && a.device == b.device && a.function == b.function; }

bool downstream_facing(const PcieLinkCapabilities& c) {
  return c.present && (c.port_type == pcie::kTypeRootPort || c.port_type == pcie::kTypeDownstreamPort);
}

bool is_endpoint(uint8_t type) { return type == pcie::kTypeEndpoint || type == pcie::kTypeLegacyEndpoint; }

}  // namespace

bool pcie_link_capabilities(const IPciBus& pci, PciAddress function, PcieLinkCapabilities* out) {
  *out = PcieLinkCapabilities();
  const uint16_t cap = pci.find_capability(function, pcie::kCapabilityExpress);
  if (cap == 0 || cap > 0x100 - 0x18) return false;    // (the registers used here reach cap+0x17 in the first 256 bytes)
  const uint16_t caps = read16(pci, function, static_cast<uint16_t>(cap + pcie::kCapabilities));
  const uint32_t device = pci.read32(function, static_cast<uint16_t>(cap + pcie::kDeviceCapabilities));
  const uint32_t link = pci.read32(function, static_cast<uint16_t>(cap + pcie::kLinkCapabilities));
  out->present = true;
  out->capability = cap;
  out->port_type = static_cast<uint8_t>((caps >> 4) & 0xF);
  out->slot = (caps & pcie::kCapSlotImplemented) != 0;
  out->max_speed = static_cast<uint8_t>(link & 0xF);
  out->max_width = static_cast<uint8_t>((link >> 4) & 0x3F);
  out->aspm_l0s = (link & pcie::kLinkCapAspmL0s) != 0;
  out->aspm_l1 = (link & pcie::kLinkCapAspmL1) != 0;
  out->l0s_exit = static_cast<uint8_t>((link >> 12) & 0x7);
  out->l1_exit = static_cast<uint8_t>((link >> 15) & 0x7);
  out->clock_pm = (link & pcie::kLinkCapClockPm) != 0;
  out->dll_active_reporting = (link & pcie::kLinkCapDllActiveReporting) != 0;
  if (is_endpoint(out->port_type)) {
    out->l0s_acceptable = static_cast<uint8_t>((device >> 6) & 0x7);
    out->l1_acceptable = static_cast<uint8_t>((device >> 9) & 0x7);
  }
  return true;
}

bool pcie_link_state(const IPciBus& pci, PciAddress function, PcieLinkState* out) {
  *out = PcieLinkState();
  PcieLinkCapabilities c;
  if (!pcie_link_capabilities(pci, function, &c)) return false;
  const uint32_t v = pci.read32(function, static_cast<uint16_t>(c.capability + pcie::kLinkControl));
  const uint16_t control = static_cast<uint16_t>(v), status = static_cast<uint16_t>(v >> 16);
  out->speed = static_cast<uint8_t>(status & 0xF);
  out->width = static_cast<uint8_t>((status >> 4) & 0x3F);
  out->training = (status & pcie::kLinkStaTraining) != 0;
  out->dll_active = (status & pcie::kLinkStaDllActive) != 0;
  out->disabled = (control & pcie::kLinkCtlDisable) != 0;
  out->aspm = control & pcie::kLinkCtlAspmMask;
  out->clock_pm = (control & pcie::kLinkCtlClockPm) != 0;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Topology

bool pci_is_bridge(const IPciBus& pci, PciAddress function) {
  const uint32_t v = pci.read32(function, kHeaderTypeOffset);
  if (v == 0xFFFFFFFF) return false;                   // (nothing answers)
  return ((v >> 16) & 0x7F) == 0x01;
}

bool pci_find_upstream_bridge(const IPciBus& pci, PciAddress function, PciAddress* bridge) {
  bool found = false;
  uint8_t best_secondary = 0;
  PciDeviceInfo info;
  for (uint32_t i = 0; pci.pci_device(i, &info); ++i) {
    if (same_address(info.address, function) || !pci_is_bridge(pci, info.address)) continue;
    const uint32_t buses = pci.read32(info.address, kBusNumbersOffset);
    const uint8_t primary = static_cast<uint8_t>(buses), secondary = static_cast<uint8_t>(buses >> 8), subordinate = static_cast<uint8_t>(buses >> 16);
    // (an unconfigured bridge has secondary 0; a bridge's secondary bus is always above its primary)
    if (secondary == 0 || secondary <= primary || subordinate < secondary) continue;
    if (function.bus < secondary || function.bus > subordinate) continue;
    if (!found || secondary > best_secondary) {
      found = true;
      best_secondary = secondary;
      *bridge = info.address;
    }
  }
  return found;
}

uint32_t pci_device_functions(const IPciBus& pci, PciAddress function, ArrayView<PciDeviceInfo> out) {
  uint32_t count = 0;
  PciDeviceInfo info;
  for (uint32_t i = 0; pci.pci_device(i, &info) && count < out.size(); ++i) {
    if (info.address.bus != function.bus || info.address.device != function.device) continue;
    bool duplicate = false;
    for (uint32_t j = 0; j < count; ++j) duplicate = duplicate || out[j].address.function == info.address.function;
    if (duplicate) continue;
    // insert in function-number order
    uint32_t at = count;
    while (at > 0 && out[at - 1].address.function > info.address.function) {
      out[at] = out[at - 1];
      --at;
    }
    out[at] = info;
    ++count;
  }
  return count;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Link Disable

bool pcie_link_disable(const IPciBus& pci, PciAddress bridge) {
  PcieLinkCapabilities c;
  if (!pcie_link_capabilities(pci, bridge, &c) || !downstream_facing(c)) return false;
  const uint16_t control = link_control(pci, bridge, c.capability);
  write_link_control(pci, bridge, c.capability, static_cast<uint16_t>((control | pcie::kLinkCtlDisable) & ~pcie::kLinkCtlRetrain));
  return (link_control(pci, bridge, c.capability) & pcie::kLinkCtlDisable) != 0;
}

bool pcie_link_enable(const IPciBus& pci, PciAddress bridge, PcieDelayFn delay, uint32_t timeout_ms, uint32_t settle_ms) {
  PcieLinkCapabilities c;
  if (!pcie_link_capabilities(pci, bridge, &c) || !downstream_facing(c)) return false;
  const uint16_t control = static_cast<uint16_t>(link_control(pci, bridge, c.capability) & ~(pcie::kLinkCtlDisable | pcie::kLinkCtlRetrain));
  write_link_control(pci, bridge, c.capability, control);
  // (Retrain Link has no effect while Link Disable is set, so it is a second write; it reads back as 0.)
  write_link_control(pci, bridge, c.capability, static_cast<uint16_t>(control | pcie::kLinkCtlRetrain));
  if (c.dll_active_reporting) {
    bool up = false;
    for (uint32_t waited = 0; waited <= timeout_ms; ++waited) {
      if (read16(pci, bridge, static_cast<uint16_t>(c.capability + pcie::kLinkStatus)) & pcie::kLinkStaDllActive) {
        up = true;
        break;
      }
      if (waited < timeout_ms && delay) delay(1);
    }
    if (!up) return false;
  } else if (delay) {
    delay(100);
  }
  if (delay && settle_ms) delay(settle_ms);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// ASPM

PcieAspmPlan pcie_aspm_negotiate(const PcieLinkCapabilities& upstream, ArrayView<const PcieLinkCapabilities> downstream) {
  PcieAspmPlan plan;
  if (!upstream.present || downstream.size() == 0) return plan;
  plan.l0s = upstream.aspm_l0s;
  plan.l1 = upstream.aspm_l1;
  plan.clock_pm = true;
  for (const PcieLinkCapabilities& d : downstream) {
    if (!d.present) return PcieAspmPlan();
    // The exit latency of the link is the larger of its two ends'; the encodings of exit and acceptable latency line up (n: up to 2^n
    // units), 7 accepts anything.
    const uint8_t l0s_exit = upstream.l0s_exit > d.l0s_exit ? upstream.l0s_exit : d.l0s_exit;
    const uint8_t l1_exit = upstream.l1_exit > d.l1_exit ? upstream.l1_exit : d.l1_exit;
    plan.l0s = plan.l0s && d.aspm_l0s && (d.l0s_acceptable == 7 || l0s_exit <= d.l0s_acceptable);
    plan.l1 = plan.l1 && d.aspm_l1 && (d.l1_acceptable == 7 || l1_exit <= d.l1_acceptable);
    plan.clock_pm = plan.clock_pm && d.clock_pm;
  }
  return plan;
}

bool pcie_aspm_apply(const IPciBus& pci, PciAddress bridge, ArrayView<const PciDeviceInfo> functions, const PcieAspmPlan& plan) {
  PcieLinkCapabilities up;
  if (!pcie_link_capabilities(pci, bridge, &up)) return false;
  uint16_t caps[kMaxDeviceFunctions] = {};
  if (functions.size() > kMaxDeviceFunctions) return false;
  for (uint64_t i = 0; i < functions.size(); ++i) {
    PcieLinkCapabilities d;
    if (!pcie_link_capabilities(pci, functions[i].address, &d)) return false;
    caps[i] = d.capability;
  }
  const uint16_t wanted = plan.control();
  auto update = [&](PciAddress a, uint16_t cap, uint16_t aspm, bool clock_pm, bool touch_clock) {
    const uint16_t now = link_control(pci, a, cap);
    uint16_t next = static_cast<uint16_t>((now & ~(pcie::kLinkCtlAspmMask | pcie::kLinkCtlRetrain)) | aspm);
    if (touch_clock) next = static_cast<uint16_t>(clock_pm ? (next | pcie::kLinkCtlClockPm) : (next & ~pcie::kLinkCtlClockPm));
    if (next != (now & ~pcie::kLinkCtlRetrain)) write_link_control(pci, a, cap, next);
  };
  // Phase 1, turning off: the functions (the downstream component) first, then the bridge. Each keeps only the bits that stay on.
  for (uint64_t i = 0; i < functions.size(); ++i) {
    const uint16_t now = link_control(pci, functions[i].address, caps[i]) & pcie::kLinkCtlAspmMask;
    update(functions[i].address, caps[i], static_cast<uint16_t>(now & wanted), false, !plan.clock_pm);
  }
  {
    const uint16_t now = link_control(pci, bridge, up.capability) & pcie::kLinkCtlAspmMask;
    update(bridge, up.capability, static_cast<uint16_t>(now & wanted), false, false);
  }
  // Phase 2, turning on: the bridge (the upstream component) first, then the functions.
  update(bridge, up.capability, wanted, false, false);
  for (uint64_t i = 0; i < functions.size(); ++i) update(functions[i].address, caps[i], wanted, plan.clock_pm, plan.clock_pm);
  return true;
}

bool pcie_aspm_configure(const IPciBus& pci, PciAddress function, PcieAspmPlan* applied) {
  PciAddress bridge{};
  if (!pci_find_upstream_bridge(pci, function, &bridge)) return false;
  PcieLinkCapabilities up;
  if (!pcie_link_capabilities(pci, bridge, &up) || !downstream_facing(up)) return false;
  PciDeviceInfo functions[kMaxDeviceFunctions];
  const uint32_t count = pci_device_functions(pci, function, ArrayView<PciDeviceInfo>(functions));
  if (count == 0) return false;
  PcieLinkCapabilities down[kMaxDeviceFunctions];
  for (uint32_t i = 0; i < count; ++i) (void)pcie_link_capabilities(pci, functions[i].address, &down[i]);   // (absent: present stays false)
  const PcieAspmPlan plan = pcie_aspm_negotiate(up, ArrayView<const PcieLinkCapabilities>(down, count));
  for (uint32_t i = 0; i < count; ++i)
    if (!down[i].present) return false;
  if (!pcie_aspm_apply(pci, bridge, ArrayView<const PciDeviceInfo>(functions, count), plan)) return false;
  if (applied) *applied = plan;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// D3 of the bridge

bool pcie_bridge_d3_possible(const IPciBus& pci, PciAddress bridge) {
  PcieLinkCapabilities c;
  if (!pcie_link_capabilities(pci, bridge, &c) || !downstream_facing(c)) return false;
  if (pci.find_capability(bridge, pcie::kCapabilityPm) == 0) return false;
  if (c.slot) {
    const uint32_t slot = pci.read32(bridge, static_cast<uint16_t>(c.capability + pcie::kSlotCapabilities));
    if ((slot & pcie::kSlotCapHotPlugCapable) && !c.dll_active_reporting) return false;
  }
  return true;
}

}  // namespace hardware
