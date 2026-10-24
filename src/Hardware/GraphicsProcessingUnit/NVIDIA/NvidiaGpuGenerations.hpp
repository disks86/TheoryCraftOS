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

namespace hardware {

// The graphics architecture of an NVIDIA GPU from the Ryzen era on (Pascal, GeForce 10 series, and newer): the GeForce and RTX cards and the data
// center parts that share the chips.
//
// What this OS knows about these GPUs is what NVIDIA and the open-source drivers (nouveau, NVIDIA's open-gpu-kernel-modules) publish: the PCI device IDs
// and the chip ID in the first register of the GPU (NV_PMC_BOOT_0, which tells the architecture even for a card whose device ID is not in the table), and which codecs each
// generation of NVDEC/NVENC serves. Mode setting, the display engine and the video engines of Turing and newer are programmed through the GSP
// firmware that NVIDIA signs and does not document, so nothing here drives the hardware: the framebuffer stays with the firmware (UEFI GOP) and
// sessions run in software.
enum class NvidiaGpuArchitecture : uint8_t {
  Unknown,
  Pascal,     // GeForce GTX 10 series (GP10x)
  Volta,      // Titan V, Tesla V100 (GV100)
  Turing,     // GeForce RTX 20 and GTX 16 series (TU10x, TU11x)
  Ampere,     // GeForce RTX 30 series, A100 (GA10x)
  Hopper,     // H100, H200 (GH100): no display, no NVENC
  AdaLovelace,  // GeForce RTX 40 series (AD10x)
  Blackwell,  // GeForce RTX 50 series, B100/B200 (GB10x, GB20x)
};

struct NvidiaGpuInfo {
  NvidiaGpuArchitecture architecture = NvidiaGpuArchitecture::Unknown;
  const char* name = "NVIDIA graphics";  // architecture and the parts it ships in; static storage
  // NVDEC (decode) of this generation.
  bool media_hevc = false;
  bool media_vp9 = false;
  bool media_av1_decode = false;
  // NVENC (encode); Hopper has none.
  bool has_nvenc = false;
  bool media_av1_encode = false;
};

// The facts that depend on the architecture alone (name = the architecture's own), from the NVDEC/NVENC generation each shipped with.
NvidiaGpuInfo nvidia_gpu_info_for(NvidiaGpuArchitecture architecture);

// Classifies an NVIDIA (PCI vendor 0x10DE) display device by its PCI device ID. The IDs are grouped by the ranges NVIDIA allocates to each chip
// (as the PCI ID database lists them); an ID outside every range gives NvidiaGpuArchitecture::Unknown.
NvidiaGpuInfo nvidia_gpu_identify(uint16_t device_id);

// Classifies a GPU by the value of its NV_PMC_BOOT_0 register (offset 0 of BAR0): bits 28:20 are the chipset (0x132 = GP102, 0x192 = AD102,
// 0x1B2 = GB202), and the architecture is the chipset with its low four bits cleared. A GPU that is powered down reads all ones; any value outside the
// known chipsets gives Unknown.
NvidiaGpuInfo nvidia_gpu_identify_boot0(uint32_t boot0);
constexpr uint32_t kNvidiaBoot0Offset = 0;
constexpr uint64_t kNvidiaRegisterWindow = 0x1000;  // enough of BAR0 to read NV_PMC_BOOT_0

// The chip a chipset number (bits 28:20 of NV_PMC_BOOT_0) stands for - "GP102", "AD104", "GB202" - or nullptr for a number the table does not know.
const char* nvidia_chip_name(uint32_t chipset);
// Does the chip have NVENC? The compute-only GP100 and GA100 do not (Hopper's lack of it is an architecture-wide fact, see NvidiaGpuInfo::has_nvenc); every
// other chip of an architecture that has NVENC does, and an unknown chipset is assumed to follow its architecture.
bool nvidia_chip_has_nvenc(uint32_t chipset, NvidiaGpuArchitecture architecture);
// The chipset field of a NV_PMC_BOOT_0 value.
constexpr uint32_t nvidia_boot0_chipset(uint32_t boot0) { return (boot0 >> 20) & 0x1FF; }
// Offset of NV_PMC_BOOT_42 in BAR0: from Turing on it repeats the chip ID (architecture in bits 28:24, implementation in 23:20, revision in 7:0).
constexpr uint32_t kNvidiaBoot42Offset = 0xA00;

// Does the architecture come with a video engine? (Every known one does.)
bool nvidia_gpu_has_video_engine(NvidiaGpuArchitecture architecture);

}  // namespace hardware
