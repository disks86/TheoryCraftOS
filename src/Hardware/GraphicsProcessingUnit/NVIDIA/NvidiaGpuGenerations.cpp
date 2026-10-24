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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"

namespace hardware {

namespace {
using A = NvidiaGpuArchitecture;

struct Range {
  uint16_t lo, hi;  // inclusive PCI device IDs
  A architecture;
  const char* name;
};

// Listed most specific first: the first matching range wins.
constexpr Range kRanges[] = {
    // Volta: Titan V and the Tesla V100 / Quadro GV100
    {0x1D81, 0x1D81, A::Volta, "NVIDIA Titan V (Volta, GV100)"},
    {0x1DB0, 0x1DBF, A::Volta, "NVIDIA Tesla V100 / Quadro GV100 (Volta, GV100)"},
    // Pascal
    {0x15F0, 0x15FF, A::Pascal, "NVIDIA Tesla P100 (Pascal, GP100)"},
    {0x1B00, 0x1B3F, A::Pascal, "NVIDIA GeForce GTX 1080 Ti / Titan X (Pascal, GP102)"},
    {0x1B80, 0x1BBF, A::Pascal, "NVIDIA GeForce GTX 1070 / 1080 (Pascal, GP104)"},
    {0x1BC0, 0x1BFF, A::Pascal, "NVIDIA Quadro P4000 / P5000 (Pascal, GP104)"},
    {0x1C00, 0x1C5F, A::Pascal, "NVIDIA GeForce GTX 1060 (Pascal, GP106)"},
    {0x1C60, 0x1CFF, A::Pascal, "NVIDIA GeForce GTX 1050 (Pascal, GP107)"},
    {0x1D00, 0x1D7F, A::Pascal, "NVIDIA GeForce GT 1030 / MX150 (Pascal, GP108)"},
    // Turing: RTX 20 (TU102/104/106), GTX 16 (TU116/117)
    {0x1E00, 0x1EFF, A::Turing, "NVIDIA GeForce RTX 2070 / 2080 (Turing, TU102/TU104)"},
    {0x1F00, 0x1FFF, A::Turing, "NVIDIA GeForce RTX 2060 / GTX 1650 (Turing, TU106/TU117)"},
    {0x2100, 0x21FF, A::Turing, "NVIDIA GeForce GTX 1660 (Turing, TU116)"},
    // Ampere: A100, RTX 30
    {0x20B0, 0x20FF, A::Ampere, "NVIDIA A100 (Ampere, GA100)"},
    {0x2200, 0x22FF, A::Ampere, "NVIDIA GeForce RTX 3080 / 3090 (Ampere, GA102)"},
    {0x2400, 0x24FF, A::Ampere, "NVIDIA GeForce RTX 3060 Ti / 3070 (Ampere, GA104)"},
    {0x2500, 0x25FF, A::Ampere, "NVIDIA GeForce RTX 3050 / 3060 (Ampere, GA106/GA107)"},
    // Hopper
    {0x2300, 0x23FF, A::Hopper, "NVIDIA H100 / H200 (Hopper, GH100)"},
    // Ada Lovelace
    {0x2600, 0x26FF, A::AdaLovelace, "NVIDIA GeForce RTX 4090 (Ada Lovelace, AD102)"},
    {0x2700, 0x27FF, A::AdaLovelace, "NVIDIA GeForce RTX 4070 / 4080 (Ada Lovelace, AD103/AD104)"},
    {0x2800, 0x28FF, A::AdaLovelace, "NVIDIA GeForce RTX 4060 (Ada Lovelace, AD106/AD107)"},
    // Blackwell
    {0x2900, 0x29FF, A::Blackwell, "NVIDIA B100 / B200 (Blackwell, GB100)"},
    {0x2B00, 0x2BFF, A::Blackwell, "NVIDIA GeForce RTX 5090 (Blackwell, GB202)"},
    {0x2C00, 0x2CFF, A::Blackwell, "NVIDIA GeForce RTX 5080 (Blackwell, GB203)"},
    {0x2D00, 0x2DFF, A::Blackwell, "NVIDIA GeForce RTX 5060 / 5070 (Blackwell, GB205/GB206)"},
    {0x2F00, 0x2FFF, A::Blackwell, "NVIDIA GeForce RTX 5070 (Blackwell, GB205)"},
};

struct Chip {
  uint16_t chipset;
  const char* name;
};
constexpr Chip kChips[] = {
    {0x130, "GP100"}, {0x132, "GP102"}, {0x134, "GP104"}, {0x136, "GP106"}, {0x137, "GP107"}, {0x138, "GP108"},
    {0x140, "GV100"},
    {0x162, "TU102"}, {0x164, "TU104"}, {0x166, "TU106"}, {0x167, "TU117"}, {0x168, "TU116"},
    {0x170, "GA100"}, {0x172, "GA102"}, {0x173, "GA103"}, {0x174, "GA104"}, {0x176, "GA106"}, {0x177, "GA107"},
    {0x180, "GH100"},
    {0x192, "AD102"}, {0x193, "AD103"}, {0x194, "AD104"}, {0x196, "AD106"}, {0x197, "AD107"},
    {0x1A0, "GB100"}, {0x1B2, "GB202"}, {0x1B3, "GB203"}, {0x1B5, "GB205"}, {0x1B6, "GB206"}, {0x1B7, "GB207"},
};

// The chipset's architecture bits (chipset & 0x1F0) -> architecture.
A architecture_of_chipset(uint32_t chipset) {
  switch (chipset & 0x1F0) {
    case 0x130: return A::Pascal;
    case 0x140: return A::Volta;
    case 0x160: return A::Turing;
    case 0x170: return A::Ampere;
    case 0x180: return A::Hopper;
    case 0x190: return A::AdaLovelace;
    case 0x1A0:
    case 0x1B0: return A::Blackwell;
    default: return A::Unknown;
  }
}

const char* architecture_name(A a) {
  switch (a) {
    case A::Pascal: return "NVIDIA graphics (Pascal)";
    case A::Volta: return "NVIDIA graphics (Volta)";
    case A::Turing: return "NVIDIA graphics (Turing)";
    case A::Ampere: return "NVIDIA graphics (Ampere)";
    case A::Hopper: return "NVIDIA graphics (Hopper)";
    case A::AdaLovelace: return "NVIDIA graphics (Ada Lovelace)";
    case A::Blackwell: return "NVIDIA graphics (Blackwell)";
    case A::Unknown: break;
  }
  return "NVIDIA graphics";
}
}  // namespace

NvidiaGpuInfo nvidia_gpu_info_for(NvidiaGpuArchitecture architecture) {
  NvidiaGpuInfo info;
  info.architecture = architecture;
  info.name = architecture_name(architecture);
  switch (architecture) {
    case A::Unknown: break;
    case A::Pascal:
    case A::Volta:
    case A::Turing: info.media_hevc = info.media_vp9 = info.has_nvenc = true; break;  // NVDEC 2 to 4: H.264, HEVC, VP9; NVENC: H.264, HEVC
    case A::Ampere: info.media_hevc = info.media_vp9 = info.media_av1_decode = info.has_nvenc = true; break;  // NVDEC 5 adds AV1
    case A::Hopper: info.media_hevc = info.media_vp9 = info.media_av1_decode = true; break;  // decode only
    case A::AdaLovelace:
    case A::Blackwell: info.media_hevc = info.media_vp9 = info.media_av1_decode = info.has_nvenc = info.media_av1_encode = true; break;  // NVENC adds AV1
  }
  return info;
}

NvidiaGpuInfo nvidia_gpu_identify(uint16_t device_id) {
  for (const Range& r : kRanges)
    if (device_id >= r.lo && device_id <= r.hi) {
      NvidiaGpuInfo info = nvidia_gpu_info_for(r.architecture);
      info.name = r.name;
      return info;
    }
  return {};
}

NvidiaGpuInfo nvidia_gpu_identify_boot0(uint32_t boot0) {
  return nvidia_gpu_info_for(architecture_of_chipset(nvidia_boot0_chipset(boot0)));
}

const char* nvidia_chip_name(uint32_t chipset) {
  for (const Chip& c : kChips)
    if (c.chipset == chipset) return c.name;
  return nullptr;
}

bool nvidia_chip_has_nvenc(uint32_t chipset, NvidiaGpuArchitecture architecture) {
  if (chipset == 0x130 || chipset == 0x170) return false;   // GP100 and GA100
  return nvidia_gpu_info_for(architecture).has_nvenc;
}

bool nvidia_gpu_has_video_engine(NvidiaGpuArchitecture architecture) { return architecture != NvidiaGpuArchitecture::Unknown; }

}  // namespace hardware
