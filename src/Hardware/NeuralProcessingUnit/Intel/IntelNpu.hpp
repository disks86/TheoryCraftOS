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
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// Intel's NPU ("VPU"), the neural engine in Core Ultra processors: Meteor Lake,
// Arrow Lake, Lunar Lake, Panther Lake, Wildcat Lake and Nova Lake. This class names the device; the
// computation still runs on the CPU until a native driver exists, and this is
// where it will go.
class IntelNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "Intel NPU (software path)"; }
  // The platform an NPU device ID belongs to, or nullptr for a device that is not a known Intel NPU.
  static const char* platform(uint16_t device_id) { return intel_npu_platform(device_id).name; }
  static bool matches(uint16_t device_id) { return platform(device_id) != nullptr; }
};

}  // namespace hardware
