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

#include "Hardware/HardwareMonitor/Ite87xx.hpp"
#include "Hardware/HardwareMonitor/SensorMath.hpp"

namespace hardware {

namespace {

using sensor_math::view;

constexpr uint8_t kVoltageRegs[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2F};
constexpr uint8_t kFanLow[] = {0x0D, 0x0E, 0x0F, 0x80, 0x82, 0x4C};
constexpr uint8_t kFanHigh[] = {0x18, 0x19, 0x1A, 0x81, 0x83, 0x4D};
constexpr uint8_t kPwmControl[] = {0x15, 0x16, 0x17, 0x7F, 0xA7, 0xAF};
constexpr uint8_t kPwmDuty[] = {0x63, 0x6B, 0x73, 0x7B, 0xA3, 0xAB};
static_assert(sizeof kPwmControl == Ite87xx::kMaxPwm && sizeof kPwmDuty == Ite87xx::kMaxPwm && sizeof kFanLow == sizeof kFanHigh);

constexpr const char* kOldVoltageLabels[] = {"in0", "in1", "in2", "in3", "in4", "in5", "in6", "in7", "Vbat"};
constexpr const char* kNewVoltageLabels9[] = {"in0", "in1", "in2", "+3.3V", "in4", "in5", "in6", "3VSB", "Vbat"};
constexpr const char* kNewVoltageLabels10[] = {"in0", "in1", "in2", "+3.3V", "in4", "in5", "in6", "3VSB", "Vbat", "AVCC3"};
constexpr const char* kTemperatureLabels[] = {"Temp1", "Temp2", "Temp3"};
constexpr const char* kPeciLabel = "CPU (PECI)";
constexpr const char* kFanLabels[] = {"Fan1", "Fan2", "Fan3", "Fan4", "Fan5", "Fan6"};
constexpr const char* kPwmLabels[] = {"PWM1", "PWM2", "PWM3", "PWM4", "PWM5", "PWM6"};

constexpr uint16_t kInternalNew = (1u << 3) | (1u << 7) | (1u << 8) | (1u << 9);

// By generation (Linux it87's feature flags). Fan and PWM counts follow the datasheets' pin lists; boards may wire fewer.
constexpr IteLayout kIt8705 = {16000, 0, view(kOldVoltageLabels), 3, IteFanWidth::Bits8, 3, false};
constexpr IteLayout kIt8712 = {16000, 0, view(kOldVoltageLabels), 3, IteFanWidth::Selectable, 3, false};
constexpr IteLayout kIt8716 = {16000, 0, view(kOldVoltageLabels), 5, IteFanWidth::Bits16, 3, false};
constexpr IteLayout kIt8721 = {12000, kInternalNew, view(kNewVoltageLabels9), 5, IteFanWidth::Bits16, 3, true};
constexpr IteLayout kIt8771 = {12000, kInternalNew, view(kNewVoltageLabels9), 3, IteFanWidth::Bits16, 3, true};
constexpr IteLayout kIt8792 = {10900, kInternalNew, view(kNewVoltageLabels9), 3, IteFanWidth::Bits16, 3, true};
constexpr IteLayout kIt8620 = {12000, kInternalNew, view(kNewVoltageLabels10), 5, IteFanWidth::Bits16, 5, true};
constexpr IteLayout kIt8628 = {12000, kInternalNew, view(kNewVoltageLabels10), 6, IteFanWidth::Bits16, 6, true};
constexpr IteLayout kIt8655 = {10900, kInternalNew, view(kNewVoltageLabels10), 3, IteFanWidth::Bits16, 3, true};
constexpr IteLayout kIt8665 = {10900, kInternalNew, view(kNewVoltageLabels10), 6, IteFanWidth::Bits16, 6, true};

}  // namespace

const IteLayout* ite_layout(SuperIoChip chip) {
  switch (chip) {
    case SuperIoChip::It8705: return &kIt8705;
    case SuperIoChip::It8712: return &kIt8712;
    case SuperIoChip::It8716:
    case SuperIoChip::It8718:
    case SuperIoChip::It8720:
    case SuperIoChip::It8726: return &kIt8716;
    case SuperIoChip::It8721:
    case SuperIoChip::It8728: return &kIt8721;
    case SuperIoChip::It8771:
    case SuperIoChip::It8772: return &kIt8771;
    case SuperIoChip::It8792:
    case SuperIoChip::It87952: return &kIt8792;
    case SuperIoChip::It8620:
    case SuperIoChip::It8622:
    case SuperIoChip::It8686:
    case SuperIoChip::It8688: return &kIt8620;
    case SuperIoChip::It8628:
    case SuperIoChip::It8689: return &kIt8628;
    case SuperIoChip::It8655: return &kIt8655;
    case SuperIoChip::It8625:
    case SuperIoChip::It8665: return &kIt8665;
    default: return nullptr;
  }
}

bool Ite87xx::initialize() {
  layout_ = nullptr;
  peci_channel_ = -1;
  if (!found_.info || found_.info->vendor != SuperIoVendor::Ite || found_.sensor_base == 0) return false;
  const IteLayout* layout = ite_layout(found_.info->chip);
  if (!layout) return false;
  if (ports_.read(kRegChipId) != kChipIdValue || (ports_.read(kRegConfig) & 0x80)) return false;
  layout_ = layout;
  if (layout->newer_pwm) {
    const uint32_t peci = ports_.read(kRegTemperatureType) >> 6;   // 1..3: that temperature reads the CPU over PECI
    if (peci >= 1 && peci <= kTemperatures) peci_channel_ = static_cast<int32_t>(peci - 1);
  }
  for (uint32_t i = 0; i < layout->pwms && i < kMaxPwm; ++i) firmware_control_[i] = ports_.read(view(kPwmControl)[i]);
  return true;
}

void Ite87xx::describe(IPropertySink& sink) const {
  if (found_.info) sink.add_text("Chip", found_.info->name);
  sink.add_hex("Chip id", found_.raw_id);
  sink.add_number("Revision", found_.revision);
  sink.add_hex("Sensor I/O base", found_.sensor_base);
  sink.add_hex("Configuration port", found_.config_port, 2);
}

uint32_t Ite87xx::channel_count(MonitorChannel kind) const {
  if (!layout_) return 0;
  switch (kind) {
    case MonitorChannel::Temperature: return kTemperatures;
    case MonitorChannel::Fan: return layout_->fans;
    case MonitorChannel::Voltage: return static_cast<uint32_t>(layout_->voltage_labels.size());
    case MonitorChannel::Pwm: return layout_->pwms;
  }
  return 0;
}

const char* Ite87xx::label(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return nullptr;
  switch (kind) {
    case MonitorChannel::Temperature: return static_cast<int32_t>(channel) == peci_channel_ ? kPeciLabel : view(kTemperatureLabels)[channel];
    case MonitorChannel::Fan: return view(kFanLabels)[channel];
    case MonitorChannel::Voltage: return layout_->voltage_labels[channel];
    case MonitorChannel::Pwm: return view(kPwmLabels)[channel];
  }
  return nullptr;
}

SensorRole Ite87xx::role(MonitorChannel kind, uint32_t channel) const {
  if (kind == MonitorChannel::Temperature && static_cast<int32_t>(channel) == peci_channel_) return SensorRole::Cpu;
  return SensorRole::Other;   // (which header a fan or temperature input serves is the board's choice)
}

Optional<int32_t> Ite87xx::temperature_millicelsius(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Temperature)) return {};
  return sensor_math::celsius_byte(ports_.read(static_cast<uint8_t>(kRegTemperature + channel)));
}

bool Ite87xx::fan_is_16bit(uint32_t channel) {
  switch (layout_->fan_width) {
    case IteFanWidth::Bits8: return false;
    case IteFanWidth::Bits16: return true;
    case IteFanWidth::Selectable: return channel >= 3 || (ports_.read(kRegFan16) & (1u << channel)) != 0;
  }
  return true;
}

Optional<uint32_t> Ite87xx::fan_rpm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Fan)) return {};
  if (fan_is_16bit(channel)) {
    const uint32_t count = ports_.read(view(kFanLow)[channel]) | (static_cast<uint32_t>(ports_.read(view(kFanHigh)[channel])) << 8);
    if (count == 0) return {};   // (it87: no reading)
    return sensor_math::rpm_from_count(count, 0xFFFF, 1350000 / 2);
  }
  // Eight bits with a divisor: fan 1 bits 2:0 and fan 2 bits 5:3 of 0x0B are log2 of it, fan 3 bit 6 picks 8 over 2.
  const uint32_t count = ports_.read(view(kFanLow)[channel]);
  if (count == 0) return {};
  const uint8_t divisors = ports_.read(kRegFanDivisor);
  const uint32_t shift = channel == 0 ? (divisors & 7u) : channel == 1 ? ((divisors >> 3) & 7u) : ((divisors & 0x40) ? 3u : 1u);
  return sensor_math::rpm_from_count(count << shift, 0xFFu << shift, 1350000);
}

Optional<int32_t> Ite87xx::voltage_millivolts(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Voltage)) return {};
  uint32_t lsb = layout_->lsb_microvolts;
  if (layout_->internal_inputs & (1u << channel)) lsb *= 2;
  return static_cast<int32_t>((ports_.read(view(kVoltageRegs)[channel]) * lsb + 500) / 1000);
}

Optional<PwmState> Ite87xx::pwm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return {};
  const uint8_t control = ports_.read(view(kPwmControl)[channel]);
  PwmState state;
  state.mode = (control & 0x80) ? PwmMode::Automatic : PwmMode::Manual;
  if (layout_->newer_pwm) {
    state.duty = ports_.read(view(kPwmDuty)[channel]);
  } else if (state.mode == PwmMode::Manual) {
    state.duty = static_cast<uint8_t>((control & 0x7F) << 1);
  } else {
    state.duty_known = false;
  }
  return state;
}

void Ite87xx::enable_pwm_output(uint32_t channel) {
  if (channel >= 3) return;
  const uint8_t main = ports_.read(kRegFanMainControl);
  if (!(main & (1u << channel))) ports_.write(kRegFanMainControl, static_cast<uint8_t>(main | (1u << channel)));
}

bool Ite87xx::set_pwm_duty(uint32_t channel, uint8_t duty) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  const uint8_t control_reg = view(kPwmControl)[channel];
  uint8_t control;
  if (layout_->newer_pwm) {
    ports_.write(view(kPwmDuty)[channel], duty);
    control = static_cast<uint8_t>(ports_.read(control_reg) & 0x7F);   // (keeps the temperature input the curve would use)
  } else {
    control = static_cast<uint8_t>(duty >> 1);
  }
  ports_.write(control_reg, control);
  enable_pwm_output(channel);
  if (ports_.read(control_reg) != control) return false;
  return !layout_->newer_pwm || ports_.read(view(kPwmDuty)[channel]) == duty;
}

bool Ite87xx::set_pwm_mode(uint32_t channel, PwmMode mode) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  const uint8_t control_reg = view(kPwmControl)[channel];
  const uint8_t control = ports_.read(control_reg);
  uint8_t wanted;
  if (mode == PwmMode::Automatic) {
    wanted = view(firmware_control_)[channel];
    if (!(wanted & 0x80)) return false;   // the firmware ran this output by hand
  } else {
    if (!(control & 0x80)) return true;
    // Manual from automatic: the newer parts keep their duty register; the older ones start at full speed (their curve's duty is not visible).
    wanted = layout_->newer_pwm ? static_cast<uint8_t>(control & 0x7F) : 0x7F;
  }
  if (control != wanted) ports_.write(control_reg, wanted);
  if (mode == PwmMode::Manual) enable_pwm_output(channel);
  return ports_.read(control_reg) == wanted;
}

}  // namespace hardware
