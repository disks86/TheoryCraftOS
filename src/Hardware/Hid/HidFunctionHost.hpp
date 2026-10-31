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

// What the drivers of the functions of a HID device (a pen, a sensor) need from the device they are on, whatever carries its reports: the parsed report descriptor, a way to be given
// every input report, a read of the device for those reports, and the feature report that turns a function on. I2cHidPointer (HID over I2C) and IshHidDevice (the Intel sensor hub)
// implement it; HidSensor and I2cHidPen are written against it.
#pragma once
#include "Hardware/Bus/I2c/HidInputMap.hpp"
#include "Hardware/Bus/I2c/I2cHidClient.hpp"

namespace hardware {

class IHidFunctionHost {
 public:
  virtual const HidInputMap& map() const = 0;
  virtual bool add_client(II2cHidClient* client) = 0;
  virtual void remove_client(II2cHidClient* client) = 0;
  // Reads what the device has and hands it to the clients.
  virtual void pump() = 0;
  // Sends a feature report: `id` (0 if the device uses no ids) and the report without its id byte.
  virtual bool set_feature(uint8_t id, ConstByteView report) = 0;

 protected:
  constexpr IHidFunctionHost() = default;
  ~IHidFunctionHost() = default;
};

}  // namespace hardware
