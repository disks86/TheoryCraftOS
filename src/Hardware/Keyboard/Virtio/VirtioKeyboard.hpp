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
#include "Hardware/Bus/Virtio/VirtioInputDevice.hpp"
#include "Hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// QEMU's virtio-keyboard-pci (and any virtio-input device that reports keys).
class VirtioKeyboard final : public KeyboardBase {
 public:
  VirtioKeyboard(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : device_(pci, device, allocator, st) {}

  const char* name() const override { return "virtio keyboard"; }
  bool initialize() override { return device_.initialize() && device_.kind() == VirtioInputKind::Keyboard; }
  void shutdown() override { device_.shutdown(); }

 private:
  void fetch() override;

  VirtioInputDevice device_;
};

}  // namespace hardware
