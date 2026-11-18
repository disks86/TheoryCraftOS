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

// The environment controller of the ITE IT87xx and IT86xx Super I/O chips (Linux it87). Eight-bit registers through the address (base+5) and
// data (base+6) ports; 0x58 reads 0x90 on every part, and bit 7 of 0x00 (reset in progress) reads 0.
//
//   Voltages: 0x20..0x28 and 0x2F, one count = 16 mV (IT8705..IT8726), 12 mV (IT8721, IT8728, IT877x, IT86xx) or 10.9 mV (IT8625, IT8655,
//     IT8665, IT8792, IT87952); the inputs behind an internal divider (3VSB, Vbat, ... on the newer parts) read half.
//   Temperatures: 0x29..0x2B, signed degrees. Bits 7:6 of 0x51 name the one wired to the CPU's PECI on the newer parts.
//   Fans: 16-bit counts (low byte 0x0D, 0x0E, 0x0F, 0x80, 0x82, 0x4C; high byte 0x18, 0x19, 0x1A, 0x81, 0x83, 0x4D), RPM = 1350000 / (2 * count).
//     The IT8705 has 8-bit counts with a divisor in 0x0B; the IT8712 has either, by bits 2:0 of 0x0C.
//   PWM: control 0x15, 0x16, 0x17, 0x7F, 0xA7, 0xAF, bit 7 set for the automatic (SmartGuardian) mode. Older parts keep a manual 7-bit duty in
//     bits 6:0; the newer ones an 8-bit duty in 0x63, 0x6B, 0x73, 0x7B, 0xA3, 0xAB. Bits 2:0 of 0x13 turn the PWM on for the first three outputs.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"
#include "Hardware/HardwareMonitor/SuperIo.hpp"

namespace hardware {

enum class IteFanWidth : uint8_t { Bits8, Selectable, Bits16 };

struct IteLayout {
  uint32_t lsb_microvolts;     // 16000, 12000 or 10900
  uint16_t internal_inputs;    // bit n: input n reads half through an internal divider
  ArrayView<const char* const> voltage_labels;   // one per input
  uint8_t fans;
  IteFanWidth fan_width;
  uint8_t pwms;
  bool newer_pwm;              // 8-bit duty registers, PECI in 0x51
};

const IteLayout* ite_layout(SuperIoChip chip);

class Ite87xx final : public IHardwareMonitor {
 public:
  static constexpr uint8_t kRegConfig = 0x00, kRegFanMainControl = 0x13, kRegFanDivisor = 0x0B, kRegFan16 = 0x0C, kRegTemperature = 0x29,
                           kRegTemperatureType = 0x51, kRegChipId = 0x58;
  static constexpr uint8_t kChipIdValue = 0x90;
  static constexpr uint32_t kTemperatures = 3, kMaxPwm = 6;

  Ite87xx(IPortIo& io, const SuperIoChipFound& found) : ports_(io, found.sensor_base), found_(found) {}

  const char* name() const override { return found_.info ? found_.info->name : "ITE"; }
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
  bool fan_is_16bit(uint32_t channel);
  void enable_pwm_output(uint32_t channel);

  IndexedPorts ports_;
  SuperIoChipFound found_;
  const IteLayout* layout_ = nullptr;
  int32_t peci_channel_ = -1;               // the temperature wired to PECI, -1 for none
  uint8_t firmware_control_[kMaxPwm] = {};  // the control registers at initialize()
};

}  // namespace hardware
