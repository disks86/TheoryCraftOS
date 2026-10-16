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
#include "hardware/Bus/Ps2/I8042Controller.hpp"
#include "hardware/Mouse/MouseBase.hpp"

namespace hardware {

// A PS/2 mouse on the i8042's second port, with the IntelliMouse extension
// (scroll wheel) when the mouse supports it.
class Ps2Mouse final : public MouseBase {
 public:
  explicit constexpr Ps2Mouse(I8042Controller& controller) : controller_(controller) {}

  const char* name() const override { return "PS/2 mouse"; }
  bool initialize() override;  // false if no mouse answers
  void shutdown() override {}

  bool is_absolute() const override { return false; }
  uint32_t max_x() const override { return 0; }
  uint32_t max_y() const override { return 0; }

 private:
  void fetch() override;

  I8042Controller& controller_;
  uint8_t packet_[4] = {};
  uint32_t packet_length_ = 3;
  uint32_t filled_ = 0;
};

}  // namespace hardware
