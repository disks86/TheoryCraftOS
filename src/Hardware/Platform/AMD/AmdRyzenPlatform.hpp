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

#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/VideoCodec/AMD/AmdIpDiscovery.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnGenerations.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

// The CPU core of an AMD processor of the Zen era, as the Linux kernel classifies family/model pairs (arch/x86/kernel/cpu/amd.c: X86_FEATURE_ZEN1..6).
enum class AmdZenCore : uint8_t { Unknown, Zen, ZenPlus, Zen2, Zen3, Zen4, Zen5, Zen6 };
AmdZenCore amd_zen_core(uint32_t family, uint32_t model);
const char* amd_zen_core_name(AmdZenCore core);

// The AMD NPU generations (AI Engine arrays; see NeuralProcessingUnit/AMD).
enum class AmdNpuGeneration : uint8_t { None, Xdna, Xdna2 };

// Every Ryzen system-on-chip: the die (or the I/O die, for the chiplet parts) a Ryzen, Ryzen Threadripper or Ryzen AI processor is built from.
enum class AmdRyzenSoc : uint8_t {
  Unknown,
  SummitRidge,    // Ryzen 1000, Threadripper 1000 (Whitehaven)
  RavenRidge,     // Ryzen 2000 APUs
  PinnacleRidge,  // Ryzen 2000, Threadripper 2000 (Colfax)
  Picasso,        // Ryzen 3000 APUs
  Dali,           // Ryzen 3000U entry (Dali, Pollock): "Raven2"
  CastlePeak,     // Threadripper 3000
  Matisse,        // Ryzen 3000
  CyanSkillfish,  // Ryzen 4700S desktop kit (a console APU with its graphics off)
  Renoir,         // Ryzen 4000 APUs
  Lucienne,       // Ryzen 5000U on Zen 2
  VanGogh,        // the Steam Deck APU (custom APU 0405/0932)
  Mendocino,      // Ryzen 7020
  Chagall,        // Threadripper 5000
  Vermeer,        // Ryzen 5000
  Rembrandt,      // Ryzen 6000, Ryzen 7035 (Rembrandt-R)
  Cezanne,        // Ryzen 5000 APUs, Ryzen 7030 (Barcelo, Barcelo-R)
  StormPeak,      // Threadripper 7000
  Raphael,        // Ryzen 7000, Ryzen 7045 (Dragon Range)
  Phoenix,        // Ryzen 7040, Ryzen 8000G, Ryzen Z1
  HawkPoint,      // Ryzen 8040, Ryzen AI 200
  TurinShimada,   // Threadripper 9000 (Shimada Peak), on the Turin I/O die (EPYC 9005 shares the models)
  StrixPoint,     // Ryzen AI 300
  GraniteRidge,   // Ryzen 9000, Ryzen 9000HX (Fire Range)
  KrackanPoint,   // Ryzen AI 5/7 300
  StrixHalo,      // Ryzen AI Max
  Zen6Client,     // a Zen 6 part the catalogue knows only by its core
};

// What the catalogue knows about one SoC. Versions are the hardware block versions amdgpu finds in the IP discovery table of that SoC (GC graphics,
// DCN display, VCN video); a machine's own table is the authority, and this is what to expect (and to fall back on before the table can be read).
struct AmdRyzenSocInfo {
  AmdRyzenSoc soc = AmdRyzenSoc::Unknown;
  const char* codename = "AMD processor";
  const char* products = "";
  AmdZenCore core = AmdZenCore::Unknown;

  // Integrated graphics: none on the parts without one (Summit Ridge, Pinnacle Ridge, Matisse, Vermeer, the Threadrippers).
  AmdGpuArchitecture graphics = AmdGpuArchitecture::Unknown;
  const char* graphics_name = nullptr;
  AmdIpVersion gc, dcn, vcn;
  uint8_t vcn_instances = 0;
  ArrayView<const uint16_t> gpu_device_ids;   // PCI device IDs (vendor 0x1002) of the integrated GPU

  // The NPU (PCI vendor 0x1022), on Ryzen AI parts. Not every part of such a SoC has it enabled (Phoenix2 and Hawk Point 2 parts have none).
  AmdNpuGeneration npu = AmdNpuGeneration::None;
  uint16_t npu_device_id = 0;
  uint8_t npu_revision = 0;

  bool has_graphics() const { return graphics != AmdGpuArchitecture::Unknown; }
  bool has_video_engine() const { return vcn.major != 0; }
  bool has_npu() const { return npu != AmdNpuGeneration::None; }
  // The video engine of the SoC, as amd_vcn_info describes its version (with the firmware file of this SoC).
  AmdVcnInfo video_engine() const;
};

// Picks the SoC from the CPUID family and model. Where two SoCs share a model range (Phoenix and Hawk Point), the PCI device ID of the integrated GPU
// decides (`gpu_device_id`, 0 when not known: the first SoC of the range is taken). Unknown family/model: soc == Unknown, core still classified.
AmdRyzenSocInfo amd_ryzen_identify(uint32_t family, uint32_t model, uint16_t gpu_device_id = 0);
// The SoC an integrated GPU belongs to, from its PCI device ID; soc == Unknown for a discrete card or an ID the catalogue does not know.
AmdRyzenSocInfo amd_ryzen_soc_of_gpu(uint16_t gpu_device_id);
// The SoC an NPU belongs to, from its PCI device ID and revision (the XDNA 2 NPUs share 0x17F0 and differ by revision).
AmdRyzenSocInfo amd_ryzen_soc_of_npu(uint16_t npu_device_id, uint8_t revision);

const char* amd_ryzen_soc_name(AmdRyzenSoc soc);
const char* amd_npu_generation_name(AmdNpuGeneration generation);

// The machine as a whole: the SoC from the CPU, refined by the integrated GPU and the NPU found on the PCI bus (reads only: the device list and the NPU's
// revision byte).
struct AmdRyzenMachine {
  AmdRyzenSocInfo soc;
  uint16_t gpu_device_id = 0;   // the integrated GPU of the SoC on the bus (vendor 0x1002), 0 when none was found
  PciAddress gpu_address{};
  uint16_t npu_device_id = 0;   // the NPU (vendor 0x1022), 0 when none was found
  uint8_t npu_revision = 0;
  PciAddress npu_address{};
  bool gpu_found() const { return gpu_device_id != 0; }
  bool npu_found() const { return npu_device_id != 0; }
};
AmdRyzenMachine amd_ryzen_identify_machine(uint32_t family, uint32_t model, const IPciBus& pci);

// One line for the boot log: "Strix Point (Ryzen AI 300 ...), Zen 5; Radeon 880M/890M (RDNA 3.5), GC 11.5.0, DCN 3.5.0, VCN 4.0.5 (vcn_4_0_5.bin);
// XDNA 2 NPU 17F0 rev 10". Parts that were expected but not found on the bus say so.
void amd_ryzen_describe(const AmdRyzenMachine& machine, FixedString<256>& out);

// Every catalogue entry, for listing (tests, the inventory).
uint32_t amd_ryzen_soc_count();
AmdRyzenSocInfo amd_ryzen_soc_at(uint32_t index);

}  // namespace hardware
