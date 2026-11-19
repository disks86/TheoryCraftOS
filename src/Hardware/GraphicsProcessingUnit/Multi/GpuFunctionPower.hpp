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

// Lets the driver of one of a GPU's other functions (the HD-audio controller of a discrete card) tell the GpuManager when it suspends and resumes:
// idle lets the GPU go on to D3cold, busy brings the GPU's power back first (it can take ~100 ms) so the driver finds the function there.
#pragma once
#include "Hardware/Bus/Pci/PciFunctionPower.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"

namespace hardware {

class GpuFunctionPowerBridge final : public IPciFunctionPowerObserver {
 public:
  explicit GpuFunctionPowerBridge(GpuManager& manager) : manager_(manager) {}
  void function_idle(PciAddress function) override { (void)manager_.mark_function_busy(function, false); }
  void function_busy(PciAddress function) override { (void)manager_.mark_function_busy(function, true); }

 private:
  GpuManager& manager_;
};

}  // namespace hardware
