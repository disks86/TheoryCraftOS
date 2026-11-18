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

// The board's sensor chips: probes both Super I/O configuration ports (a board may carry two chips, e.g. an IT8686 at 0x2E and an IT8792 at
// 0x4E) and owns a driver for each chip it supports. The drivers live inside the set (no allocation: it is built before the kernel heap is
// handed around and lasts as long as the machine runs).
//
// Not thread-safe: every call goes through the ports, and an index write by one caller followed by another's data read would read the wrong
// register. SystemHardwareMonitor wraps the set in a Synchronized; the host tests use a mutex.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/FintekF718xx.hpp"
#include "Hardware/HardwareMonitor/Ite87xx.hpp"
#include "Hardware/HardwareMonitor/NuvotonNct67xx.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// One channel's reading, copied out by HardwareMonitorSet::snapshot().
struct HardwareMonitorReading {
  MonitorChannel kind = MonitorChannel::Temperature;
  uint8_t chip = 0;
  uint8_t channel = 0;
  const char* label = nullptr;   // static text of the driver
  SensorRole role = SensorRole::Other;
  bool valid = false;
  int32_t value = 0;   // millidegrees Celsius, RPM, millivolts or the PWM duty (0..255), by kind
};

class HardwareMonitorSet {
 public:
  static constexpr uint32_t kMaxChips = 2;

  HardwareMonitorSet() = default;
  HardwareMonitorSet(const HardwareMonitorSet&) = delete;
  HardwareMonitorSet& operator=(const HardwareMonitorSet&) = delete;

  // Forgets what an earlier probe found, then looks at 0x2E and 0x4E. `io` must outlive the set. Returns how many chips have a working driver.
  uint32_t probe(IPortIo& io);
  void clear();

  // Every chip that answered, supported or not.
  uint32_t chip_count() const { return chip_count_; }
  const SuperIoChipFound* found(uint32_t chip) const;
  // The chip's driver; nullptr when the chip is not supported or its driver did not start (why: problem()).
  IHardwareMonitor* monitor(uint32_t chip);
  const char* problem(uint32_t chip) const;

  // The first CPU temperature that reads (PECI, CPUTIN, ...).
  [[nodiscard]] Optional<int32_t> cpu_temperature_millicelsius();
  // The CPU fan's speed when a chip labels one, otherwise the fastest fan that turns (most boards wire the CPU fan to a chip's first input but
  // nothing tells which).
  [[nodiscard]] Optional<uint32_t> fan_rpm();

  // Reads every channel of every driver into `out`; returns how many were written (stops when `out` is full).
  uint32_t snapshot(ArrayView<HardwareMonitorReading> out);

 private:
  struct Slot {
    SuperIoChipFound found;
    Optional<NuvotonNct67xx> nuvoton;
    Optional<Ite87xx> ite;
    Optional<FintekF718xx> fintek;
    IHardwareMonitor* monitor = nullptr;   // into one of the three above
    const char* problem = nullptr;
  };
  void start(IPortIo& io, Slot& slot);

  Slot slots_[kMaxChips];
  uint32_t chip_count_ = 0;
};

}  // namespace hardware
