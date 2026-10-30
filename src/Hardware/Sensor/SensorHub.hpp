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

// The sensors of the machine, wherever they come from. Drivers add theirs; programs ask for one by what it measures. Not owning: the driver keeps its sensor alive until it removed it.
#pragma once
#include "Hardware/Sensor/ISensor.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class SensorHub {
 public:
  static constexpr uint32_t kMaxSensors = 16;
  bool add(ISensor* sensor);
  bool remove(ISensor* sensor);
  uint32_t count() const;
  ISensor* at(uint32_t index) const;
  // The n-th sensor (from 0) of that kind, or nullptr.
  ISensor* find(SensorKind kind, uint32_t n = 0) const;
  // Reads the first sensor of the kind. False if there is none or it has no sample.
  bool read(SensorKind kind, SensorReading* out);

 private:
  mutable kernel::Mutex mutex_;
  ISensor* sensors_[kMaxSensors] = {};
  uint32_t count_ = 0;
};

}  // namespace hardware
