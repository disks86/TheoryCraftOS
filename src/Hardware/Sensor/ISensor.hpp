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

// A hardware sensor: the interface drivers of every bus implement (HID sensors over I2C, the ACPI ambient light sensor, an Intel sensor hub later) and programs use (auto-rotate,
// auto-brightness, a compass, a game) without knowing any of them. SensorHub keeps the list and finds a sensor by what it measures.
#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"

namespace hardware {

enum class SensorKind : uint8_t {
  Accelerometer,     // x, y, z in g (the sensor's own axes)
  Gyroscope,         // x, y, z in degrees per second
  Magnetometer,      // x, y, z in gauss
  AmbientLight,      // x = illuminance in lux
};

struct SensorReading {
  SensorKind kind = SensorKind::AmbientLight;
  float x = 0, y = 0, z = 0;       // see SensorKind; unused axes are 0
  uint32_t sequence = 0;           // counts the samples taken; a new value means a new sample
};

class ISensor : public IHardware {
 public:
  virtual SensorKind kind() const = 0;
  // Fetches whatever the device has to say and gives the latest sample. False until the sensor has delivered one (or when it cannot be read).
  virtual bool read(SensorReading* out) = 0;

 protected:
  constexpr ISensor() = default;
  ~ISensor() = default;
};

inline const char* sensor_kind_name(SensorKind kind) {
  switch (kind) {
    case SensorKind::Accelerometer: return "accelerometer";
    case SensorKind::Gyroscope: return "gyroscope";
    case SensorKind::Magnetometer: return "magnetometer";
    case SensorKind::AmbientLight: return "ambient light sensor";
  }
  return "sensor";
}

}  // namespace hardware
