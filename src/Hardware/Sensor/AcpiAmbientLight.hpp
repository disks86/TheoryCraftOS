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

// The ambient light sensor the firmware describes (ACPI 6.4 section 9.2, device ACPI0008): _ALI returns the illuminance in lux, _ALR the response curve (not used here). This is how
// most laptops without a sensor hub report brightness of the room. The method runs in the AML interpreter, so the firmware's way of reading the sensor (the embedded controller, a
// register) is its own business.
#pragma once
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Sensor/ISensor.hpp"

namespace hardware {

class AcpiAmbientLight final : public ISensor {
 public:
  AcpiAmbientLight(acpi::AmlInterpreter& vm, acpi::Node* device) : vm_(vm), device_(device) {}
  // The ACPI0008 devices of the namespace that have an _ALI (at most `capacity`).
  static uint32_t find_devices(acpi::AmlInterpreter& vm, acpi::Node** out, uint32_t capacity);

  const char* name() const override { return "ACPI ambient light sensor"; }
  bool initialize() override;
  void shutdown() override {}
  SensorKind kind() const override { return SensorKind::AmbientLight; }
  bool read(SensorReading* out) override;

 private:
  acpi::AmlInterpreter& vm_;
  acpi::Node* device_;
  uint32_t sequence_ = 0;
};

}  // namespace hardware
