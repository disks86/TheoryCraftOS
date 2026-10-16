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

// AMD's XDNA NPU in Ryzen AI processors (Phoenix, Hawk Point, Strix). This
// class names the device; the computation still runs on the CPU until a native
// driver exists, and this is where it will go.
class AmdXdnaNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x1022;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "AMD XDNA NPU (software path)"; }
  static bool matches(uint16_t device_id) {
    return device_id == 0x1502 ||  // Phoenix / Hawk Point
           device_id == 0x17F0;    // Strix and later
  }
};

}  // namespace hardware
