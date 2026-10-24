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
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {

// What the first look at an NVIDIA GPU found.
struct NvidiaChipStatus {
  bool probed = false;          // the chip ID was read, is consistent and names an architecture this OS knows (false: no window, GPU off, or an unknown chip)
  uint32_t boot0 = 0;           // NV_PMC_BOOT_0 as read
  uint32_t chipset = 0;         // bits 28:20 of it
  uint8_t stepping_major = 0;   // the chip's revision: A1 is major 1 / minor 1
  uint8_t stepping_minor = 0;
  NvidiaGpuArchitecture architecture = NvidiaGpuArchitecture::Unknown;
  const char* chip = nullptr;   // "GA102", ...; nullptr if the chipset is one the table has no name for (static storage)
  bool boot42_present = false;  // the chip repeats its ID in NV_PMC_BOOT_42 (Turing and newer)
  bool has_nvenc = false;       // the chip has NVENC (nvidia_chip_has_nvenc)
};

// The look at an NVIDIA GPU's identity registers. NVIDIA documents little of the GPU's inside: the chip ID registers at the start of BAR0 are the part that is
// public (nouveau and NVIDIA's open-gpu-kernel-modules both read them), so this is all the probe does - it reads, never writes. The engines themselves
// (NVDEC, NVENC) are brought up by firmware NVIDIA signs and does not publish (the GSP from Turing on, secure falcon microcode before), so there is no wake-up
// handshake to run the way Intel's forcewake allows, and nothing is submitted to the engine; see NvidiaVideoEngine.
class NvidiaMediaEngineHw {
 public:
  // The registers the probe touches all lie below this offset; a smaller window is refused.
  static constexpr uint64_t kRegisterWindow = kNvidiaRegisterWindow;

  // Reads NV_PMC_BOOT_0 and, when the chip it names is Turing or newer, NV_PMC_BOOT_42, which must agree with it. A GPU that is off reads all ones, a window
  // that is not the GPU reads zeros; both, a disagreement between the two registers, and a chip of an architecture the table lacks leave probed false.
  static NvidiaChipStatus probe(const MmioRegion& registers);
};

}  // namespace hardware
