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
//
// The monitors: when the video BIOS was read and an EDID came over the GPU's own DDC buses or AUX channels, every connector of the board is
// listed with what is plugged into it (the one the firmware shows first: its EDID is the firmware's). The screen is still one display, the
// firmware's, so the mode calls go to that display whatever connector they name. Without that the firmware's one connector stays, as before.
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

  // IMonitorSource
  uint32_t connector_count() const override;
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override;
  bool refresh_connectors() override;
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override { (void)connector; return UefiGopGpu::mode_available(0, width, height); }
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override { (void)connector; return UefiGopGpu::device_modes(0, out, capacity); }
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override { (void)connector; return UefiGopGpu::set_display_mode(0, width, height); }

 private:
  bool use_native_connectors() const;
  void order_connectors();                    // the firmware's display first
  void engage();
  void release();

  NvidiaGpuInfo info_;
  GpuContext context_;
  UniquePointer<nv::NvidiaNative> native_;
  char name_[48] = {};
  uint8_t order_[16] = {};                    // our connector number -> the board's
  uint32_t ordered_ = 0;
};

}  // namespace hardware
