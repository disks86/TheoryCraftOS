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

SystemHardwareMonitor* system_hardware_monitor() { return __atomic_load_n(&g_monitor, __ATOMIC_ACQUIRE); }

void set_system_hardware_monitor(SystemHardwareMonitor* monitor) { __atomic_store_n(&g_monitor, monitor, __ATOMIC_RELEASE); }

}  // namespace hardware
