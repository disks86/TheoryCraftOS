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

// Register value conversions the sensor chip drivers share. Every one rejects the values a missing sensor or a stuck bus gives instead of
// dividing by them.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware::sensor_math {

// A signed byte in degrees Celsius. 0x80 (-128) is what the chips read for an open or disabled sensor.
inline Optional<int32_t> celsius_byte(uint8_t raw) {
  if (raw == 0x80) return {};
  return static_cast<int32_t>(static_cast<int8_t>(raw)) * 1000;
}

// A word in half degrees: the high byte signed degrees, bit 7 of the low byte the half.
inline Optional<int32_t> celsius_half_word(uint16_t raw) {
  if ((raw >> 8) == 0x80) return {};
  const int32_t halves = static_cast<int16_t>(raw & 0xFF80) >> 7;
  return halves * 500;
}

// RPM from a tachometer count: `clock` / count, where 0 and every bit set (the counter ran over: the fan stands still) mean 0 RPM.
inline uint32_t rpm_from_count(uint32_t count, uint32_t all_ones, uint32_t clock) {
  if (count == 0 || count >= all_ones) return 0;
  return clock / count;
}

// A checked view of a constant table, so channel lookups go through ArrayView's bounds check.
template <class T, uint64_t N>
constexpr ArrayView<const T> view(const T (&array)[N]) { return ArrayView<const T>(array); }

// (raw * scale + 50) / 100, the rounding Linux's drivers use.
inline int32_t scaled(uint32_t raw, uint32_t scale_times_100) { return static_cast<int32_t>((raw * scale_times_100 + 50) / 100); }

// Guesses what a channel measures from its label (sources named by the chip, the fan headers of the reference boards).
inline bool starts_with(const char* text, const char* prefix) {
  if (!text) return false;
  for (; *prefix; ++prefix, ++text) {
    if (*text != *prefix) return false;
  }
  return true;
}
inline SensorRole role_of_label(const char* label) {
  if (starts_with(label, "CPU") || starts_with(label, "PECI") || starts_with(label, "AMD TSI") || starts_with(label, "Vcore")) return SensorRole::Cpu;
  if (starts_with(label, "SYS")) return SensorRole::System;
  if (starts_with(label, "PCH")) return SensorRole::Chipset;
  return SensorRole::Other;
}

}  // namespace hardware::sensor_math
