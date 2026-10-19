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

namespace hardware {

// VMware's absolute pointer ("vmmouse"), reached through the backdoor: while the pointer is over the VM window the host sends its absolute position
// (0..0xFFFF), and when the VM has captured the pointer it sends relative movement instead. The emulated PS/2 mouse still sends a packet for each
// report; they are read and thrown away here, so each movement is seen once.
class VmwareMouse final : public MouseBase {
 public:
  explicit VmwareMouse(IMouse& ps2) : ps2_(ps2) {}

  const char* name() const override { return "VMware pointer integration"; }
  bool initialize() override;
  void shutdown() override;

  bool is_absolute() const override { return true; }
  uint32_t max_x() const override { return 0xFFFF; }
  uint32_t max_y() const override { return 0xFFFF; }

 private:
  void fetch() override;

  IMouse& ps2_;
  bool enabled_ = false;
};

}  // namespace hardware
