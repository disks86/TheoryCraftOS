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
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaHw.hpp"
#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// AMD's XDNA NPU in Ryzen AI processors (Phoenix, Hawk Point: XDNA; Strix, Krackan, Strix Halo: XDNA 2; the AIE4 parts the Linux driver calls npu3). This
// class is the INeuralProcessingUnit for them: the computation still runs on the CPU, since nothing compiles a graph for the AI Engine array. The hardware
// layer (AmdXdnaHw, AmdXdnaMailbox, AmdXdnaMessages, AmdXdnaDevice) can power the device up, start its firmware and talk to it.
class AmdXdnaNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x1022;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "AMD XDNA NPU (software path)"; }
  // 1 for XDNA (Ryzen 7040/8040), 2 for XDNA 2 (Ryzen AI 300 and later), 3 for the AIE4 parts (npu3); 0 for a device id the driver does not bind. By device
  // id alone; amd_xdna_identify() with the PCI revision tells the exact model.
  static uint32_t xdna_generation(uint16_t device_id) { return static_cast<uint32_t>(amd_xdna_generation(device_id)); }
  static bool matches(uint16_t device_id) { return xdna_generation(device_id) != 0; }
  // The Ryzen parts the generation ships in, or nullptr for a device that is not a known XDNA NPU.
  static const char* platform(uint16_t device_id) {
    switch (amd_xdna_generation(device_id)) {
      case AmdXdnaGeneration::Xdna: return "XDNA, Ryzen 7040/8040";
      case AmdXdnaGeneration::Xdna2: return "XDNA 2, Ryzen AI 300";
      case AmdXdnaGeneration::Aie4: return "AIE4 (npu3)";
      default: return nullptr;
    }
  }
  // The precise platform for a device id and PCI revision, or nullptr when the driver does not know that pair.
  static const char* platform(uint16_t device_id, uint8_t revision) { return amd_xdna_identify(device_id, revision).platform; }
};

}  // namespace hardware
