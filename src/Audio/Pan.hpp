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

constexpr int kPanLeft = -100, kPanCenter = 0, kPanRight = 100;

namespace pan_detail {
// sin(x) for 0 <= x <= pi/2 by Taylor series; evaluated at compile time only.
constexpr double sine(double x) {
  double term = x, sum = x;
  for (int n = 1; n < 12; ++n) {
    term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}

struct Table {
  int32_t left[201];   // gain of the left channel for pan -100 .. +100, Q15 (32768 = 1.0), equal power
  int32_t right[201];  // and the right
};

constexpr Table make_table() {
  constexpr double kPi = 3.14159265358979323846;
  Table t{};
  for (int i = 0; i <= 200; ++i) {
    const double angle = kPi / 2 * i / 200;  // 0 = hard left, pi/2 = hard right
    t.right[i] = static_cast<int32_t>(sine(angle) * 32768 + 0.5);
    t.left[i] = static_cast<int32_t>(sine(kPi / 2 - angle) * 32768 + 0.5);
  }
  return t;
}
inline constexpr Table kTable = make_table();
}  // namespace pan_detail

// Channel gains for a position from -100 (all left) to +100 (all right), Q15.
//  - A mono source is placed with the equal-power law (the two gains' squares add
//    up to one, so it is as loud at the sides as in the middle): in the centre
//    each channel gets 0.707.
//  - A stereo source is balanced: the centre leaves both channels untouched, and
//    moving off-centre turns the far channel down along the same curve (the
//    near one stays at full level).
inline void pan_gains(int pan, bool mono_source, int32_t* left, int32_t* right) {
  if (pan < kPanLeft) pan = kPanLeft;
  if (pan > kPanRight) pan = kPanRight;
  int32_t l = pan_detail::kTable.left[pan + 100], r = pan_detail::kTable.right[pan + 100];
  if (!mono_source) {  // scale so the centre is unity, then cap at unity
    l = (l * 46341 + 16384) / 32768;  // times sqrt(2), rounded
    r = (r * 46341 + 16384) / 32768;
    if (l > 32760) l = 32768;  // the centre must leave the audio untouched (rounding would leave it a hair under)
    if (r > 32760) r = 32768;
  }
  *left = l;
  *right = r;
}

}  // namespace audio
