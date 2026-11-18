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

// The machine's board sensors for the rest of the kernel: a HardwareMonitorSet on the real I/O ports behind one lock, so the Super I/O's
// index/data sequences (bank select, address, data) of two cores never interleave. SystemChipset owns one and probes it at boot; the system
// monitor (Computer Manager) reads it through system_hardware_monitor().
//
// The firmware's ACPI code and SMM may use the same chip; nothing here can lock them out (Linux has the same exposure). Fan outputs are only
// written when a caller asks.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/HardwareMonitorSet.hpp"
#include "Synchronization/Synchronized.hpp"

namespace hardware {

class SystemHardwareMonitor {
 public:
  explicit SystemHardwareMonitor(const ICentralProcessingUnit& cpu) : set_(cpu) {}
  SystemHardwareMonitor(const SystemHardwareMonitor&) = delete;
  SystemHardwareMonitor& operator=(const SystemHardwareMonitor&) = delete;

  // Looks for the chips; returns how many have a working driver.
  uint32_t probe();

  // Runs `operation(HardwareMonitorSet&)` with the lock held and returns what it returns. Copy readings out; do not keep the drivers' pointers.
  template <class Operation>
  decltype(auto) with(Operation&& operation) {
    return set_.with([&](HardwareMonitorSet& set) -> decltype(auto) { return operation(set); });
  }

  // The Super I/O's reading, else the embedded controller's.
  [[nodiscard]] Optional<int32_t> cpu_temperature_millicelsius();
  [[nodiscard]] Optional<uint32_t> fan_rpm();

  // A laptop's sensors and fans behind its embedded controller (ChromeEcHardwareMonitor on a Chrome EC): a second source beside the Super I/O
  // set, outside its lock, because the controller's commands may wait for it (the monitor serializes itself). nullptr removes it; the monitor
  // must outlive its registration.
  void set_embedded_controller(IHardwareMonitor* monitor) { __atomic_store_n(&embedded_controller_, monitor, __ATOMIC_RELEASE); }
  IHardwareMonitor* embedded_controller() const { return __atomic_load_n(&embedded_controller_, __ATOMIC_ACQUIRE); }

 private:
  X86PortIo io_;
  Synchronized<HardwareMonitorSet> set_;
  IHardwareMonitor* embedded_controller_ = nullptr;
};

// The monitor SystemChipset probed, or nullptr (before that, or after it is gone). Set once at boot; readers may run on any core.
SystemHardwareMonitor* system_hardware_monitor();
void set_system_hardware_monitor(SystemHardwareMonitor* monitor);

}  // namespace hardware
