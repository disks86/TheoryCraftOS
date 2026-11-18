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

// The ACPI side of laptop hybrid graphics: the firmware methods that cut the discrete GPU's power (D3cold) and, on muxed machines, move the
// panel. After Linux: the PCI core's D3cold through ACPI power resources (drivers/pci/pci-acpi.c, drivers/acpi/power.c), nouveau_acpi.c
// (NVIDIA's Optimus, and the older "hybrid" mux _DSM) and amdgpu_atpx_handler.c (AMD's ATPX method). The NVIDIA JT _DSM is the one the
// NVIDIA driver uses on newer laptops; Linux only reads it and powers such GPUs off through the power resources, as done here.
//
// Every handler evaluates under the interpreter's lock, like the other ACPI drivers. They hold the interpreter by reference: it outlives
// them (it lives as long as the kernel).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/AmdAtpx.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/GpuAcpiNamespace.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp"
#include "Utility/Optional.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// --- NVIDIA _DSM methods (on the discrete GPU's Device).

// A486D8F8-0BDA-471B-A72B-6042A6B5BEE0, revision 0x100: Optimus ("NVOP").
extern const AcpiDsmUuid kNvidiaOptimusDsmUuid;
// CBECA351-067B-4924-9CBD-B46B00B86F34, revision 0x103: JT ("GPS"/JT, GC6 and power control of newer laptops).
extern const AcpiDsmUuid kNvidiaJtDsmUuid;
// 9D95A0A0-0060-4D48-B34D-7E5FEA129FD4, revision 0x102: the mux and power _DSM of the pre-Optimus "hybrid" laptops.
extern const AcpiDsmUuid kNvidiaMuxDsmUuid;

struct NvidiaDsm {
  static constexpr uint32_t kOptimusRevision = 0x100, kJtRevision = 0x103, kMuxRevision = 0x102;
  // Optimus functions (nouveau_acpi.c).
  static constexpr uint32_t kOptimusCaps = 0x1A, kOptimusFlags = 0x1B;
  static constexpr uint32_t kOptimusSetPowerdown = 0x03000001;   // POWERDOWN_PS3 (3 << 24) | FLAGS_CHANGED: power off when _PS3 runs
  static constexpr uint32_t kOptimusFlagsArgument = 0x3;
  static constexpr uint32_t kOptimusEnabled = 1u << 0;            // caps: Optimus enabled (bits 3-4: the current power state)
  // JT functions.
  static constexpr uint32_t kJtCaps = 0x01;
  // Mux functions (NOUVEAU_DSM_*).
  static constexpr uint32_t kMuxActive = 0x01, kMuxLed = 0x02, kMuxPower = 0x03;
  static constexpr uint32_t kMuxLedStamina = 0x11, kMuxLedSpeed = 0x12;   // panel on the integrated / discrete GPU
  static constexpr uint32_t kMuxPowerSpeed = 0x01, kMuxPowerStamina = 0x02;  // discrete GPU on / off
  static constexpr uint32_t kMuxUnsupported = 0x80000002;
};

// The power resources alone (the GPU's or its root port's _PR3), with _PS3/_PS0 around them.
class AcpiGpuPowerResources final : public IGpuPlatformPower {
 public:
  AcpiGpuPowerResources(acpi::AmlInterpreter& aml, const GpuPowerResourceSet& set, GpuPowerMechanism mechanism = GpuPowerMechanism::AcpiPowerResources)
      : aml_(aml), set_(set), mechanism_(mechanism) {}
  GpuPowerMechanism mechanism() const override { return mechanism_; }
  [[nodiscard]] bool power_off() override { return set_.power_off(aml_); }
  [[nodiscard]] bool power_on() override { return set_.power_on(aml_); }
  const GpuPowerResourceSet& resources() const { return set_; }

 private:
  acpi::AmlInterpreter& aml_;
  GpuPowerResourceSet set_;
  GpuPowerMechanism mechanism_;
};

// Optimus: before power_off, the _DSM is told to cut the power when _PS3 runs (function 0x1A with 0x03000001, after function 0x1B with 3
// where supported), then _PS3 and the power resources run. As nouveau, the _DSM is skipped when the GPU (or its root port) has _PR3: the
// power resources do the job there, and the _DSM has side effects on some of those machines.
class NvidiaOptimusPower final : public IGpuPlatformPower {
 public:
  NvidiaOptimusPower(acpi::AmlInterpreter& aml, acpi::Node* dsm, uint64_t functions, uint32_t capabilities, const GpuPowerResourceSet& set)
      : aml_(aml), dsm_(dsm), functions_(functions), capabilities_(capabilities), set_(set) {}
  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::NvidiaOptimus; }
  [[nodiscard]] bool power_off() override;
  [[nodiscard]] bool power_on() override { return set_.power_on(aml_); }
  uint64_t functions() const { return functions_; }
  // Function 0x1A with 0, read when the handler was made.
  uint32_t capabilities() const { return capabilities_; }
  bool skips_dsm() const { return set_.has_resources(); }
  const GpuPowerResourceSet& resources() const { return set_; }

 private:
  acpi::AmlInterpreter& aml_;
  acpi::Node* dsm_;
  uint64_t functions_;
  uint32_t capabilities_;
  GpuPowerResourceSet set_;
};

// JT: the capabilities are read (function 1); the power goes through the power resources.
class NvidiaJtPower final : public IGpuPlatformPower {
 public:
  NvidiaJtPower(acpi::AmlInterpreter& aml, uint32_t capabilities, const GpuPowerResourceSet& set) : aml_(aml), capabilities_(capabilities), set_(set) {}
  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::NvidiaJt; }
  [[nodiscard]] bool power_off() override { return set_.power_off(aml_); }
  [[nodiscard]] bool power_on() override { return set_.power_on(aml_); }
  uint32_t capabilities() const { return capabilities_; }
  uint32_t jt_revision() const { return capabilities_ >> 20; }   // JTREV, bits 20-31
  const GpuPowerResourceSet& resources() const { return set_; }

 private:
  acpi::AmlInterpreter& aml_;
  uint32_t capabilities_;
  GpuPowerResourceSet set_;
};

// The old NVIDIA hybrid _DSM (nouveau_dsm_switch_mux, nouveau_dsm_set_discrete_state): function 3 powers the discrete GPU on (1) or off
// (2), function 2 moves the mux (0x11 integrated, 0x12 discrete). The method has no reliable query; active() reports the last switch.
class NvidiaHybridDsm final : public IGpuPlatformPower, public IGpuMux {
 public:
  NvidiaHybridDsm(acpi::AmlInterpreter& aml, acpi::Node* dsm, uint64_t functions) : aml_(aml), dsm_(dsm), functions_(functions) {}
  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::NvidiaHybridDsm; }
  [[nodiscard]] bool power_off() override { return call(NvidiaDsm::kMuxPower, NvidiaDsm::kMuxPowerStamina); }
  [[nodiscard]] bool power_on() override { return call(NvidiaDsm::kMuxPower, NvidiaDsm::kMuxPowerSpeed); }
  [[nodiscard]] bool active(GpuMuxTarget* out) override;
  [[nodiscard]] bool switch_to(GpuMuxTarget target) override;
  bool can_switch() const { return functions_ & (1u << NvidiaDsm::kMuxLed); }
  uint64_t functions() const { return functions_; }

 private:
  [[nodiscard]] bool call(uint32_t function, uint32_t argument);

  acpi::AmlInterpreter& aml_;
  acpi::Node* dsm_;
  uint64_t functions_;
  Optional<GpuMuxTarget> active_;
};

// What was found on a GPU, for the log and the tests.
struct GpuAcpiProbe {
  GpuAcpiNodes nodes;
  GpuPowerResourceSet resources;
  acpi::Node* dsm = nullptr;          // the GPU Device's _DSM
  uint64_t optimus_functions = 0;     // function-0 bitmaps (0: not there)
  uint64_t jt_functions = 0;
  uint64_t mux_functions = 0;
  acpi::Node* mux_dsm = nullptr;      // the _DSM that answered the mux UUID (the discrete or the integrated GPU's)
  acpi::Node* atpx = nullptr;         // the ATPX method
  Optional<AmdAtpxInfo> atpx_info;
};

class GpuAcpiPower {
 public:
  // Reads everything there is about the GPU. Takes the lock.
  [[nodiscard]] static GpuAcpiProbe probe(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge_hint, uint16_t vendor_id,
                                          const Optional<PciAddress>& integrated_gpu = {});

  // The best way to cut the GPU's power, or empty if the firmware offers none:
  //  - NVIDIA (0x10DE): Optimus _DSM (JT when the machine has JT and power resources, the Optimus _DSM is skipped where there are power
  //    resources), else JT with power resources, else the old hybrid mux _DSM, else the power resources;
  //  - AMD (0x1002): the power resources when ATPX says hybrid graphics (bit 14 of its parameters: newer amdgpu turns ATPX power control off
  //    there) or when the GPU or its root port has _PR3 at all, else ATPX power control, else nothing;
  //  - anything else: the power resources.
  // `parent_bridge_hint`: the PCI bridge above the GPU (see GpuAcpiNamespace). `integrated_gpu`: where ATPX and the NVIDIA mux _DSM may sit
  // as well. `sleep_ms` waits the 200 ms ATPX needs after power off (nullptr: no wait).
  [[nodiscard]] static UniquePointer<IGpuPlatformPower> find(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge_hint,
                                                             uint16_t vendor_id, allocators::IAllocator& allocator,
                                                             const Optional<PciAddress>& integrated_gpu = {}, AmdAtpx::SleepFn sleep_ms = nullptr);
  // The panel mux, or empty if there is none the firmware drives: NVIDIA's hybrid _DSM (function 2 supported) or ATPX with display mux
  // control.
  [[nodiscard]] static UniquePointer<IGpuMux> find_mux(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge_hint,
                                                       uint16_t vendor_id, allocators::IAllocator& allocator,
                                                       const Optional<PciAddress>& integrated_gpu = {}, AmdAtpx::SleepFn sleep_ms = nullptr);

  static constexpr uint16_t kVendorNvidia = 0x10DE, kVendorAmd = 0x1002;
};

}  // namespace hardware
