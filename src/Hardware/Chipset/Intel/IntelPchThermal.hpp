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

// The thermal sensor of the Intel PCH (00:12.0 on Haswell's Lynx Point through Comet Lake, Wellsburg and Lewisburg: Linux intel_pch_thermal). Its BAR 0
// holds TEMP (0x00, 9 bits of half degrees from -50 C), TSEL (0x08, bit 0: the firmware turned the sensor on) and CTT (0x10, the catastrophic trip
// point in the same unit). From Ice Lake on the PCH reports its temperature through the PMC and ACPI instead, and has no such function.
//
// Only reading: a sensor the firmware left off stays off (Linux turns it on unless TSEL is locked).
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"
#include "Hardware/Power/Thermal.hpp"

namespace hardware {

// True for the PCI device IDs (vendor 0x8086) of the PCH thermal functions Linux drives.
bool intel_pch_thermal_device(uint16_t device_id);

class IntelPchThermal final : public IThermalSource {
 public:
  static constexpr uint64_t kRegisterBytes = 0x100;
  static constexpr uint32_t kTemp = 0x00, kTsel = 0x08, kCtt = 0x10;
  static constexpr uint16_t kReadingMask = 0x1FF;
  static constexpr uint8_t kTselEnabled = 0x01, kTselLocked = 0x80;

  // The sensor's registers (BAR 0, at least kRegisterBytes). False if the window is unusable.
  bool attach(const MmioRegion& registers);
  bool attached() const { return registers_.valid(); }
  bool enabled() const;
  bool locked() const;

  // Half degrees from -50 C, as millidegrees.
  static int32_t millicelsius_of(uint16_t reading) { return static_cast<int32_t>(reading & kReadingMask) * 500 - 50000; }

  const char* name() const override { return "chipset"; }
  bool read(int32_t* millicelsius) override;
  // The critical point is the CTT register when the firmware set one; no passive point (slowing the processor does little for the PCH).
  ThermalLimits limits() override;

 private:
  MmioRegion registers_;
};

}  // namespace hardware
