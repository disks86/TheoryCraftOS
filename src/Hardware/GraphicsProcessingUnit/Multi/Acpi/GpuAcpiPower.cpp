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

#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/GpuAcpiPower.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

const AcpiDsmUuid kNvidiaOptimusDsmUuid = {{0xF8, 0xD8, 0x86, 0xA4, 0xDA, 0x0B, 0x1B, 0x47, 0xA7, 0x2B, 0x60, 0x42, 0xA6, 0xB5, 0xBE, 0xE0}};
const AcpiDsmUuid kNvidiaJtDsmUuid = {{0x51, 0xA3, 0xEC, 0xCB, 0x7B, 0x06, 0x24, 0x49, 0x9C, 0xBD, 0xB4, 0x6B, 0x00, 0xB8, 0x6F, 0x34}};
const AcpiDsmUuid kNvidiaMuxDsmUuid = {{0xA0, 0xA0, 0x95, 0x9D, 0x60, 0x00, 0x48, 0x4D, 0xB3, 0x4D, 0x7E, 0x5F, 0xEA, 0x12, 0x9F, 0xD4}};

// --- NVIDIA

bool NvidiaOptimusPower::power_off() {
  if (!skips_dsm()) {
    kernel::ScopedRecursiveMutex guard(aml_.mutex());
    if (functions_ & (1ull << NvidiaDsm::kOptimusFlags))   // (nouveau ignores what this answers)
      (void)AcpiDsm::call_u32(aml_, dsm_, kNvidiaOptimusDsmUuid, NvidiaDsm::kOptimusRevision, NvidiaDsm::kOptimusFlags, DsmArgument::Buffer32,
                              NvidiaDsm::kOptimusFlagsArgument);
    if (!AcpiDsm::call_u32(aml_, dsm_, kNvidiaOptimusDsmUuid, NvidiaDsm::kOptimusRevision, NvidiaDsm::kOptimusCaps, DsmArgument::Buffer32,
                           NvidiaDsm::kOptimusSetPowerdown))
      return false;
  }
  return set_.power_off(aml_);
}

bool NvidiaHybridDsm::call(uint32_t function, uint32_t argument) {
  if (!(functions_ & (1ull << function))) return false;
  const Optional<uint32_t> r = AcpiDsm::call_u32(aml_, dsm_, kNvidiaMuxDsmUuid, NvidiaDsm::kMuxRevision, function, DsmArgument::Integer, argument);
  return r && *r != NvidiaDsm::kMuxUnsupported;
}

bool NvidiaHybridDsm::active(GpuMuxTarget* out) {
  if (!active_) return false;
  *out = *active_;
  return true;
}

bool NvidiaHybridDsm::switch_to(GpuMuxTarget target) {
  if (!call(NvidiaDsm::kMuxLed, target == GpuMuxTarget::Discrete ? NvidiaDsm::kMuxLedSpeed : NvidiaDsm::kMuxLedStamina)) return false;
  active_ = target;
  return true;
}

// --- Probing

namespace {

uint64_t require(uint64_t functions, uint32_t function) { return (functions & (1ull << function)) ? functions : 0; }

acpi::Node* dsm_of(acpi::AmlInterpreter& aml, acpi::Node* device) { return GpuAcpiNamespace::child_method(aml, device, "_DSM"); }

}  // namespace

GpuAcpiProbe GpuAcpiPower::probe(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge_hint, uint16_t vendor_id,
                                 const Optional<PciAddress>& integrated_gpu) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  GpuAcpiProbe p;
  p.nodes = GpuAcpiNamespace::find_gpu(aml, gpu, parent_bridge_hint);
  p.resources.read(aml, p.nodes.device, p.nodes.bridge);
  p.dsm = dsm_of(aml, p.nodes.device);
  acpi::Node* integrated = integrated_gpu ? GpuAcpiNamespace::find_pci_device(aml, *integrated_gpu) : nullptr;
  if (vendor_id == kVendorNvidia) {
    if (p.dsm) {
      p.optimus_functions = require(AcpiDsm::supported(aml, p.dsm, kNvidiaOptimusDsmUuid, NvidiaDsm::kOptimusRevision), NvidiaDsm::kOptimusCaps);
      p.jt_functions = require(AcpiDsm::supported(aml, p.dsm, kNvidiaJtDsmUuid, NvidiaDsm::kJtRevision), NvidiaDsm::kJtCaps);
    }
    // The hybrid mux _DSM: on the discrete GPU or (older machines) the integrated one, as nouveau_dsm_detect() looks at every display device.
    acpi::Node* const holders[] = {p.dsm, dsm_of(aml, integrated)};
    for (acpi::Node* dsm : holders) {
      if (!dsm) continue;
      p.mux_functions = require(AcpiDsm::supported(aml, dsm, kNvidiaMuxDsmUuid, NvidiaDsm::kMuxRevision), NvidiaDsm::kMuxPower);
      if (p.mux_functions) {
        p.mux_dsm = dsm;
        break;
      }
    }
  } else if (vendor_id == kVendorAmd) {
    // ATPX: on the discrete GPU, the integrated GPU, the root port, or (as Linux, which tries every display device) anywhere.
    acpi::Node* const holders[] = {p.nodes.device, integrated, p.nodes.bridge};
    for (acpi::Node* holder : holders)
      if (!p.atpx) p.atpx = GpuAcpiNamespace::child_method(aml, holder, "ATPX");
    if (!p.atpx) p.atpx = GpuAcpiNamespace::child_method(aml, GpuAcpiNamespace::find_device_with_method(aml, "ATPX"), "ATPX");
    if (p.atpx) p.atpx_info = AmdAtpx::read(aml, p.atpx);
  }
  return p;
}

UniquePointer<IGpuPlatformPower> GpuAcpiPower::find(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge_hint,
                                                    uint16_t vendor_id, allocators::IAllocator& allocator, const Optional<PciAddress>& integrated_gpu,
                                                    AmdAtpx::SleepFn sleep_ms) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  const GpuAcpiProbe p = probe(aml, gpu, parent_bridge_hint, vendor_id, integrated_gpu);
  const GpuPowerResourceSet& r = p.resources;
  auto plain = [&]() -> UniquePointer<IGpuPlatformPower> {
    if (!r.has_resources()) return {};
    return UniquePointer<IGpuPlatformPower>::make_as<AcpiGpuPowerResources>(allocator, aml, r);
  };
  auto jt = [&]() -> UniquePointer<IGpuPlatformPower> {
    const Optional<uint32_t> caps = AcpiDsm::call_u32(aml, p.dsm, kNvidiaJtDsmUuid, NvidiaDsm::kJtRevision, NvidiaDsm::kJtCaps, DsmArgument::Buffer32, 0);
    return UniquePointer<IGpuPlatformPower>::make_as<NvidiaJtPower>(allocator, aml, caps ? *caps : 0u, r);
  };
  if (vendor_id == kVendorNvidia) {
    if (p.jt_functions && r.has_resources()) return jt();
    if (p.optimus_functions && (r.has_resources() || r.has_ps3())) {
      // nouveau_dsm_pci_probe() reads the capabilities once (function 0x1A with 0).
      const Optional<uint32_t> caps = AcpiDsm::call_u32(aml, p.dsm, kNvidiaOptimusDsmUuid, NvidiaDsm::kOptimusRevision, NvidiaDsm::kOptimusCaps,
                                                        DsmArgument::Buffer32, 0);
      return UniquePointer<IGpuPlatformPower>::make_as<NvidiaOptimusPower>(allocator, aml, p.dsm, p.optimus_functions, caps ? *caps : 0u, r);
    }
    if (p.mux_functions)
      return UniquePointer<IGpuPlatformPower>::make_as<NvidiaHybridDsm>(allocator, aml, p.mux_dsm, p.mux_functions, r.power_on_delay_ms());
    return plain();
  }
  if (vendor_id == kVendorAmd && p.atpx_info) {
    if (p.atpx_info->hybrid || r.has_resources()) return plain();   // hybrid graphics: D3cold through _PR3, ATPX power control is off
    if (p.atpx_info->power_control)
      return UniquePointer<IGpuPlatformPower>::make_as<AmdAtpx>(allocator, aml, p.atpx, *p.atpx_info, sleep_ms, r.power_on_delay_ms());
    return {};
  }
  return plain();
}

UniquePointer<IGpuMux> GpuAcpiPower::find_mux(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge_hint,
                                              uint16_t vendor_id, allocators::IAllocator& allocator, const Optional<PciAddress>& integrated_gpu,
                                              AmdAtpx::SleepFn sleep_ms) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  const GpuAcpiProbe p = probe(aml, gpu, parent_bridge_hint, vendor_id, integrated_gpu);
  if (vendor_id == kVendorNvidia && (p.mux_functions & (1ull << NvidiaDsm::kMuxLed)))
    return UniquePointer<IGpuMux>::make_as<NvidiaHybridDsm>(allocator, aml, p.mux_dsm, p.mux_functions);
  if (vendor_id == kVendorAmd && p.atpx_info && p.atpx_info->display_mux)
    return UniquePointer<IGpuMux>::make_as<AmdAtpx>(allocator, aml, p.atpx, *p.atpx_info, sleep_ms);
  return {};
}

}  // namespace hardware
