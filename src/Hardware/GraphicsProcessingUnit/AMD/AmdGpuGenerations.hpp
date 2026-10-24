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

// The graphics architecture of an AMD GPU from the Ryzen era on: the Ryzen APUs' integrated graphics and the discrete Radeon / Instinct cards
// sold beside them.
enum class AmdGpuArchitecture : uint8_t {
  Unknown,
  Polaris,  // GCN 4: Radeon RX 400/500
  Vega,     // GCN 5: Ryzen 2000-5000 APUs (Raven, Picasso, Renoir, Cezanne), Radeon RX Vega, Radeon VII
  Cdna,     // compute-only Instinct accelerators (MI100 and later): no display, no video engine
  Rdna1,    // Radeon RX 5000
  Rdna2,    // Radeon RX 6000; Ryzen 6000 (Rembrandt), 7000 (Raphael) and Mendocino
  Rdna3,    // Radeon RX 7000; Ryzen 7040/8040 (Phoenix, Hawk Point)
  Rdna3_5,  // Ryzen AI 300 and Max (Strix Point, Strix Halo, Krackan Point)
  Rdna4,    // Radeon RX 9000
};

struct AmdGpuInfo {
  AmdGpuArchitecture architecture = AmdGpuArchitecture::Unknown;
  const char* name = "AMD graphics";  // architecture and the parts it ships in; static storage
  bool discrete = false;              // a card with memory of its own, not the graphics of an APU
  // The video engine of this generation (UVD/VCE, then VCN) decodes/encodes (see AmdVcnVideoEngine). H.264 and JPEG are on every part that has an engine.
  bool media_hevc = false;
  bool media_vp9 = false;
  bool media_av1_decode = false;
  bool media_av1_encode = false;
};

// Classifies an AMD (PCI vendor 0x1002) display device by its PCI device ID. The IDs are grouped by the ranges AMD allocates to each chip (as
// the Linux amdgpu driver lists them); an ID outside every range gives AmdGpuArchitecture::Unknown.
AmdGpuInfo amd_gpu_identify(uint16_t device_id);

// Does the architecture come with a video engine? (Unknown and the compute-only parts do not.)
bool amd_gpu_has_video_engine(AmdGpuArchitecture architecture);

}  // namespace hardware
