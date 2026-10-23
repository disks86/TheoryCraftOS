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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// Intel display controller (PCI vendor 0x8086), identified by device ID down to its architecture (UHD 6xx, Iris Xe, Arc,
// Xe2, Xe3; see IntelGpuGenerations.hpp). Modes and the framebuffer still
// come from the firmware (UEFI GOP); native mode setting for these parts is
// future work, and this class is where it will go.
class IntelGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  explicit IntelGpu(const GpuContext& context) : UefiGopGpu(context), info_(intel_gpu_identify(context.device.device_id)) {}

  const char* name() const override { return info_.name; }
  const IntelGpuInfo& generation() const { return info_; }

 private:
  IntelGpuInfo info_;
};

}  // namespace hardware
