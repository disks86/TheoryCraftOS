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
#include "Hardware/Bus/Ps2/I8042Controller.hpp"
#include "Hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// A PS/2 keyboard on the i8042. The controller translates the keyboard's scan
// code set 2 into set 1, which is decoded here (including the 0xE0 extended
// keys) into HID usages.
class Ps2Keyboard final : public KeyboardBase {
 public:
  explicit constexpr Ps2Keyboard(I8042Controller& controller) : controller_(controller) {}

  const char* name() const override { return "PS/2 keyboard"; }
  bool initialize() override;  // false if no keyboard answers
  void shutdown() override {}

 private:
  void fetch() override;

  I8042Controller& controller_;
  bool extended_ = false;   // saw 0xE0
  uint8_t skip_ = 0;        // bytes of the Pause sequence still to ignore
};

}  // namespace hardware
