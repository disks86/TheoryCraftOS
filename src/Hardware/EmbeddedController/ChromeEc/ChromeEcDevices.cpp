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

#include "Hardware/EmbeddedController/ChromeEc/ChromeEcDevices.hpp"

namespace hardware {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

bool contains_nocase(StringView text, StringView needle) {
  if (needle.size() > text.size()) return false;
  for (uint64_t i = 0; i + needle.size() <= text.size(); ++i) {
    bool same = true;
    for (uint64_t j = 0; j < needle.size() && same; ++j) same = lower(text[i + j]) == lower(needle[j]);
    if (same) return true;
  }
  return false;
}

// What a sensor of this name measures (Framework's boards name them "F75303_CPU", "PECI", "APU", "cpu@4c", "F75303_DDR", "Battery", ...).
SensorRole role_of(StringView name) {
  static const char* const kCpuNames[] = {"cpu", "peci", "apu", "soc"};
  for (const char* cpu : kCpuNames)
    if (contains_nocase(name, cpu)) return SensorRole::Cpu;
  if (contains_nocase(name, "local") || contains_nocase(name, "ambient")) return SensorRole::System;
  return SensorRole::Other;
}

const char* image_name(EcImage image) {
  switch (image) {
    case EcImage::ReadOnly: return "read-only";
    case EcImage::ReadWrite: return "read-write";
    default: return "unknown";
  }
}

}  // namespace

// --- Hardware monitor ------------------------------------------------------------------------------------------------------------------------

bool ChromeEcHardwareMonitor::initialize() {
  sensor_count_ = 0;
  fan_count_ = 0;
  for (uint32_t i = 0; i < ChromeEc::kMaxTemperatureSensors; ++i) {
    const Optional<EcTemperatureReading> t = ec_.temperature(i);
    if (!t || !t->present) continue;
    Sensor& s = ArrayView<Sensor>(sensors_)[sensor_count_++];
    s = Sensor();
    s.index = static_cast<uint8_t>(i);
    if (!ec_.temperature_sensor_name(s.index, &s.label)) {
      (void)s.label.assign("EC sensor ");
      (void)s.label.append_uint(i);
    }
    s.role = role_of(s.label.view());
  }
  for (uint32_t i = 0; i < ChromeEc::kMaxFans; ++i) {
    if (!ec_.fan_rpm(i)) continue;
    Fan& f = ArrayView<Fan>(fans_)[fan_count_++];
    f = Fan();
    f.index = static_cast<uint8_t>(i);
    (void)f.label.assign("Fan ");
    (void)f.label.append_uint(i + 1);
  }
  return sensor_count_ > 0 || fan_count_ > 0;
}

void ChromeEcHardwareMonitor::describe(IPropertySink& sink) const {
  const EcVersionInfo& v = ec_.version();
  sink.add_text("Embedded controller firmware (read-only)", v.read_only.c_str());
  sink.add_text("Embedded controller firmware (read-write)", v.read_write.c_str());
  sink.add_text("Running image", image_name(v.current));
  sink.add_text("Controller vendor", ec_.chip().vendor.c_str());
  sink.add_text("Controller chip", ec_.chip().name.c_str());
  sink.add_text("Host interface", ec_.transport_kind() == EcTransportKind::Mec ? "Microchip EMI, protocol 3" : "LPC, protocol 3");
}

uint32_t ChromeEcHardwareMonitor::channel_count(MonitorChannel kind) const {
  switch (kind) {
    case MonitorChannel::Temperature: return sensor_count_;
    case MonitorChannel::Fan:
    case MonitorChannel::Pwm: return fan_count_;
    case MonitorChannel::Voltage: return 0;
  }
  return 0;
}

const char* ChromeEcHardwareMonitor::label(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return nullptr;
  if (kind == MonitorChannel::Temperature) return ArrayView<const Sensor>(sensors_)[channel].label.c_str();
  return ArrayView<const Fan>(fans_)[channel].label.c_str();
}

SensorRole ChromeEcHardwareMonitor::role(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return SensorRole::Other;
  if (kind == MonitorChannel::Temperature) return ArrayView<const Sensor>(sensors_)[channel].role;
  return fan_count_ == 1 ? SensorRole::Cpu : SensorRole::Other;   // (a laptop's only fan is the one on the processor)
}

Optional<int32_t> ChromeEcHardwareMonitor::temperature_millicelsius(uint32_t channel) {
  if (channel >= sensor_count_) return {};
  const Optional<EcTemperatureReading> t = ec_.temperature(ArrayView<Sensor>(sensors_)[channel].index);
  if (!t || !t->valid) return {};
  return t->millicelsius;
}

Optional<uint32_t> ChromeEcHardwareMonitor::fan_rpm(uint32_t channel) {
  if (channel >= fan_count_) return {};
  return ec_.fan_rpm(ArrayView<Fan>(fans_)[channel].index);
}

Optional<PwmState> ChromeEcHardwareMonitor::pwm(uint32_t channel) {
  if (channel >= fan_count_) return {};
  kernel::ScopedMutex guard(fan_mutex_);
  const Fan& f = ArrayView<Fan>(fans_)[channel];
  PwmState s;
  s.mode = f.mode;
  s.duty_known = f.duty_known;
  s.duty = f.duty_known ? f.duty : 0;
  return s;
}

Optional<uint8_t> ChromeEcHardwareMonitor::fan_target(uint32_t channel) const {
  if (fan_count_ == 1) return {};
  return ArrayView<const Fan>(fans_)[channel].index;
}

bool ChromeEcHardwareMonitor::set_pwm_duty(uint32_t channel, uint8_t duty) {
  if (channel >= fan_count_) return false;
  const uint8_t percent = static_cast<uint8_t>((uint32_t{duty} * 100 + 127) / 255);
  if (!ec_.set_fan_duty(fan_target(channel), percent)) return false;
  kernel::ScopedMutex guard(fan_mutex_);
  Fan& f = ArrayView<Fan>(fans_)[channel];
  f.mode = PwmMode::Manual;
  f.duty_known = true;
  f.duty = duty;
  return true;
}

bool ChromeEcHardwareMonitor::set_pwm_mode(uint32_t channel, PwmMode mode) {
  if (channel >= fan_count_) return false;
  if (mode == PwmMode::Automatic) {
    if (!ec_.set_fan_automatic(fan_target(channel))) return false;
    kernel::ScopedMutex guard(fan_mutex_);
    Fan& f = ArrayView<Fan>(fans_)[channel];
    f.mode = PwmMode::Automatic;
    f.duty_known = false;
    return true;
  }
  uint8_t duty = 0;
  {
    kernel::ScopedMutex guard(fan_mutex_);
    const Fan& f = ArrayView<Fan>(fans_)[channel];
    if (f.mode == PwmMode::Manual) return true;
    if (!f.duty_known) return false;   // nothing to keep: the EC cannot say what duty its curve runs
    duty = f.duty;
  }
  return set_pwm_duty(channel, duty);
}

bool ChromeEcHardwareMonitor::set_fan_target_rpm(uint32_t channel, uint32_t rpm) {
  if (channel >= fan_count_) return false;
  if (!ec_.set_fan_target_rpm(fan_target(channel), rpm)) return false;
  kernel::ScopedMutex guard(fan_mutex_);
  Fan& f = ArrayView<Fan>(fans_)[channel];
  f.mode = PwmMode::Manual;
  f.duty_known = false;
  return true;
}

// --- Keyboard backlight ----------------------------------------------------------------------------------------------------------------------

bool ChromeEcKeyboardBacklight::percent(uint8_t* out) {
  const Optional<EcKeyboardBacklight> k = ec_.keyboard_backlight();
  if (!k) return false;
  *out = k->enabled ? k->percent : 0;
  return true;
}

bool ChromeEcKeyboardBacklight::set_percent(uint8_t percent) { return ec_.set_keyboard_backlight(percent > 100 ? 100 : percent); }

}  // namespace hardware
