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

// The PCI Express link of a discrete GPU and the other functions of its device, for putting the GPU to sleep:
//  - the bridge above it (the root port or switch downstream port whose secondary..subordinate bus range holds the GPU's bus);
//  - the device's other functions (a laptop NVIDIA GPU has HD audio at function 1, Turing and later also USB xHCI at 2 and UCSI at 3): every
//    one has to be in D3hot before the link can go down, or (Linux's "the HDA function keeps the GPU awake") the device stays in D0;
//  - the bridge's Link Control: disabling the link, and enabling it again with a retrain and a bounded wait for Data Link Layer Link Active;
//  - ASPM (L0s, L1) and clock power management, negotiated from both ends' Link Capabilities;
//  - whether the bridge can go to D3 itself (a simple pci_bridge_d3_possible()).
// Register layout: PCI Express Base Specification 5.0, section 7.5.3 (PCI Express Capability structure).
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace pcie {
constexpr uint8_t kCapabilityPm = 0x01, kCapabilityExpress = 0x10;
// Offsets inside the PCI Express capability.
constexpr uint16_t kCapabilities = 0x02, kDeviceCapabilities = 0x04, kLinkCapabilities = 0x0C, kLinkControl = 0x10, kLinkStatus = 0x12;
constexpr uint16_t kSlotCapabilities = 0x14;
// Device/Port Type (PCI Express Capabilities register bits 7:4).
constexpr uint8_t kTypeEndpoint = 0x0, kTypeLegacyEndpoint = 0x1, kTypeRootPort = 0x4, kTypeUpstreamPort = 0x5, kTypeDownstreamPort = 0x6;
constexpr uint8_t kTypePcieToPciBridge = 0x7, kTypePciToPcieBridge = 0x8, kTypeRootComplexEndpoint = 0x9;
constexpr uint16_t kCapSlotImplemented = 1u << 8;
// Link Capabilities.
constexpr uint32_t kLinkCapAspmL0s = 1u << 10, kLinkCapAspmL1 = 1u << 11, kLinkCapClockPm = 1u << 18, kLinkCapDllActiveReporting = 1u << 20;
// Link Control.
constexpr uint16_t kLinkCtlAspmMask = 0x0003, kLinkCtlAspmL0s = 0x0001, kLinkCtlAspmL1 = 0x0002;
constexpr uint16_t kLinkCtlDisable = 1u << 4, kLinkCtlRetrain = 1u << 5, kLinkCtlCommonClock = 1u << 6, kLinkCtlClockPm = 1u << 8;
// Link Status.
constexpr uint16_t kLinkStaTraining = 1u << 11, kLinkStaDllActive = 1u << 13;
// Slot Capabilities.
constexpr uint32_t kSlotCapHotPlugCapable = 1u << 6;
}  // namespace pcie

// What one function's PCI Express capability says about its link.
struct PcieLinkCapabilities {
  bool present = false;            // false: no PCI Express capability (a conventional PCI function)
  uint16_t capability = 0;         // its offset in config space
  uint8_t port_type = 0;           // pcie::kType*
  bool slot = false;               // a downstream port with a slot behind it
  uint8_t max_speed = 0;           // Link Capabilities' Max Link Speed (1: 2.5 GT/s, 2: 5, 3: 8, 4: 16, 5: 32)
  uint8_t max_width = 0;
  bool aspm_l0s = false, aspm_l1 = false;
  uint8_t l0s_exit = 0, l1_exit = 0;  // the encoded exit latencies (L0s: 2^(6+n) ns; L1: 2^n us; 7: more than that)
  bool clock_pm = false;
  bool dll_active_reporting = false;
  // Endpoints: the latencies the function accepts (Device Capabilities; the same encodings as the exit latencies, 7: no limit).
  uint8_t l0s_acceptable = 7, l1_acceptable = 7;
};

struct PcieLinkState {
  uint8_t speed = 0, width = 0;
  bool training = false, dll_active = false;
  bool disabled = false;           // Link Control's Link Disable
  uint16_t aspm = 0;               // Link Control's ASPM Control bits
  bool clock_pm = false;
};

// What both ends of a link may enable.
struct PcieAspmPlan {
  bool l0s = false, l1 = false, clock_pm = false;
  uint16_t control() const { return static_cast<uint16_t>((l0s ? pcie::kLinkCtlAspmL0s : 0) | (l1 ? pcie::kLinkCtlAspmL1 : 0)); }
};

using PcieDelayFn = void (*)(uint32_t milliseconds);

// Reads the PCI Express capability of a function. False if it has none (out->present stays false).
[[nodiscard]] bool pcie_link_capabilities(const IPciBus& pci, PciAddress function, PcieLinkCapabilities* out);
[[nodiscard]] bool pcie_link_state(const IPciBus& pci, PciAddress function, PcieLinkState* out);

// A type 1 (PCI-to-PCI bridge) config header.
bool pci_is_bridge(const IPciBus& pci, PciAddress function);
// The bridge directly above `function`: of the bridges whose secondary..subordinate range holds its bus, the innermost one (the highest
// secondary bus number: the one whose secondary bus is the function's own in a well-formed hierarchy). False on the root bus, or if no bridge
// on the bus list claims it.
[[nodiscard]] bool pci_find_upstream_bridge(const IPciBus& pci, PciAddress function, PciAddress* bridge);
// The functions of the device `function` is part of (the same bus and device number), in function-number order, at most out.size() of them;
// returns how many were stored. The GPU is function 0: put the others in D3hot first (and wake it first, before them).
uint32_t pci_device_functions(const IPciBus& pci, PciAddress function, ArrayView<PciDeviceInfo> out);

// Disables the bridge's link (Link Control's Link Disable). The functions behind it must be in D3hot already. False if the bridge has no PCI
// Express capability or is not a downstream-facing port (root port or switch downstream port).
[[nodiscard]] bool pcie_link_disable(const IPciBus& pci, PciAddress bridge);
// Enables the link again: clears Link Disable, then asks for a retrain, and waits for the link. Where the bridge reports Data Link Layer Link
// Active (Link Capabilities bit 20), it polls Link Status every millisecond until it is set, for at most `timeout_ms`, and returns false if it
// never is; elsewhere it waits a fixed 100 ms (the spec's time for a link to come up) and returns true. Then it waits `settle_ms` more
// (PCIe 6.6.1: 100 ms after the link is up before the first configuration request to the device). `delay` may be nullptr (no waits: the
// polls still end after timeout_ms of them).
[[nodiscard]] bool pcie_link_enable(const IPciBus& pci, PciAddress bridge, PcieDelayFn delay, uint32_t timeout_ms = 1000, uint32_t settle_ms = 100);

// Which ASPM states a link may use: the ones both ends support and whose exit latency (the larger of the two ends') every downstream function
// accepts. Clock PM: every downstream function supports it (it is enabled on the downstream functions only; the bit is reserved in a
// downstream port). A downstream function with no PCI Express capability makes everything off.
PcieAspmPlan pcie_aspm_negotiate(const PcieLinkCapabilities& upstream, ArrayView<const PcieLinkCapabilities> downstream);

// Programs ASPM Control (and Enable Clock Power Management) of the bridge and the functions below it. The order follows the PCIe Base
// Specification's rule for ASPM L1 (5.0, section 5.4.1, and the ASPM Control field of Link Control, 7.5.3.7; Linux pcie_config_aspm_link()
// does the same): ASPM L1 is enabled in the upstream component (the bridge) before the downstream component, and disabled in the downstream
// component first. All functions of the device get the same setting. So: when the plan turns states on, the bridge is written first and then the functions; when it turns them off,
// the functions first and then the bridge. (L0s has no ordering rule; it moves with L1.) False if either side has no PCI Express capability.
[[nodiscard]] bool pcie_aspm_apply(const IPciBus& pci, PciAddress bridge, ArrayView<const PciDeviceInfo> functions, const PcieAspmPlan& plan);
// pcie_aspm_negotiate() and pcie_aspm_apply() for the bridge above `function` and every function of its device. `applied` (may be nullptr)
// receives the plan.
[[nodiscard]] bool pcie_aspm_configure(const IPciBus& pci, PciAddress function, PcieAspmPlan* applied = nullptr);

// Whether the bridge can go to D3 (so the device behind it can reach D3cold), a simple form of Linux's pci_bridge_d3_possible(): a root port
// or switch downstream port with a power management capability; a hot-plug slot only if it reports Data Link Layer Link Active (otherwise a
// device appearing behind a sleeping port would not be noticed). Linux also asks the firmware (_S0W, the BIOS date, quirk lists); the caller
// does that (the ACPI power resources say whether D3cold is reachable at all).
bool pcie_bridge_d3_possible(const IPciBus& pci, PciAddress bridge);

}  // namespace hardware
