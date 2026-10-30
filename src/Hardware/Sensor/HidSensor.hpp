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

// A sensor of a HID sensor device (HID Usage Tables, page 0x20; the sensor hubs of laptops expose accelerometers, gyroscopes, magnetometers and ambient light sensors this way) on an
// I2C-HID link. discover() looks in the device's descriptor for input reports with the data fields of a kind - acceleration X / Y / Z (0x453-0x455), angular velocity (0x457-0x459),
// magnetic flux (0x485-0x487), illuminance (0x4D1) - and makes one sensor per report id. A raw value is scaled by 10^(unit exponent) of its field, which gives the units of ISensor.hpp
// (g, degrees per second, gauss, lux). Before a sensor reports anything the device is told to: power state "D0 full power" and reporting state "all events", set through the
// feature report that has those properties (the value is the index of the selector among the item's usages). The device reports as it likes (no report interval is set).
#pragma once
#include "Hardware/Bus/I2c/HidInputMap.hpp"
#include "Hardware/Bus/I2c/I2cHidClient.hpp"
#include "Hardware/Mouse/I2c/I2cHidPointer.hpp"
#include "Hardware/Sensor/ISensor.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class HidSensor final : public ISensor, public II2cHidClient {
 public:
  static constexpr uint32_t kMaxPerDevice = 8;
  HidSensor(I2cHidPointer& device, SensorKind kind, uint8_t report_id, const char* name) : device_(device), kind_(kind), report_id_(report_id), name_(name) {}

  // Finds the sensors in the device's report descriptor; for each one found `make(kind, report_id)` is called by the caller's loop: this fills `kinds` / `ids` (up to kMaxPerDevice) and
  // returns how many.
  static uint32_t discover(const HidInputMap& map, SensorKind* kinds, uint8_t* ids);
  // Sets the sensor's fields up and registers it with the device. False if a field is missing.
  bool initialize() override;
  const char* name() const override { return name_; }
  void shutdown() override { device_.remove_client(this); }
  SensorKind kind() const override { return kind_; }
  bool read(SensorReading* out) override;
  void on_report(ConstByteView report) override;

 private:
  bool enable();
  I2cHidPointer& device_;
  SensorKind kind_;
  uint8_t report_id_;
  const char* name_;
  kernel::Mutex mutex_;
  const HidInputField* fields_[3] = {};
  float scale_[3] = {1, 1, 1};
  uint32_t axes_ = 0;
  SensorReading latest_;
  bool have_ = false;
};

}  // namespace hardware
