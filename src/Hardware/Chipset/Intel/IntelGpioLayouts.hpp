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

// Where the GPIO pads of each Intel PCH and SoC are: the pads are split into communities, each a memory range of the GPIO controller (the order of the
// ranges in its _CRS), and the pins of a community into pad groups. ACPI (GpioInt, GpioIo) names a pad by its GPIO number, which is the pin number on
// Sunrise Point and Lewisburg but, from Cannon Lake on, starts every group at a multiple of 32 and leaves some groups (SPI, JTAG, CPU) without numbers.
// The tables are generated from the Linux pinctrl drivers by scripts/gen_intel_gpio_layouts.py.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

struct IntelGpioGroup {
  uint16_t first_pin, last_pin;
  int16_t gpio_base;   // the GPIO number of first_pin; -1 when the group's pins have none
};

struct IntelGpioCommunityLayout {
  uint8_t bar;                        // which memory range of the controller's _CRS
  uint16_t first_pin, last_pin;
  ArrayView<const IntelGpioGroup> groups;
};

struct IntelGpioLayout {
  const char* hid;                    // the controller's ACPI _HID
  const char* uid;                    // its _UID where one _HID covers several controllers (Apollo Lake, Elkhart Lake...); nullptr: any
  const char* name;
  ArrayView<const IntelGpioCommunityLayout> communities;
};

// The layout for a controller; nullptr when the catalogue does not know it. `uid` may be nullptr when the device has no _UID.
const IntelGpioLayout* intel_gpio_layout(const char* hid, const char* uid);
uint32_t intel_gpio_layout_count();
const IntelGpioLayout* intel_gpio_layout_at(uint32_t index);

}  // namespace hardware
