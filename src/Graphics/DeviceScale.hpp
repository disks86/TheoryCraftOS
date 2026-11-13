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

// HiDPI: the desktop's device scale as a percentage (100 = one device pixel per unit, 150 = three per two, up to 400), and the one mapping between
// units and device pixels that the scaling renderer, the monitor compositor, the window systems and DPI virtualization all use, so that they agree
// to the pixel. Units to device rounds up, device to units down: device_to_units(units_to_device(v)) == v for every v, so a coordinate that went
// one way and back is the one it was (an unaware program reads back what it set), and edges map in order (rectangles that touch still touch).
#pragma once
#include <stdint.h>

namespace graphics {

inline constexpr uint32_t kMinScalePercent = 100, kMaxScalePercent = 400;

constexpr uint32_t clamp_scale_percent(uint32_t p) { return p < kMinScalePercent ? kMinScalePercent : p > kMaxScalePercent ? kMaxScalePercent : p; }

namespace detail {
constexpr int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }      // (b > 0)
constexpr int32_t saturate(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<int32_t>(v); }
}  // namespace detail

// ceil(v * percent / 100)
constexpr int32_t units_to_device(int32_t v, uint32_t percent) {
  if (percent == 100) return v;
  return detail::saturate(-detail::floor_div(-int64_t{v} * percent, 100));
}
// floor(v * 100 / percent)
constexpr int32_t device_to_units(int32_t v, uint32_t percent) {
  if (percent == 100 || percent == 0) return v;
  return detail::saturate(detail::floor_div(int64_t{v} * 100, percent));
}
// ceil(v * 100 / percent): the units that cover a device extent
constexpr int32_t device_to_units_up(int32_t v, uint32_t percent) {
  if (percent == 100 || percent == 0) return v;
  return detail::saturate(-detail::floor_div(-int64_t{v} * 100, percent));
}
// the device size of a span [x, x + w) of units (its mapped edges)
constexpr uint32_t units_span_to_device(int32_t x, uint32_t w, uint32_t percent) {
  const int64_t a = units_to_device(x, percent), b = units_to_device(detail::saturate(int64_t{x} + w), percent);
  return b > a ? static_cast<uint32_t>(b - a) : 0;
}

static_assert(device_to_units(units_to_device(1, 125), 125) == 1 && device_to_units(units_to_device(3, 150), 150) == 3, "round trip");
static_assert(device_to_units(units_to_device(-7, 175), 175) == -7 && units_to_device(5, 200) == 10, "round trip");

}  // namespace graphics
