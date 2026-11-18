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
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace nv { class NvidiaNative; }

// NVIDIA display controller (PCI vendor 0x10DE). Modes and the first framebuffer come from the firmware (UEFI GOP). On top of that the native
// driver (NvidiaNative over Nvkm/, after nouveau) identifies the chip from its registers - every generation from NV04 to Blackwell, not only the
// device IDs NvidiaGpuGenerations lists - reads the VRAM size and the temperature for the Computer Manager, and, built with
// TCOS_NVIDIA_NATIVE, takes the display over (Tesla to Turing: the firmware's mode stays, the head shows a framebuffer of the OS's own in VRAM).
// Ampere and later route their display through NVIDIA's GSP firmware, which is not run here: they stay on the firmware's framebuffer.
class NvidiaGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;

  explicit NvidiaGpu(const GpuContext& context);
  ~NvidiaGpu();

  const char* name() const override;
  const NvidiaGpuInfo& generation() const { return info_; }

  bool initialize() override;
  void shutdown() override;
  bool set_mode(uint32_t index) override;
  int32_t temperature_millicelsius() override;
  // The native driver (null when the GPU did not answer or is not known); its chip, sensors and status.
  nv::NvidiaNative* native() { return native_.get(); }
  bool native_scanout() const;

 private:
  void engage();
  void release();

  NvidiaGpuInfo info_;
  GpuContext context_;
  UniquePointer<nv::NvidiaNative> native_;
  char name_[48] = {};
};

}  // namespace hardware
