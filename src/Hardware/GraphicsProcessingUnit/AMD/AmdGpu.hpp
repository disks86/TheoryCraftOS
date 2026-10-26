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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// AMD display controller (PCI vendor 0x1002), identified by device ID down to its architecture (Vega APUs, RDNA 1 to 4, Polaris;
// see AmdGpuGenerations.hpp). Modes and the framebuffer still come from the firmware (UEFI GOP). Native mode setting is not done: the display engine (DCE/DCN)
// is brought up through the PSP and the SMU with firmware blobs this OS does not load, and without them its registers are not safe to program. This class is
// where it will go; Intel's (IntelGpu, IntelDisplay) shows how the firmware path stays the fallback.
class AmdGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x1002;

  explicit AmdGpu(const GpuContext& context) : UefiGopGpu(context), info_(amd_gpu_identify(context.device.device_id)) {}

  const char* name() const override { return info_.name; }
  const AmdGpuInfo& generation() const { return info_; }

 private:
  AmdGpuInfo info_;
};

}  // namespace hardware
