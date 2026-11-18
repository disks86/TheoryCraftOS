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

// Which NVIDIA chip this is, for every generation nouveau knows (nvkm/engine/device/base.c: nv04 to Blackwell), from NV_PMC_BOOT_0 (and
// NV_PMC_BOOT_42 from Turing on). The family decides which implementation of each subsystem runs, exactly as nouveau's per-chip function
// tables do; `chipset` is the number nouveau names chips by (0x50 = G80, 0xc0 = GF100, 0xe4 = GK104, 0x117 = GM107, 0x132 = GP102,
// 0x162 = TU104, 0x172 = GA102, 0x192 = AD102, 0x1b2 = GB202).
#pragma once
#include <stdint.h>

namespace hardware {
namespace nv {

enum class Family : uint8_t {
  Unknown,
  Nv04, Nv10, Nv20, Nv30, Nv40,     // Fahrenheit .. Curie (pre-unified)
  Tesla,                            // NV50/G8x-GT2xx (chipset 0x50-0xaf)
  Fermi,                            // GF1xx (0xc0-0xdf)
  Kepler,                           // GK1xx (0xe0-0xff, 0x100-0x10f)
  Maxwell,                          // GM1xx/GM2xx (0x110-0x12f)
  Pascal,                           // GP1xx (0x130-0x13f)
  Volta,                            // GV100 (0x140)
  Turing,                           // TU1xx (0x160-0x16f)
  Ampere,                           // GA1xx (0x170-0x17f)
  Hopper,                           // GH100 (0x180)
  Ada,                              // AD10x (0x190-0x19f)
  Blackwell,                        // GB1xx/GB2xx (0x1a0-0x1bf)
};

struct Chip {
  uint32_t boot0 = 0;
  uint32_t chipset = 0;             // nouveau's number
  uint8_t revision = 0;
  Family family = Family::Unknown;
  const char* name = "unknown";     // "GK104", "TU106" ...
  // Where each part of the chip's support stands here (what nouveau runs without NVIDIA's signed firmware, and what needs it).
  bool needs_gsp = false;           // Ampere and later: nearly everything goes through the GSP firmware (nouveau requires it from GA102 on)
  bool display_native = false;      // the display engine can be programmed directly (Tesla .. Turing without GSP)
  // (added by NvChip.cpp's table)
  bool integrated = false;          // an IGP: the nForce/MCP chipset graphics (NV1A, NV1F, C51, C61, MCP7x, MCP89) or a Tegra GPU
  bool tegra = false;               // a Tegra GPU (GK20A, GM20B, GP10B): no PCI, no VBIOS, its display is the SoC's
  bool has_display = true;          // nouveau's table gives the chip a display engine (not GP100's sibling GA100, GH100, GB100, the Tegras)
  uint16_t card_type = 0;           // nouveau's card_type: 0x04, 0x10, 0x11 (NV11-style NV1x), 0x20, 0x30, 0x40, 0x50, 0xc0, 0xe0, 0x110, 0x130, 0x140,
                                    // 0x160, 0x170, 0x180, 0x190, 0x1a0 (0 unknown). The shadow and DCB code branch on it as nouveau does.
};

// Decodes NV_PMC_BOOT_0 (and, from Turing on, BOOT_42) into the chip. A powered-down GPU (all ones) or an unknown chipset gives Family::Unknown.
Chip identify_chip(uint32_t boot0, uint32_t boot42);
const char* family_name(Family family);

constexpr uint32_t kPmcBoot0 = 0x000000, kPmcBoot42 = 0x000a00, kPmcEnable = 0x000200;

}  // namespace nv
}  // namespace hardware
