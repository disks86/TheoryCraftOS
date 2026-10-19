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
#include "Hardware/IoPort.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"

namespace hardware {

// QEMU's "std" VGA (PCI 1234:1111) via the Bochs VBE interface: real mode
// setting by programming the dispi registers, with the framebuffer in PCI BAR 0.
//
// It is also the monitor source of its one display: a connector that is always plugged in, with no EDID, and the modes that fit the card's video
// memory (QEMU's default is 16 MiB; start QEMU with -device VGA,vgamem_mb=64 for the larger ones).
class QemuStdVgaGpu final : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint16_t kPciVendor = 0x1234;
  static constexpr uint16_t kPciDevice = 0x1111;
  // VirtualBox's VBoxVGA (80EE:BEEF) is the same Bochs VBE interface.
  static constexpr uint16_t kVirtualBoxVendor = 0x80EE;
  static constexpr uint16_t kVirtualBoxDevice = 0xBEEF;
  static bool supports(uint16_t vendor, uint16_t device) {
    return (vendor == kPciVendor && device == kPciDevice) || (vendor == kVirtualBoxVendor && device == kVirtualBoxDevice);
  }
  static constexpr uint32_t kMaxModes = 24;

  explicit QemuStdVgaGpu(const GpuContext& context) : pci_(context.pci), device_(context.device) {}

  const char* name() const override { return device_.vendor_id == kVirtualBoxVendor ? "VirtualBox VGA" : "QEMU std VGA"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;
  IMonitorSource* monitor_source() override { return this; }

  uint32_t connector_count() const override { return 1; }
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override { (void)connector; (void)out; return 0; }
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override;

 private:
  uint16_t read_register(uint16_t index) const;
  void write_register(uint16_t index, uint16_t value) const;

  const IPciBus* pci_;
  PciDeviceInfo device_;
  uint8_t* framebuffer_ = nullptr;
  uint32_t current_ = 0;
  DisplayMode modes_[kMaxModes] = {};   // the sizes that fit the video memory
  uint32_t mode_count_ = 0;
  IoPort index_port_{0x1CE};
  IoPort data_port_{0x1CF};
};

}  // namespace hardware
