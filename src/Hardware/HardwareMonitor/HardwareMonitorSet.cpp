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

#include "Hardware/HardwareMonitor/HardwareMonitorSet.hpp"

namespace hardware {

void HardwareMonitorSet::clear() {
  for (Slot& slot : slots_) {
    slot.monitor = nullptr;
    slot.problem = nullptr;
    slot.nuvoton.reset();
    slot.ite.reset();
    slot.fintek.reset();
    slot.found = SuperIoChipFound();
  }
  chip_count_ = 0;
}

void HardwareMonitorSet::start(IPortIo& io, Slot& slot) {
  slot.problem = slot.found.problem;
  if (slot.problem || !slot.found.info) return;
  IHardwareMonitor* monitor = nullptr;
  switch (slot.found.info->vendor) {
    case SuperIoVendor::Nuvoton: monitor = &slot.nuvoton.emplace(io, slot.found); break;
    case SuperIoVendor::Ite: monitor = &slot.ite.emplace(io, slot.found); break;
    case SuperIoVendor::Fintek: monitor = &slot.fintek.emplace(io, slot.found); break;
  }
  if (monitor && monitor->initialize()) {
    slot.monitor = monitor;
    return;
  }
  slot.problem = "the sensor window did not answer as this chip's should";
  slot.nuvoton.reset();
  slot.ite.reset();
  slot.fintek.reset();
}

uint32_t HardwareMonitorSet::probe(IPortIo& io) {
  clear();
  uint32_t working = 0;
  for (uint16_t port : superio::kConfigPorts) {
    if (chip_count_ >= kMaxChips) break;
    Optional<SuperIoChipFound> found = superio_probe(io, port);
    if (!found) continue;
    Slot& slot = ArrayView<Slot>(slots_)[chip_count_++];
    slot.found = *found;
    start(io, slot);
    if (slot.monitor) ++working;
  }
  return working;
}

const SuperIoChipFound* HardwareMonitorSet::found(uint32_t chip) const {
  if (chip >= chip_count_) return nullptr;
  return &ArrayView<const Slot>(slots_)[chip].found;
}

IHardwareMonitor* HardwareMonitorSet::monitor(uint32_t chip) {
  if (chip >= chip_count_) return nullptr;
  return ArrayView<Slot>(slots_)[chip].monitor;
}

const char* HardwareMonitorSet::problem(uint32_t chip) const {
  if (chip >= chip_count_) return nullptr;
  return ArrayView<const Slot>(slots_)[chip].problem;
}

Optional<int32_t> HardwareMonitorSet::cpu_temperature_millicelsius() {
  for (uint32_t chip = 0; chip < chip_count_; ++chip) {
    IHardwareMonitor* m = monitor(chip);
    if (!m) continue;
    for (uint32_t i = 0; i < m->channel_count(MonitorChannel::Temperature); ++i) {
      if (m->role(MonitorChannel::Temperature, i) != SensorRole::Cpu) continue;
      Optional<int32_t> t = m->temperature_millicelsius(i);
      if (t) return t;
    }
  }
  return {};
}

Optional<uint32_t> HardwareMonitorSet::fan_rpm() {
  Optional<uint32_t> fastest;
  for (uint32_t chip = 0; chip < chip_count_; ++chip) {
    IHardwareMonitor* m = monitor(chip);
    if (!m) continue;
    for (uint32_t i = 0; i < m->channel_count(MonitorChannel::Fan); ++i) {
      Optional<uint32_t> rpm = m->fan_rpm(i);
      if (!rpm) continue;
      if (m->role(MonitorChannel::Fan, i) == SensorRole::Cpu) return rpm;
      if (!fastest || *rpm > *fastest) fastest = rpm;
    }
  }
  return fastest;
}

uint32_t HardwareMonitorSet::snapshot(ArrayView<HardwareMonitorReading> out) {
  constexpr MonitorChannel kKinds[] = {MonitorChannel::Temperature, MonitorChannel::Fan, MonitorChannel::Voltage, MonitorChannel::Pwm};
  uint32_t written = 0;
  for (uint32_t chip = 0; chip < chip_count_; ++chip) {
    IHardwareMonitor* m = monitor(chip);
    if (!m) continue;
    for (MonitorChannel kind : kKinds) {
      for (uint32_t i = 0; i < m->channel_count(kind); ++i) {
        if (written >= out.size()) return written;
        HardwareMonitorReading& r = out[written++];
        r = HardwareMonitorReading();
        r.kind = kind;
        r.chip = static_cast<uint8_t>(chip);
        r.channel = static_cast<uint8_t>(i);
        r.label = m->label(kind, i);
        r.role = m->role(kind, i);
        switch (kind) {
          case MonitorChannel::Temperature: {
            Optional<int32_t> v = m->temperature_millicelsius(i);
            r.valid = v.has_value();
            r.value = v.value_or(0);
            break;
          }
          case MonitorChannel::Fan: {
            Optional<uint32_t> v = m->fan_rpm(i);
            r.valid = v.has_value();
            r.value = static_cast<int32_t>(v.value_or(0u));
            break;
          }
          case MonitorChannel::Voltage: {
            Optional<int32_t> v = m->voltage_millivolts(i);
            r.valid = v.has_value();
            r.value = v.value_or(0);
            break;
          }
          case MonitorChannel::Pwm: {
            Optional<PwmState> v = m->pwm(i);
            r.valid = v.has_value() && v->duty_known;
            r.value = v ? v->duty : 0;
            break;
          }
        }
      }
    }
  }
  return written;
}

}  // namespace hardware
