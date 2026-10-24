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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"

namespace hardware {

namespace {
using A = AmdGpuArchitecture;

struct Range {
  uint16_t lo, hi;  // inclusive PCI device IDs
  AmdGpuInfo info;
};

// Video engine support: Polaris (UVD 6 / VCE 3) and Vega do HEVC (Vega and the VCN parts also VP9 decode); AV1 decode arrives with VCN 3
// (RDNA2, except the entry-level Navi 24), AV1 encode with VCN 4 (RDNA3).
constexpr AmdGpuInfo make(A a, const char* name, bool discrete, bool vp9, bool av1_decode, bool av1_encode) {
  return {a, name, discrete, /*hevc=*/true, vp9, av1_decode, av1_encode};
}

constexpr Range kRanges[] = {
    // Polaris
    {0x67C0, 0x67FF, make(A::Polaris, "AMD Radeon RX 400/500 (Polaris 10/11)", true, false, false, false)},
    {0x6980, 0x699F, make(A::Polaris, "AMD Radeon RX 500 (Polaris 12)", true, false, false, false)},
    // Vega: Radeon RX Vega, Vega 12 (Pro), Radeon VII (Vega 20)
    {0x6860, 0x687F, make(A::Vega, "AMD Radeon RX Vega (Vega 10)", true, true, false, false)},
    {0x69A0, 0x69AF, make(A::Vega, "AMD Radeon Pro Vega (Vega 12)", true, true, false, false)},
    {0x66A0, 0x66AF, make(A::Vega, "AMD Radeon VII (Vega 20)", true, true, false, false)},
    // Vega: Ryzen APUs
    {0x15D8, 0x15D8, make(A::Vega, "AMD Radeon Vega (Ryzen 2000/3000, Picasso)", false, true, false, false)},
    {0x15DD, 0x15DD, make(A::Vega, "AMD Radeon Vega (Ryzen 2000, Raven Ridge)", false, true, false, false)},
    {0x15E7, 0x15E7, make(A::Vega, "AMD Radeon Vega (Ryzen 5000, Barcelo)", false, true, false, false)},
    {0x1636, 0x1636, make(A::Vega, "AMD Radeon Vega (Ryzen 4000, Renoir)", false, true, false, false)},
    {0x1638, 0x1638, make(A::Vega, "AMD Radeon Vega (Ryzen 5000, Cezanne)", false, true, false, false)},
    {0x164C, 0x164C, make(A::Vega, "AMD Radeon Vega (Ryzen 5000, Lucienne)", false, true, false, false)},
    // CDNA (compute only)
    {0x7388, 0x738F, {A::Cdna, "AMD Instinct MI100 (CDNA)", true, false, false, false, false}},
    {0x7408, 0x740F, {A::Cdna, "AMD Instinct MI200 (CDNA 2)", true, false, false, false, false}},
    {0x74A0, 0x74BF, {A::Cdna, "AMD Instinct MI300 (CDNA 3)", true, false, false, false, false}},
    // RDNA 1: Radeon RX 5000
    {0x7310, 0x731F, make(A::Rdna1, "AMD Radeon RX 5700 (RDNA 1, Navi 10)", true, true, false, false)},
    {0x7340, 0x734F, make(A::Rdna1, "AMD Radeon RX 5500 (RDNA 1, Navi 14)", true, true, false, false)},
    {0x7360, 0x736F, make(A::Rdna1, "AMD Radeon Pro 5600M (RDNA 1, Navi 12)", true, true, false, false)},
    // RDNA 2: Radeon RX 6000 and the Ryzen 6000/7000 integrated graphics
    {0x73A0, 0x73BF, make(A::Rdna2, "AMD Radeon RX 6800/6900 (RDNA 2, Navi 21)", true, true, true, false)},
    {0x73C0, 0x73DF, make(A::Rdna2, "AMD Radeon RX 6700 (RDNA 2, Navi 22)", true, true, true, false)},
    {0x73E0, 0x73FF, make(A::Rdna2, "AMD Radeon RX 6600 (RDNA 2, Navi 23)", true, true, true, false)},
    {0x7420, 0x743F, make(A::Rdna2, "AMD Radeon RX 6400/6500 (RDNA 2, Navi 24)", true, true, false, false)},
    {0x1506, 0x1506, make(A::Rdna2, "AMD Radeon Graphics (RDNA 2, Mendocino)", false, true, true, false)},
    {0x1681, 0x1681, make(A::Rdna2, "AMD Radeon 680M (RDNA 2, Ryzen 6000, Rembrandt)", false, true, true, false)},
    {0x164E, 0x164E, make(A::Rdna2, "AMD Radeon Graphics (RDNA 2, Ryzen 7000, Raphael)", false, true, true, false)},
    {0x13C0, 0x13C0, make(A::Rdna2, "AMD Radeon Graphics (RDNA 2, Ryzen 9000, Granite Ridge)", false, true, true, false)},
    // RDNA 3: Radeon RX 7000 and the Ryzen 7040/8040 integrated graphics
    {0x7440, 0x744F, make(A::Rdna3, "AMD Radeon RX 7900 (RDNA 3, Navi 31)", true, true, true, true)},
    {0x7470, 0x747F, make(A::Rdna3, "AMD Radeon RX 7700/7800 (RDNA 3, Navi 32)", true, true, true, true)},
    {0x7480, 0x748F, make(A::Rdna3, "AMD Radeon RX 7600 (RDNA 3, Navi 33)", true, true, true, true)},
    {0x15BF, 0x15BF, make(A::Rdna3, "AMD Radeon 780M (RDNA 3, Ryzen 7040, Phoenix)", false, true, true, true)},
    {0x15C8, 0x15C8, make(A::Rdna3, "AMD Radeon 780M (RDNA 3, Ryzen 8040, Hawk Point)", false, true, true, true)},
    // RDNA 3.5: Ryzen AI 300 / Max
    {0x150E, 0x150E, make(A::Rdna3_5, "AMD Radeon 890M (RDNA 3.5, Strix Point)", false, true, true, true)},
    {0x1586, 0x1586, make(A::Rdna3_5, "AMD Radeon 8060S (RDNA 3.5, Strix Halo)", false, true, true, true)},
    {0x1114, 0x1114, make(A::Rdna3_5, "AMD Radeon 860M (RDNA 3.5, Krackan Point)", false, true, true, true)},
    // RDNA 4: Radeon RX 9000
    {0x7550, 0x755F, make(A::Rdna4, "AMD Radeon RX 9070 (RDNA 4, Navi 48)", true, true, true, true)},
    {0x7590, 0x759F, make(A::Rdna4, "AMD Radeon RX 9060 (RDNA 4, Navi 44)", true, true, true, true)},
};
}  // namespace

AmdGpuInfo amd_gpu_identify(uint16_t device_id) {
  for (const Range& r : kRanges)
    if (device_id >= r.lo && device_id <= r.hi) return r.info;
  return {};
}

bool amd_gpu_has_video_engine(AmdGpuArchitecture architecture) {
  return architecture != AmdGpuArchitecture::Unknown && architecture != AmdGpuArchitecture::Cdna;
}

}  // namespace hardware
