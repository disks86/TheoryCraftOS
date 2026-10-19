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

// VMware's SVGA II adapter (PCI 15AD:0405; VMware Workstation/Fusion/ESXi, QEMU's `vmware-svga`). Registers are reached through an index/value pair of
// I/O ports, the picture is a linear framebuffer in BAR 1, and changes are announced to the host as UPDATE commands in the command FIFO (BAR 2).
// It is also the monitor source of its one display, which can take any size the adapter's maximum and video memory allow.
class VmwareSvgaGpu final : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint16_t kPciVendor = 0x15AD;
  static constexpr uint16_t kPciDevice = 0x0405;
  static constexpr uint32_t kMaxModes = 24;

  explicit VmwareSvgaGpu(const GpuContext& context) : pci_(context.pci), device_(context.device) {}

  const char* name() const override { return "VMware SVGA II"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t mode_count() const override { return mode_count_; }
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;
  void present() override;
  IMonitorSource* monitor_source() override { return this; }

  uint32_t connector_count() const override { return 1; }
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override { (void)connector; (void)out; return 0; }
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override;

 private:
  uint32_t read_register(uint32_t index) const;
  void write_register(uint32_t index, uint32_t value) const;
  bool probe_ports();
  void init_fifo();
  bool program(uint32_t width, uint32_t height);
  void fifo_write(const uint32_t* words, uint32_t count);
  void sync();

  const IPciBus* pci_;
  PciDeviceInfo device_;
  uint16_t index_port_ = 0, value_port_ = 0;
  uint8_t* framebuffer_ = nullptr;
  uint32_t framebuffer_bytes_ = 0;
  volatile uint32_t* fifo_ = nullptr;
  uint32_t fifo_bytes_ = 0;
  bool fifo_ready_ = false;
  uint32_t max_width_ = 0, max_height_ = 0;
  DisplayMode modes_[kMaxModes] = {};
  uint32_t mode_count_ = 0;
  uint32_t current_ = 0;
};

}  // namespace hardware
