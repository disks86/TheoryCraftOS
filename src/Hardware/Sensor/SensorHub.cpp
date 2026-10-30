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

#include "Hardware/Sensor/SensorHub.hpp"

namespace hardware {

bool SensorHub::add(ISensor* sensor) {
  kernel::ScopedMutex guard(mutex_);
  if (!sensor || count_ == kMaxSensors) return false;
  for (uint32_t i = 0; i < count_; ++i)
    if (sensors_[i] == sensor) return true;
  sensors_[count_++] = sensor;
  return true;
}

bool SensorHub::remove(ISensor* sensor) {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i) {
    if (sensors_[i] != sensor) continue;
    for (uint32_t k = i + 1; k < count_; ++k) sensors_[k - 1] = sensors_[k];
    sensors_[--count_] = nullptr;
    return true;
  }
  return false;
}

uint32_t SensorHub::count() const {
  kernel::ScopedMutex guard(mutex_);
  return count_;
}

ISensor* SensorHub::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < count_ ? sensors_[index] : nullptr;
}

ISensor* SensorHub::find(SensorKind kind, uint32_t n) const {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i)
    if (sensors_[i]->kind() == kind && n-- == 0) return sensors_[i];
  return nullptr;
}

bool SensorHub::read(SensorKind kind, SensorReading* out) {
  ISensor* s = find(kind);
  return s && s->read(out);
}

}  // namespace hardware
