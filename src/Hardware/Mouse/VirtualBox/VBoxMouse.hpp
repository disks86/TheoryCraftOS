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
#include "Hardware/Mouse/MouseBase.hpp"
#include "Hardware/VirtualBox/VmmDev.hpp"

namespace hardware {

// VirtualBox mouse integration. The host sends the pointer's absolute position through the VMMDev device but not the buttons: those still
// arrive as the emulated PS/2 mouse's reports. This mouse takes over that PS/2 mouse - it reads its buttons and wheel - and turns the pair
// into absolute events. While the host is not handing out the pointer (integration off, or the VM has captured it) the PS/2 mouse's own
// relative movement passes through unchanged.
class VBoxMouse final : public MouseBase {
 public:
  VBoxMouse(VmmDev& vmm, IMouse& ps2) : vmm_(vmm), ps2_(ps2) {}

  const char* name() const override { return "VirtualBox pointer integration"; }
  bool initialize() override { return true; }
  void shutdown() override {}   // (the PS/2 mouse and the VMMDev device belong to others)

  bool is_absolute() const override { return true; }
  uint32_t max_x() const override { return VmmDev::kPointerMax; }
  uint32_t max_y() const override { return VmmDev::kPointerMax; }

 private:
  void fetch() override;
  void push_position(int32_t wheel);

  VmmDev& vmm_;
  IMouse& ps2_;
  uint32_t x_ = 0, y_ = 0;
  uint8_t buttons_ = 0;
  bool have_position_ = false;
};

}  // namespace hardware
