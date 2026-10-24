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
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// NVIDIA display controller (PCI vendor 0x10DE), identified by device ID down to its architecture (Pascal to Blackwell; see
// NvidiaGpuGenerations.hpp). Modes and the framebuffer come from the firmware (UEFI GOP): NVIDIA does not document the display engine of these
// parts (Turing and newer are programmed through signed GSP firmware), so there is no native mode setting to write.
class NvidiaGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;

  explicit NvidiaGpu(const GpuContext& context) : UefiGopGpu(context), info_(nvidia_gpu_identify(context.device.device_id)) {}

  const char* name() const override { return info_.name; }
  const NvidiaGpuInfo& generation() const { return info_; }

 private:
  NvidiaGpuInfo info_;
};

}  // namespace hardware
