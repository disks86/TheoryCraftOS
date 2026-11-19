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
#include "Kernel/Sync.hpp"
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

// --- The PCI firmware _DSM (PCI Firmware Specification 3.3, 4.6): E5C937D0-3553-4D7A-9117-EA4D19C3434D on a PCI function's or bridge's Device.

extern const AcpiDsmUuid kPciFirmwareDsmUuid;

// Function 9, "_DSM for Specifying Device Readiness Durations" (revision 3 and later): a Package of five Integers, each a time in
// microseconds the function needs before it answers configuration requests after (1) a Conventional Reset, (2) the link coming up (DL_Up),
// (3) a Function Level Reset, (4) D3hot -> D0, (5) setting VF Enable. A value that is not an integer (or does not fit 32 bits) is absent.
// Linux reads it on every PCI device (pci_acpi_optimize_delay) and uses the reset time as the D3cold -> D0 delay and the D3hot -> D0 time
// as the D3hot delay, each only when it is shorter than the default (100 and 10 ms).
struct PciDeviceReadiness {
  Optional<uint32_t> reset_us, dl_up_us, flr_us, d3hot_to_d0_us, vf_enable_us;
};

class AcpiPciReadiness {
 public:
  static constexpr uint32_t kRevision = 3, kFunction = 9;
  static constexpr uint32_t kDefaultD3coldDelayMs = 100;   // PCI_PM_D3COLD_WAIT: what the PCIe base spec allows a function after a reset

  // Function 9 of `device`'s own _DSM (only when function 0 lists it). Empty if there is none, it faulted, or the answer is not a package of
  // at least five elements. Takes the lock.
  [[nodiscard]] static Optional<PciDeviceReadiness> read(acpi::AmlInterpreter& aml, acpi::Node* device);
  // The wait after the power comes back before the function answers: the longer of the reset and DL_Up times, rounded up to milliseconds,
  // when that is shorter than `default_ms`; else `default_ms` (the firmware may shorten the wait, never lengthen it, as in Linux).
  [[nodiscard]] static uint32_t d3cold_delay_ms(const Optional<PciDeviceReadiness>& readiness, uint32_t default_ms = kDefaultD3coldDelayMs);
};

// --- Power resources.

// Which devices need which power resource, for the whole machine (ACPI 6, 7.2: a power resource may be shared by several devices and is
// turned off only when no device that needs it is in a state that needs it; Linux counts references per resource in drivers/acpi/power.c).
// A holder is the namespace Device on whose behalf a resource is on: a GPU (or, when the GPU has no Device of its own, the root port holding
// its _PR3), the HD-audio function, another root port. A holder becomes known with introduce() (it is then counted as needing the
// resource: it was found in D0) and changes with need() and drop(). The interpreter's lock must be held around a decision and the _ON / _OFF
// that follows it, so no other holder's change slips in between (GpuPowerResourceSet does). Thread safe on its own; a sleeping lock (the
// callers are threads evaluating AML, which sleep anyway). Nodes are never freed by the interpreter, so they are stable keys. When the table
// is full a resource is not tracked: need() reports 1 holder and drop() 0 others, which is the behaviour without counting.
class AcpiPowerResourceRegistry {
 public:
  static constexpr uint32_t kMaxResources = 32, kMaxHolders = 8;

  constexpr AcpiPowerResourceRegistry() = default;
  AcpiPowerResourceRegistry(const AcpiPowerResourceRegistry&) = delete;
  AcpiPowerResourceRegistry& operator=(const AcpiPowerResourceRegistry&) = delete;

  // The machine's registry (what GpuPowerResourceSet uses unless told otherwise).
  static AcpiPowerResourceRegistry& global();

  // `holder` uses `resource`; the first time it is counted as needing it, later calls change nothing.
  void introduce(acpi::Node* resource, acpi::Node* holder);
  // `holder` needs `resource` (on). Returns how many holders need it now, this one included.
  uint32_t need(acpi::Node* resource, acpi::Node* holder);
  // `holder` no longer needs `resource`. Returns how many other holders still do (0: it may be turned off).
  uint32_t drop(acpi::Node* resource, acpi::Node* holder);
  // How many holders need `resource` now.
  [[nodiscard]] uint32_t holders_needing(acpi::Node* resource) const;
  [[nodiscard]] bool needs(acpi::Node* resource, acpi::Node* holder) const;
  // Forgets everything (tests; an interpreter that was thrown away).
  void clear();

 private:
  struct Holder {
    acpi::Node* node = nullptr;
    bool needs = false;
  };
  struct Entry {
    acpi::Node* resource = nullptr;
    Holder holders[kMaxHolders] = {};
    uint32_t count = 0;
  };
  Entry* find_locked(acpi::Node* resource);
  const Entry* find_locked(acpi::Node* resource) const;
  Entry* add_locked(acpi::Node* resource);
  static Holder* holder_in(Entry& e, acpi::Node* holder, bool create);
  static uint32_t needing(const Entry& e);

  mutable kernel::Mutex mutex_;
  Entry entries_[kMaxResources] = {};
  uint32_t count_ = 0;
};

// The power resources of a GPU and how to take it to D3cold and back. The resources are those of the GPU's own _PR3 or, when it has none,
// those of its parent bridge (many laptops describe the GPU's power rail on the root port): ACPI powers a device down by turning off what
// its D3hot state needs (_PR3) and up by turning on what D0 needs (_PR0). Without any resources the set only runs _PS3/_PS0, which some
// firmware uses to cut the power itself (NVIDIA Optimus).
//
// Shared resources are counted (AcpiPowerResourceRegistry): read() introduces the GPU as a holder of its _PR0 and _PR3 resources,
// power_off() drops it and runs _OFF only for those no other holder needs (another GPU, another device's root port on the same rail),
// power_on() takes them again. The holder is the GPU's Device, or the Device holding the resources when the GPU has none.
//
// read() also reads the PCI firmware _DSM's readiness durations (function 9) of the GPU's Device, else of the bridge's, for
// power_on_delay_ms().
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
  // The holder this set counts as in the registry (the GPU's Device, else the Device holding the resources).
  acpi::Node* identity() const { return device_ ? device_ : holder_; }
  // Counts in `registry` instead of the global one (before read(); tests). Not owned; must outlive the set.
  void use_registry(AcpiPowerResourceRegistry& registry) { registry_ = &registry; }
  AcpiPowerResourceRegistry& registry() const { return *registry_; }
  // Function 9 of the PCI firmware _DSM (the GPU's own, else the bridge's), and the D3cold -> D0 wait it gives (100 ms without it).
  const Optional<PciDeviceReadiness>& readiness() const { return readiness_; }
  uint32_t power_on_delay_ms() const { return AcpiPciReadiness::d3cold_delay_ms(readiness_); }

  // _PS3 of the GPU (then of the bridge, when it holds the resources), then _OFF of every _PR3 resource that is on (by _STA, a resource
  // without _STA counts as on) and that no other holder needs, last first. Stops at the first fault: the GPU stays in D3hot (and keeps
  // its claim on the resources). Takes the lock.
  [[nodiscard]] bool power_off(acpi::AmlInterpreter& aml) const;
  // The GPU needs every resource again; _ON of every resource of _PR0 and then of the _PR3 ones not in _PR0 that are off, first first,
  // then _PS0 of the bridge (when it holds the resources) and of the GPU. Goes on after a fault (to bring up as much as possible) and
  // returns false. Takes the lock.
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
  AcpiPowerResourceRegistry* registry_ = &AcpiPowerResourceRegistry::global();
  Optional<PciDeviceReadiness> readiness_;
};

}  // namespace hardware
