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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"

namespace hardware {

namespace {
using A = IntelGpuArchitecture;

struct Range {
  uint16_t lo, hi;  // inclusive PCI device IDs
  IntelGpuInfo info;
};

// Media engine (Quick Sync) support: Gen9.5 and Gen11 do HEVC and VP9; Xe-LP adds AV1 decode; Arc, Xe-LPG and later add
// AV1 encode. The compute-only Xe-HPC has no video engine.
constexpr IntelGpuInfo make(A a, const char* name, bool discrete, bool av1_decode, bool av1_encode) {
  return {a, name, discrete, /*hevc=*/true, /*vp9=*/true, av1_decode, av1_encode};
}

constexpr Range kRanges[] = {
    // Gen9.5
    {0x5900, 0x593F, make(A::Gen9_5, "Intel HD Graphics 6xx (Gen9.5, Kaby Lake / Kaby Lake-G)", false, false, false)},
    {0x3E90, 0x3EAF, make(A::Gen9_5, "Intel UHD Graphics (Gen9.5, Coffee/Whiskey Lake)", false, false, false)},
    {0x87C0, 0x87CF, make(A::Gen9_5, "Intel UHD Graphics (Gen9.5, Coffee Lake ULX)", false, false, false)},
    {0x9B21, 0x9BFF, make(A::Gen9_5, "Intel UHD Graphics (Gen9.5, Comet Lake)", false, false, false)},
    // Gen11
    {0x8A50, 0x8A7F, make(A::Gen11, "Intel Iris Plus / UHD Graphics (Gen11, Ice Lake)", false, false, false)},
    {0x9840, 0x9841, make(A::Gen11, "Intel UHD Graphics (Gen11, Lakefield)", false, false, false)},
    {0x4541, 0x4571, make(A::Gen11, "Intel UHD Graphics (Gen11, Elkhart Lake)", false, false, false)},
    {0x4E51, 0x4E71, make(A::Gen11, "Intel UHD Graphics (Gen11, Jasper Lake)", false, false, false)},
    // Xe-LP
    {0x4905, 0x4908, make(A::XeLp, "Intel Iris Xe MAX / DG1 (Xe-LP)", true, true, false)},
    {0x4C80, 0x4C9F, make(A::XeLp, "Intel UHD Graphics 750 (Xe-LP, Rocket Lake)", false, true, false)},
    {0x9A40, 0x9AFF, make(A::XeLp, "Intel Iris Xe / UHD Graphics (Xe-LP, Tiger Lake)", false, true, false)},
    {0x4600, 0x46FF, make(A::XeLp, "Intel Iris Xe / UHD Graphics (Xe-LP, Alder Lake)", false, true, false)},
    {0xA700, 0xA7FF, make(A::XeLp, "Intel Iris Xe / UHD Graphics (Xe-LP, Raptor Lake)", false, true, false)},
    // Xe-HPG (Arc A-series, Flex)
    {0x5690, 0x56CF, make(A::XeHpg, "Intel Arc Graphics (Xe-HPG, Alchemist)", true, true, true)},
    // Xe-HPC
    {0x0BD0, 0x0BDF, {A::XeHpc, "Intel Data Center GPU Max (Xe-HPC)", true, false, false, false, false}},
    // Xe-LPG (Meteor Lake, Arrow Lake)
    {0x7D40, 0x7DDF, make(A::XeLpg, "Intel Arc Graphics (Xe-LPG, Meteor/Arrow Lake)", false, true, true)},
    // Xe2 (Lunar Lake, Battlemage)
    {0x6420, 0x64BF, make(A::Xe2, "Intel Arc Graphics (Xe2, Lunar Lake)", false, true, true)},
    {0xE200, 0xE2FF, make(A::Xe2, "Intel Arc Graphics (Xe2, Battlemage)", true, true, true)},
    // Xe3 (Panther Lake, Wildcat Lake)
    {0xB080, 0xB0BF, make(A::Xe3, "Intel Arc Graphics (Xe3, Panther Lake)", false, true, true)},
    {0xFD80, 0xFD8F, make(A::Xe3, "Intel Graphics (Xe3, Wildcat Lake)", false, true, true)},
};
}  // namespace

IntelGpuInfo intel_gpu_identify(uint16_t device_id) {
  for (const Range& r : kRanges)
    if (device_id >= r.lo && device_id <= r.hi) return r.info;
  return {};
}

}  // namespace hardware
