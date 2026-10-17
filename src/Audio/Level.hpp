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

#pragma once
#include <stdint.h>

namespace audio {

// Maps a sample peak (0 .. 32768, 32768 = full scale) to a 0 .. 100 meter position on a decibel
// scale: 0 is -60 dB or quieter, 100 is full scale, and each 6 dB is the same distance (an
// integer approximation of 20*log10, within about half a decibel).
constexpr int32_t peak_to_percent(uint32_t peak) {
  if (peak == 0) return 0;
  if (peak > 32768) peak = 32768;
  const int32_t whole = 31 - __builtin_clz(peak);                             // floor(log2(peak))
  const int32_t fraction = static_cast<int32_t>(((peak - (1u << whole)) << 8) >> whole);  // the mantissa, 0..255
  const int32_t log2_q8 = whole * 256 + fraction;                              // log2(peak) in 1/256ths, linear mantissa
  const int32_t below_full_q8 = 15 * 256 - log2_q8;                           // how many octaves under 32768
  const int32_t db_tenths = below_full_q8 * 602 / 2560;                       // 6.02 dB per octave, in tenths of a dB
  const int32_t percent = 100 - db_tenths * 100 / 600;
  return percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

}  // namespace audio
