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


// The resource descriptors a device's _CRS returns (ACPI 6.4, section 6.4): the ones a platform device on an I2C bus needs - the fixed memory ranges of a controller, the I2C serial
// bus connection of a device (its address and speed, and the controller it sits behind named by a path), the GPIO interrupt connection (a pin of a GPIO controller named by a path,
// its polarity and trigger) and the interrupts. Everything else is skipped by its length. The bytes come from firmware: every length is checked.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

struct AcpiResources {
  static constexpr uint32_t kMaxMemory = 8, kMaxInterrupts = 4, kPathBytes = 64;
  struct Memory { uint32_t base = 0, length = 0; };
  struct I2c {
    bool present = false;
    uint16_t address = 0;
    bool ten_bit = false;
    uint32_t speed_hz = 0;
    char controller[kPathBytes] = {};            // the controller the device is behind ("\_SB.PCI0.I2C0")
  };
  struct GpioInterrupt {
    bool present = false;
    uint16_t pin = 0;
    bool edge = false, active_low = false, active_both = false, wake = false;
    char controller[kPathBytes] = {};            // the GPIO controller
  };
  Memory memory[kMaxMemory];
  uint32_t memory_count = 0;
  I2c i2c;
  GpioInterrupt gpio_interrupt;
  uint32_t interrupts[kMaxInterrupts] = {};
  uint32_t interrupt_count = 0;
};

// False if the buffer is not a well formed resource template (a descriptor that runs past the end, or no end tag with the buffer ending early is accepted: some firmware omits it).
bool parse_acpi_resources(ConstByteView crs, AcpiResources* out);

}  // namespace hardware
