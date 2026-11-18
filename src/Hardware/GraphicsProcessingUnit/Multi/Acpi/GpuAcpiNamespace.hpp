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

// The ACPI pieces every hybrid-graphics handler needs: finding the namespace Device of a PCI function, calling a _DSM, and the device's power
// resources (_PR0/_PR3) that take a GPU from D3hot to D3cold and back (what the Linux PCI core does through acpi_device_set_power()).
//
// Finding a PCI function: its Device has _ADR = device << 16 | function, but _ADR has no bus number. The bus comes from the parent: a Device
// directly below a host bridge (_HID/_CID PNP0A08 or PNP0A03) is on the bus that host bridge's _BBN names (0 without one); a Device below a
// PCI bridge (a root port, itself a Device with an _ADR) is on that bridge's secondary bus, which the namespace does not say. So for a GPU
// behind a root port the caller passes the bridge's own PciAddress (from the PCI topology) as a hint, and the bridge is matched by its _ADR on
// the host bridge whose _BBN is the bridge's bus. Limits: segments (_SEG) are ignored (PciAddress has none); a bridge that sits behind
// another bridge (a switch, Thunderbolt) is matched by its _ADR alone; without a hint a GPU behind a root port is guessed among the
// Devices with its _ADR under any bridge, preferring the one that looks like a GPU (_ROM, ATRM, _DSM, _PR3, _PS3), the first in namespace
// order on a tie.
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

// The namespace nodes of a GPU function.
struct GpuAcpiNodes {
  acpi::Node* device = nullptr;   // the function's own Device (_ADR = device << 16 | function), or nullptr
  acpi::Node* bridge = nullptr;   // the PCI bridge (root port) Device above it, or nullptr (on a host bridge's bus, or not found)
};

class GpuAcpiNamespace {
 public:
  static constexpr uint32_t kMaxWalk = 16384;   // nodes visited by one search
  static constexpr uint32_t kMaxDepth = 32;

  // The Device of a PCI function, or nullptr. `parent_bridge`: the PCI bridge above the function (the GPU's root port), when the function is
  // not on bus 0. Takes the interpreter's lock.
  [[nodiscard]] static acpi::Node* find_pci_device(acpi::AmlInterpreter& aml, PciAddress function, const Optional<PciAddress>& parent_bridge = {});
  // The GPU's Device and the bridge Device above it. If the GPU itself has no Device, the bridge is still looked up from the hint.
  [[nodiscard]] static GpuAcpiNodes find_gpu(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge = {});

  // A named object directly below `node` (find() alone would also search the scopes above), or nullptr.
  [[nodiscard]] static acpi::Node* child(acpi::AmlInterpreter& aml, acpi::Node* node, const char* name);
  // A method directly below `node`, or nullptr.
  [[nodiscard]] static acpi::Node* child_method(acpi::AmlInterpreter& aml, acpi::Node* node, const char* name);
  // The first Device (namespace order) that has a method called `name` of its own, or nullptr.
  [[nodiscard]] static acpi::Node* find_device_with_method(acpi::AmlInterpreter& aml, const char* name);
  [[nodiscard]] static bool is_host_bridge(acpi::AmlInterpreter& aml, acpi::Node* node);
  // _ADR of a Device; empty if it has none or it does not evaluate to an integer.
  [[nodiscard]] static Optional<uint64_t> address_of(acpi::AmlInterpreter& aml, acpi::Node* device);
  static constexpr uint64_t pci_adr(PciAddress a) { return (uint64_t{a.device} << 16) | a.function; }
};

// The bytes of a String or Buffer value (empty for anything else). The value comes from the interpreter; it is valid until the next
// evaluation, so the caller holds the interpreter's lock while it reads them.
[[nodiscard]] ConstByteView acpi_value_bytes(const acpi::Value& value);

// --- _DSM (ACPI 6 9.1.1): Arg0 a UUID buffer, Arg1 the revision, Arg2 the function, Arg3 a package of arguments (or, for the NVIDIA and
// AMD GPU methods, an integer or a buffer).

// A UUID as ToUUID lays it out in a buffer: the first three fields little endian, the rest as written.
struct AcpiDsmUuid {
  uint8_t bytes[16];
};

enum class DsmArgument : uint8_t {
  EmptyPackage,   // Package () {}: what acpi_evaluate_dsm() passes for "no arguments"
  Integer,        // the value as an integer
  Buffer32,       // the value as a 4-byte little endian buffer (NVIDIA's Optimus and JT methods)
};

class AcpiDsm {
 public:
  // Calls `dsm`. The caller holds the interpreter's lock and reads `result` before the next evaluation. False if the method faulted.
  [[nodiscard]] static bool call(acpi::AmlInterpreter& aml, acpi::Node* dsm, const AcpiDsmUuid& uuid, uint32_t revision, uint32_t function,
                                 DsmArgument kind, uint32_t argument, acpi::Value* result);
  // The result as a 32-bit value: an integer, or a buffer of 1 to 4 bytes (little endian; a longer buffer gives its first four). Empty if
  // the method faulted or returned anything else. Takes the lock.
  [[nodiscard]] static Optional<uint32_t> call_u32(acpi::AmlInterpreter& aml, acpi::Node* dsm, const AcpiDsmUuid& uuid, uint32_t revision,
                                                   uint32_t function, DsmArgument kind, uint32_t argument);
  // Function 0: the bitmap of the functions the method supports (an integer or a buffer, up to 64 functions). 0 when bit 0 ("anything
  // supported") is clear, the result is not a number, or the method faulted, as acpi_check_dsm() treats it. Takes the lock.
  [[nodiscard]] static uint64_t supported(acpi::AmlInterpreter& aml, acpi::Node* dsm, const AcpiDsmUuid& uuid, uint32_t revision);
};

// --- Power resources.

// The power resources of a GPU and how to take it to D3cold and back. The resources are those of the GPU's own _PR3 or, when it has none,
// those of its parent bridge (many laptops describe the GPU's power rail on the root port): ACPI powers a device down by turning off what
// its D3hot state needs (_PR3) and up by turning on what D0 needs (_PR0). Without any resources the set only runs _PS3/_PS0, which some
// firmware uses to cut the power itself (NVIDIA Optimus).
//
// Not done: reference counting between devices that share a resource (the audio function of the GPU, another device on the same rail). The
// manager has put every function of the GPU in D3hot before power_off(); a resource shared with an unrelated device would be cut under it,
// which firmware for a GPU rail does not do in practice.
class GpuPowerResourceSet {
 public:
  static constexpr uint32_t kMaxResources = 8;

  // Reads the lists. Takes the lock. `device` or `bridge` may be nullptr.
  void read(acpi::AmlInterpreter& aml, acpi::Node* device, acpi::Node* bridge);
  // Any _PR3 resource found.
  bool has_resources() const { return d3_count_ != 0; }
  // The resources are the bridge's (not the GPU's own).
  bool on_bridge() const { return holder_ != nullptr && holder_ == bridge_ && holder_ != device_; }
  // A _PS3 on the GPU (or on the bridge holding the resources): something to run at all.
  bool has_ps3() const { return ps3_; }
  ArrayView<acpi::Node* const> d3_resources() const { return ArrayView<acpi::Node* const>(d3_, d3_count_); }
  ArrayView<acpi::Node* const> d0_resources() const { return ArrayView<acpi::Node* const>(d0_, d0_count_); }
  acpi::Node* device() const { return device_; }
  acpi::Node* holder() const { return holder_; }

  // _PS3 of the GPU (then of the bridge, when it holds the resources), then _OFF of every _PR3 resource that is on (by _STA, a resource
  // without _STA counts as on), last first. Stops at the first fault: the GPU stays in D3hot. Takes the lock.
  [[nodiscard]] bool power_off(acpi::AmlInterpreter& aml) const;
  // _ON of every resource of _PR0 and then of the _PR3 ones not in _PR0 that are off, first first, then _PS0 of the bridge (when it holds
  // the resources) and of the GPU. Goes on after a fault (to bring up as much as possible) and returns false. Takes the lock.
  [[nodiscard]] bool power_on(acpi::AmlInterpreter& aml) const;

 private:
  static uint32_t read_list(acpi::AmlInterpreter& aml, acpi::Node* holder, const char* name, ArrayView<acpi::Node*> out);

  acpi::Node* device_ = nullptr;
  acpi::Node* bridge_ = nullptr;
  acpi::Node* holder_ = nullptr;   // the Device whose _PR3 these are
  acpi::Node* d3_[kMaxResources] = {};
  acpi::Node* d0_[kMaxResources] = {};
  uint32_t d3_count_ = 0, d0_count_ = 0;
  bool ps3_ = false;
};

}  // namespace hardware
