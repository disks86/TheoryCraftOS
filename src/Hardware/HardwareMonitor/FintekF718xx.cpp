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

#include "Hardware/HardwareMonitor/FintekF718xx.hpp"
#include "Hardware/HardwareMonitor/SensorMath.hpp"

namespace hardware {

namespace {

using sensor_math::view;

constexpr const char* kVoltageLabels[] = {"VCC3V", "Vcore", "in2", "in3", "in4", "in5", "in6", "VSB3V", "VBAT"};
constexpr uint16_t kInternalInputs = (1u << 0) | (1u << 7) | (1u << 8);
constexpr const char* kTemperatureLabels[] = {"Temp1", "Temp2", "Temp3"};
constexpr const char* kFanLabels[] = {"Fan1", "Fan2", "Fan3", "Fan4"};
constexpr const char* kPwmLabels[] = {"PWM1", "PWM2", "PWM3", "PWM4"};
static_assert(sizeof kVoltageLabels / sizeof kVoltageLabels[0] == FintekF718xx::kVoltages);

constexpr FintekLayout kThreeFans = {3};
constexpr FintekLayout kFourFans = {4};

constexpr uint8_t kModeManualDuty = 3;

}  // namespace

const FintekLayout* fintek_layout(SuperIoChip chip) {
  switch (chip) {
    case SuperIoChip::F71882: return &kFourFans;
    case SuperIoChip::F71862:
    case SuperIoChip::F71868:
    case SuperIoChip::F71869:
    case SuperIoChip::F71869A:
    case SuperIoChip::F71889:
    case SuperIoChip::F71889E:
    case SuperIoChip::F71889A: return &kThreeFans;
    default: return nullptr;
  }
}

bool FintekF718xx::initialize() {
  layout_ = nullptr;
  if (!found_.info || found_.info->vendor != SuperIoVendor::Fintek || found_.sensor_base == 0) return false;
  const FintekLayout* layout = fintek_layout(found_.info->chip);
  if (!layout) return false;
  const uint8_t start = ports_.read(kRegStart);
  if (start == 0xFF || !(start & 0x03)) return false;   // (a floating bus, or the firmware left monitoring off: f71882fg gives up too)
  measuring_ = (start & 0x01) != 0;
  counting_ = (start & 0x02) != 0;
  firmware_mode_ = ports_.read(kRegPwmMode);
  layout_ = layout;
  return true;
}

void FintekF718xx::describe(IPropertySink& sink) const {
  if (found_.info) sink.add_text("Chip", found_.info->name);
  sink.add_hex("Chip id", found_.raw_id);
  sink.add_hex("Sensor I/O base", found_.sensor_base);
  sink.add_hex("Configuration port", found_.config_port, 2);
}

uint32_t FintekF718xx::channel_count(MonitorChannel kind) const {
  if (!layout_) return 0;
  switch (kind) {
    case MonitorChannel::Temperature: return measuring_ ? kTemperatures : 0;
    case MonitorChannel::Voltage: return measuring_ ? kVoltages : 0;
    case MonitorChannel::Fan: return counting_ ? layout_->fans : 0;
    case MonitorChannel::Pwm: return layout_->fans;
  }
  return 0;
}

const char* FintekF718xx::label(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return nullptr;
  switch (kind) {
    case MonitorChannel::Temperature: return view(kTemperatureLabels)[channel];
    case MonitorChannel::Voltage: return view(kVoltageLabels)[channel];
    case MonitorChannel::Fan: return view(kFanLabels)[channel];
    case MonitorChannel::Pwm: return view(kPwmLabels)[channel];
  }
  return nullptr;
}

SensorRole FintekF718xx::role(MonitorChannel kind, uint32_t channel) const {
  if (kind == MonitorChannel::Voltage && channel == 1 && channel < channel_count(kind)) return SensorRole::Cpu;   // Vcore
  return SensorRole::Other;
}

Optional<int32_t> FintekF718xx::temperature_millicelsius(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Temperature)) return {};
  const uint8_t raw = ports_.read(static_cast<uint8_t>(kRegTemperature + 2 * channel));
  if (raw >= 0x80) return {};   // (unsigned degrees; above 127 is a disconnected diode or a stuck bus)
  return static_cast<int32_t>(raw) * 1000;
}

Optional<uint32_t> FintekF718xx::fan_rpm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Fan)) return {};
  const uint8_t reg = static_cast<uint8_t>(kRegFan + 16 * channel);
  const uint32_t count = static_cast<uint32_t>(ports_.read(reg)) << 8 | ports_.read(static_cast<uint8_t>(reg + 1));
  return sensor_math::rpm_from_count(count, 0x0FFF, 1500000);
}

Optional<int32_t> FintekF718xx::voltage_millivolts(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Voltage)) return {};
  const uint32_t scale = (kInternalInputs & (1u << channel)) ? 1600 : 800;
  return sensor_math::scaled(ports_.read(static_cast<uint8_t>(kRegVoltage + channel)), scale);
}

Optional<PwmState> FintekF718xx::pwm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return {};
  PwmState state;
  const uint8_t bits = mode_bits(channel);
  state.mode = bits >= 2 ? PwmMode::Manual : PwmMode::Automatic;
  state.duty = ports_.read(static_cast<uint8_t>(kRegPwmDuty + 16 * channel));
  state.duty_known = (bits & 1) != 0;   // (in the speed modes the register holds a speed, not a duty)
  return state;
}

bool FintekF718xx::write_mode_bits(uint32_t channel, uint8_t bits) {
  const uint8_t shift = static_cast<uint8_t>(2 * channel);
  const uint8_t mode = ports_.read(kRegPwmMode);
  const uint8_t wanted = static_cast<uint8_t>((mode & ~(3u << shift)) | (static_cast<uint32_t>(bits) << shift));
  if (wanted != mode) ports_.write(kRegPwmMode, wanted);
  return ports_.read(kRegPwmMode) == wanted;
}

bool FintekF718xx::set_pwm_duty(uint32_t channel, uint8_t duty) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  if (!write_mode_bits(channel, kModeManualDuty)) return false;
  const uint8_t reg = static_cast<uint8_t>(kRegPwmDuty + 16 * channel);
  ports_.write(reg, duty);
  return ports_.read(reg) == duty;
}

bool FintekF718xx::set_pwm_mode(uint32_t channel, PwmMode mode) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  if (mode == PwmMode::Automatic) {
    const uint8_t bits = static_cast<uint8_t>((firmware_mode_ >> (2 * channel)) & 3);
    if (bits >= 2) return false;   // the firmware ran this output by hand
    return write_mode_bits(channel, bits);
  }
  if (mode_bits(channel) == kModeManualDuty) return true;
  return write_mode_bits(channel, kModeManualDuty);
}

}  // namespace hardware
