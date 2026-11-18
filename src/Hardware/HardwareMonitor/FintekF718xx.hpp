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

// The hardware monitor of the Fintek F71862, F71868, F71869(A), F71882 and F71889 (Linux f71882fg). Eight-bit registers through the address
// (base+5) and data (base+6) ports; 0x01 bit 0 says the chip measures, bit 1 that it counts fan pulses.
//
//   Voltages: 0x20 + n, 8 mV per count; VCC3V (in0), VSB3V (in7) and VBAT (in8) read half through internal dividers.
//   Temperatures: 0x72, 0x74, 0x76, degrees.
//   Fans: 0xA0 + 16n (high byte) and the next register, a 12-bit count, RPM = 1500000 / count; a full count means the fan stands still.
//   PWM: duty 0xA3 + 16n; the mode in bits 2n+1:2n of 0x96 (0, 1 automatic by speed or by duty; 2, 3 manual by speed or by duty).
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"
#include "Hardware/HardwareMonitor/SuperIo.hpp"

namespace hardware {

struct FintekLayout {
  uint8_t fans;   // fans and PWM outputs
};

const FintekLayout* fintek_layout(SuperIoChip chip);

class FintekF718xx final : public IHardwareMonitor {
 public:
  static constexpr uint8_t kRegStart = 0x01, kRegVoltage = 0x20, kRegTemperature = 0x72, kRegFan = 0xA0, kRegPwmDuty = 0xA3, kRegPwmMode = 0x96;
  static constexpr uint32_t kVoltages = 9, kTemperatures = 3, kMaxFans = 4;

  FintekF718xx(IPortIo& io, const SuperIoChipFound& found) : ports_(io, found.sensor_base), found_(found) {}

  const char* name() const override { return found_.info ? found_.info->name : "Fintek"; }
  bool initialize() override;
  void shutdown() override {}
  void describe(IPropertySink& sink) const override;

  uint32_t channel_count(MonitorChannel kind) const override;
  const char* label(MonitorChannel kind, uint32_t channel) const override;
  SensorRole role(MonitorChannel kind, uint32_t channel) const override;
  [[nodiscard]] Optional<int32_t> temperature_millicelsius(uint32_t channel) override;
  [[nodiscard]] Optional<uint32_t> fan_rpm(uint32_t channel) override;
  [[nodiscard]] Optional<int32_t> voltage_millivolts(uint32_t channel) override;
  [[nodiscard]] Optional<PwmState> pwm(uint32_t channel) override;
  [[nodiscard]] bool set_pwm_duty(uint32_t channel, uint8_t duty) override;
  [[nodiscard]] bool set_pwm_mode(uint32_t channel, PwmMode mode) override;

 private:
  uint8_t mode_bits(uint32_t channel) { return static_cast<uint8_t>((ports_.read(kRegPwmMode) >> (2 * channel)) & 3); }
  bool write_mode_bits(uint32_t channel, uint8_t bits);

  IndexedPorts ports_;
  SuperIoChipFound found_;
  const FintekLayout* layout_ = nullptr;
  bool measuring_ = false, counting_ = false;
  uint8_t firmware_mode_ = 0;   // 0x96 at initialize()
};

}  // namespace hardware
