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

// The brightness of a display's backlight, whichever way it is driven (the firmware's ACPI methods, the PWM of the graphics chip or the PCH, a panel's own controller). Programs that change the
// brightness - the Fn keys, a settings window - use this and know none of the ways.
#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"

namespace hardware {

class IBacklight : public IHardware {
 public:
  // How many different brightness levels the backlight has (at least 2 for one that can be changed).
  virtual uint32_t steps() const = 0;
  // The current brightness in percent of the maximum, as far as the backlight can say; false if it cannot be read.
  virtual bool percent(uint8_t* out) = 0;
  // Sets the brightness (0 to 100). The nearest level the backlight has is used. False if it did not take it.
  virtual bool set_percent(uint8_t percent) = 0;

 protected:
  constexpr IBacklight() = default;
  ~IBacklight() = default;
};

}  // namespace hardware
