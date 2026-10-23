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

// The graphics architecture of an Intel GPU, 9th-generation Core and newer (including the discrete Arc and Data Center parts).
enum class IntelGpuArchitecture : uint8_t {
  Unknown,
  Gen9_5,  // UHD Graphics 6xx: Coffee Lake, Whiskey Lake, Comet Lake
  Gen11,   // Iris Plus / UHD: Ice Lake, Lakefield, Elkhart Lake, Jasper Lake
  XeLp,    // Xe-LP (Gen12): Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake, DG1
  XeHpg,   // Xe-HPG: Arc Alchemist (A-series), Flex series
  XeHpc,   // Xe-HPC: Data Center GPU Max (Ponte Vecchio)
  XeLpg,   // Xe-LPG: Meteor Lake and Arrow Lake
  Xe2,     // Xe2: Lunar Lake and Arc Battlemage (B-series)
  Xe3,     // Xe3: Panther Lake and Wildcat Lake
};

struct IntelGpuInfo {
  IntelGpuArchitecture architecture = IntelGpuArchitecture::Unknown;
  const char* name = "Intel graphics";  // architecture and the parts it ships in; static storage
  bool discrete = false;                // a card with memory of its own, not the graphics tile of a CPU
  // The media engine of this generation decodes/encodes (see IntelMediaVideoEngine).
  bool media_hevc = false;
  bool media_vp9 = false;
  bool media_av1_decode = false;
  bool media_av1_encode = false;
};

// Classifies an Intel (PCI vendor 0x8086) display device by its PCI device ID. The IDs are grouped by the
// ranges Intel allocates to each generation; an ID outside every range gives IntelGpuArchitecture::Unknown.
IntelGpuInfo intel_gpu_identify(uint16_t device_id);

}  // namespace hardware
