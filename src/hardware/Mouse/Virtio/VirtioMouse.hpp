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
#include "hardware/Bus/Virtio/VirtioInputDevice.hpp"
#include "hardware/Mouse/MouseBase.hpp"

namespace hardware {

// QEMU's virtio-mouse-pci (relative) and virtio-tablet-pci (absolute).
class VirtioMouse final : public MouseBase {
 public:
  VirtioMouse(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
              efi::SystemTable* st)
      : device_(pci, device, allocator, st) {}

  const char* name() const override { return device_.kind() == VirtioInputKind::Tablet ? "virtio tablet" : "virtio mouse"; }
  bool initialize() override {
    if (!device_.initialize()) return false;
    return device_.kind() == VirtioInputKind::Mouse || device_.kind() == VirtioInputKind::Tablet;
  }
  void shutdown() override { device_.shutdown(); }

  bool is_absolute() const override { return device_.kind() == VirtioInputKind::Tablet; }
  uint32_t max_x() const override { return device_.absolute_max(0); }
  uint32_t max_y() const override { return device_.absolute_max(1); }

 private:
  void fetch() override;

  VirtioInputDevice device_;
  // Values accumulate until the device sends its sync marker.
  int32_t dx_ = 0, dy_ = 0, wheel_ = 0;
  uint32_t x_ = 0, y_ = 0;
  uint8_t buttons_ = 0;
};

}  // namespace hardware
