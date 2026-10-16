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
#include "hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// Intel's NPU ("VPU"), the neural engine in Core Ultra processors: Meteor Lake,
// Arrow Lake, Lunar Lake and Panther Lake. This class names the device; the
// computation still runs on the CPU until a native driver exists, and this is
// where it will go.
class IntelNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "Intel NPU (software path)"; }
  static bool matches(uint16_t device_id) {
    return device_id == 0x7D1D ||  // Meteor Lake
           device_id == 0xAD1D ||  // Arrow Lake
           device_id == 0x643E ||  // Lunar Lake
           device_id == 0xB03E;    // Panther Lake
  }
};

}  // namespace hardware
