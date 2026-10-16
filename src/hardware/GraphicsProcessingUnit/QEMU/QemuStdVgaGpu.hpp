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
#include "hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

// QEMU's "std" VGA (PCI 1234:1111) via the Bochs VBE interface: real mode
// setting by programming the dispi registers, with the framebuffer in PCI BAR 0.
class QemuStdVgaGpu final : public FramebufferGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x1234;
  static constexpr uint16_t kPciDevice = 0x1111;

  explicit QemuStdVgaGpu(const GpuContext& context) : pci_(context.pci), device_(context.device) {}

  const char* name() const override { return "QEMU std VGA"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;

 private:
  uint16_t read_register(uint16_t index) const;
  void write_register(uint16_t index, uint16_t value) const;

  const IPciBus* pci_;
  PciDeviceInfo device_;
  uint8_t* framebuffer_ = nullptr;
  uint32_t current_ = 0;
  IoPort index_port_{0x1CE};
  IoPort data_port_{0x1CF};
};

}  // namespace hardware
