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

#include "Hardware/HardwareMonitor/SystemHardwareMonitor.hpp"

namespace hardware {

namespace {
SystemHardwareMonitor* g_monitor = nullptr;
}  // namespace

uint32_t SystemHardwareMonitor::probe() {
  X86PortIo* io = &io_;
  return with([&](HardwareMonitorSet& set) { return set.probe(*io); });
}

Optional<int32_t> SystemHardwareMonitor::cpu_temperature_millicelsius() {
  const Optional<int32_t> board = with([](HardwareMonitorSet& set) { return set.cpu_temperature_millicelsius(); });
  if (board) return board;
  IHardwareMonitor* ec = embedded_controller();
  if (!ec) return {};
  for (uint32_t i = 0; i < ec->channel_count(MonitorChannel::Temperature); ++i) {
    if (ec->role(MonitorChannel::Temperature, i) != SensorRole::Cpu) continue;
    const Optional<int32_t> t = ec->temperature_millicelsius(i);
    if (t) return t;
  }
  return {};
}

Optional<uint32_t> SystemHardwareMonitor::fan_rpm() {
  const Optional<uint32_t> board = with([](HardwareMonitorSet& set) { return set.fan_rpm(); });
  if (board) return board;
  IHardwareMonitor* ec = embedded_controller();
  if (!ec) return {};
  Optional<uint32_t> fastest;
  for (uint32_t i = 0; i < ec->channel_count(MonitorChannel::Fan); ++i) {
    const Optional<uint32_t> rpm = ec->fan_rpm(i);
    if (rpm && (!fastest || *rpm > *fastest)) fastest = rpm;
  }
  return fastest;
}

SystemHardwareMonitor* system_hardware_monitor() { return __atomic_load_n(&g_monitor, __ATOMIC_ACQUIRE); }

void set_system_hardware_monitor(SystemHardwareMonitor* monitor) { __atomic_store_n(&g_monitor, monitor, __ATOMIC_RELEASE); }

}  // namespace hardware
