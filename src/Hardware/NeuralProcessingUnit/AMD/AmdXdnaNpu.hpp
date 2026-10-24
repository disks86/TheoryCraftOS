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
#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// AMD's XDNA NPU in Ryzen AI processors (Phoenix, Hawk Point: XDNA; Strix, Krackan: XDNA 2). This
// class names the device; the computation still runs on the CPU until a native
// driver exists, and this is where it will go.
class AmdXdnaNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x1022;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "AMD XDNA NPU (software path)"; }
  // XDNA (Ryzen 7040/8040) and XDNA 2 (Ryzen AI 300 and later); 0 for a device that is neither.
  static uint32_t xdna_generation(uint16_t device_id) {
    return device_id == 0x1502 ? 1 : device_id == 0x17F0 ? 2 : 0;
  }
  static bool matches(uint16_t device_id) { return xdna_generation(device_id) != 0; }
  // The Ryzen parts the generation ships in, or nullptr for a device that is not a known XDNA NPU.
  static const char* platform(uint16_t device_id) {
    switch (xdna_generation(device_id)) {
      case 1: return "XDNA, Ryzen 7040/8040";
      case 2: return "XDNA 2, Ryzen AI 300";
      default: return nullptr;
    }
  }
};

}  // namespace hardware
