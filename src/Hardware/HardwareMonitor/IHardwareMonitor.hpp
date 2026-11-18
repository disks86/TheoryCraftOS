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

// A motherboard hardware monitor: the temperature, fan, voltage and fan-control (PWM) channels of a Super I/O chip (Nuvoton, ITE, Fintek) or a
// similar sensor chip. Channels are numbered from 0 per kind; each has a short label ("CPUTIN", "CPUFAN", "Vcore"). A reading that the chip
// cannot give, or a register value that makes no sense (a stuck bus, a count that would divide by zero), is an empty Optional, never a made-up
// number. Not thread-safe by itself: SystemHardwareMonitor serializes the callers.
#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

enum class MonitorChannel : uint8_t { Temperature, Fan, Voltage, Pwm };

// What a channel measures, so callers can pick "the CPU temperature" without knowing a chip's labels.
enum class SensorRole : uint8_t { Other, Cpu, System, Chipset };

enum class PwmMode : uint8_t {
  Manual,      // the duty is what the OS last wrote
  Automatic,   // the chip's own fan curve (Smart Fan, SmartGuardian, ...) sets the duty
};

struct PwmState {
  uint8_t duty = 0;   // 0..255 (0 off, 255 full speed)
  PwmMode mode = PwmMode::Automatic;
  bool duty_known = true;   // false when the chip does not show the duty its curve chose (older ITE parts)
};

class IHardwareMonitor : public IHardware {
 public:
  virtual uint32_t channel_count(MonitorChannel kind) const = 0;
  // A short label of the channel, nullptr when `channel` is out of range.
  virtual const char* label(MonitorChannel kind, uint32_t channel) const = 0;
  virtual SensorRole role(MonitorChannel kind, uint32_t channel) const { (void)kind; (void)channel; return SensorRole::Other; }

  // Thousandths of a degree Celsius.
  [[nodiscard]] virtual Optional<int32_t> temperature_millicelsius(uint32_t channel) = 0;
  // Revolutions per minute; 0 when the fan stands still (or none is connected).
  [[nodiscard]] virtual Optional<uint32_t> fan_rpm(uint32_t channel) = 0;
  // Millivolts at the pin, scaled by the chip's internal dividers (not by the board's, which only the board's vendor knows).
  [[nodiscard]] virtual Optional<int32_t> voltage_millivolts(uint32_t channel) = 0;
  [[nodiscard]] virtual Optional<PwmState> pwm(uint32_t channel) = 0;

  // Puts the output in manual mode at `duty`. False when the channel does not exist or the chip did not take the value.
  [[nodiscard]] virtual bool set_pwm_duty(uint32_t channel, uint8_t duty) = 0;
  // Automatic hands the fan back to the chip's curve as the firmware configured it; false when the firmware had it in manual mode (there is no
  // curve to return to) or the write did not stick. Manual keeps the present duty.
  [[nodiscard]] virtual bool set_pwm_mode(uint32_t channel, PwmMode mode) = 0;

 protected:
  constexpr IHardwareMonitor() = default;
  ~IHardwareMonitor() = default;
};

}  // namespace hardware
