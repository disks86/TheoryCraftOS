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
#include "Hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"

namespace hardware {

// Fallback GPU: drives the framebuffer the firmware set up through the UEFI
// Graphics Output Protocol. Mode setting is delegated to the firmware, so it
// works on any display hardware but only offers the modes the firmware lists
// (32-bit RGB/BGR ones). The vendor GPU classes build on this until they
// grow native drivers.
//
// It is also the monitor source of the framebuffer: one connector, the display the firmware set up, with the EDID the firmware read from it
// (the EDID active / discovered protocols) when it has one.
class UefiGopGpu : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint32_t kMaxModes = 64;

  explicit UefiGopGpu(const GpuContext& context) : st_(context.st) {}

  const char* name() const override { return "UEFI framebuffer"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t mode_count() const override { return mode_count_; }
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;
  IMonitorSource* monitor_source() override { return this; }

  uint32_t connector_count() const override { return 1; }
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override;
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override;

 private:
  bool apply_current_mode();

  efi::SystemTable* st_;
  efi::GraphicsOutput* gop_ = nullptr;
  uint32_t gop_mode_[kMaxModes] = {};  // our index -> GOP mode number
  DisplayMode size_[kMaxModes] = {};
  uint32_t mode_count_ = 0;
  uint32_t current_ = 0;
};

}  // namespace hardware
