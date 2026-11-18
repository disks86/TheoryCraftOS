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

// The Chrome EC as the system's class interfaces see it:
//   ChromeEcHardwareMonitor  IHardwareMonitor: the memory map's temperatures and fan speeds, one PWM output per fan (EC_CMD_PWM_SET_FAN_DUTY,
//                            back to the EC's curve with EC_CMD_THERMAL_AUTO_FAN_CTRL). The EC does not report the duty its curve chose, so
//                            pwm() has duty_known false until the OS sets one.
//   ChromeEcKeyboardBacklight IBacklight over EC_CMD_PWM_{GET,SET}_KEYBOARD_BACKLIGHT (percent).
// Both are thread-safe (ChromeEc serializes the commands; the monitor's own fan-mode table has a mutex) and may sleep: SystemHardwareMonitor
// calls the monitor outside its spin lock.
#pragma once
#include <stdint.h>
#include "Hardware/Backlight/IBacklight.hpp"
#include "Hardware/EmbeddedController/ChromeEc/ChromeEc.hpp"
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"

namespace hardware {

class ChromeEcHardwareMonitor final : public IHardwareMonitor {
 public:
  explicit ChromeEcHardwareMonitor(ChromeEc& ec) : ec_(ec) {}

  const char* name() const override { return "Chrome EC sensors"; }
  // Finds the sensors and fans the memory map shows and names the sensors (EC_CMD_TEMP_SENSOR_GET_INFO). False with neither.
  bool initialize() override;
  void shutdown() override {}
  void describe(IPropertySink& sink) const override;

  uint32_t channel_count(MonitorChannel kind) const override;
  const char* label(MonitorChannel kind, uint32_t channel) const override;
  SensorRole role(MonitorChannel kind, uint32_t channel) const override;
  [[nodiscard]] Optional<int32_t> temperature_millicelsius(uint32_t channel) override;
  [[nodiscard]] Optional<uint32_t> fan_rpm(uint32_t channel) override;
  [[nodiscard]] Optional<int32_t> voltage_millivolts(uint32_t channel) override { (void)channel; return {}; }
  [[nodiscard]] Optional<PwmState> pwm(uint32_t channel) override;
  [[nodiscard]] bool set_pwm_duty(uint32_t channel, uint8_t duty) override;
  // Automatic: the EC's thermal curve. Manual: only after a duty was set (the EC cannot say what its curve is running now).
  [[nodiscard]] bool set_pwm_mode(uint32_t channel, PwmMode mode) override;

  // Asks the EC to hold a fan at `rpm` (EC_CMD_PWM_SET_FAN_TARGET_RPM; the fan is then in manual mode, duty unknown).
  [[nodiscard]] bool set_fan_target_rpm(uint32_t channel, uint32_t rpm);

 private:
  struct Sensor {
    uint8_t index = 0;
    SensorRole role = SensorRole::Other;
    FixedString<33> label;
  };
  struct Fan {
    uint8_t index = 0;
    FixedString<16> label;
    PwmMode mode = PwmMode::Automatic;
    bool duty_known = false;
    uint8_t duty = 0;
  };
  // A fan index for the command: empty (every fan, version 0) when the machine has one fan.
  Optional<uint8_t> fan_target(uint32_t channel) const;

  ChromeEc& ec_;
  Sensor sensors_[ChromeEc::kMaxTemperatureSensors];
  uint32_t sensor_count_ = 0;
  Fan fans_[ChromeEc::kMaxFans];
  uint32_t fan_count_ = 0;
  kernel::Mutex fan_mutex_;   // the mode/duty of fans_ (the rest is fixed after initialize())
};

class ChromeEcKeyboardBacklight final : public IBacklight {
 public:
  explicit ChromeEcKeyboardBacklight(ChromeEc& ec) : ec_(ec) {}
  const char* name() const override { return "Keyboard backlight (Chrome EC)"; }
  // False when the EC has no keyboard backlight (the command fails).
  bool initialize() override { return ec_.keyboard_backlight().has_value(); }
  void shutdown() override {}
  uint32_t steps() const override { return 101; }
  bool percent(uint8_t* out) override;
  bool set_percent(uint8_t percent) override;

 private:
  ChromeEc& ec_;
};

}  // namespace hardware
