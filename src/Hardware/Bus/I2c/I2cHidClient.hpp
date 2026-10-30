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

// What a function of an I2C-HID device other than a touchpad or touchscreen (a pen, a sensor) implements to get the device's input reports. I2cHidPointer reads the device and hands every
// report (its report id byte first, when the device uses ids) to each client; a client picks the reports that are its own.
#pragma once
#include "Utility/ArrayView.hpp"

namespace hardware {

class II2cHidClient {
 public:
  virtual void on_report(ConstByteView report) = 0;

 protected:
  constexpr II2cHidClient() = default;
  ~II2cHidClient() = default;
};

}  // namespace hardware
