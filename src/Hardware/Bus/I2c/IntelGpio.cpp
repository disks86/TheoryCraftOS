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


#include "Hardware/Bus/I2c/IntelGpio.hpp"

namespace hardware {

bool IntelPchGpio::is_sunrise_point_lp(const char* hid) {
  const char want[] = "INT344B";
  for (uint32_t i = 0; i < 8; ++i)
    if (hid[i] != want[i]) return false;
  return true;
}

bool IntelPchGpio::add(const MmioRegion& region, uint32_t first_pin, uint32_t pins, ArrayView<const IntelGpioGroup> groups) {
  if (count_ >= kMaxCommunities || !region.valid() || pins == 0 || region.length() < 0x10) return false;
  const uint32_t pad_offset = region.read32(kPadbar);
  const uint32_t revision = region.read32(kRevid) >> 16;
  const uint32_t stride = revision != 0xFFFF && revision >= kRevidWithFourRegisters ? 2 * kPadStride : kPadStride;
  // the pads must lie inside the window (and a window that reads as all ones is no GPIO community)
  if (pad_offset == 0xFFFFFFFFu || pad_offset < 0x10 || pad_offset > region.length() || static_cast<uint64_t>(pins) * stride > region.length() - pad_offset) return false;
  Community& c = communities_[count_++];
  c.region = region;
  c.first_pin = first_pin;
  c.pins = pins;
  c.pad_offset = pad_offset;
  c.stride = stride;
  c.groups = groups;
  return true;
}

bool IntelPchGpio::add_community(const MmioRegion& region, uint32_t pin_base, uint32_t pins) { return add(region, pin_base, pins, ArrayView<const IntelGpioGroup>()); }

bool IntelPchGpio::attach(const IntelGpioLayout& layout, ArrayView<const MmioRegion> bars) {
  bool any = false;
  for (const IntelGpioCommunityLayout& c : layout.communities) {
    MmioRegion region;
    if (c.last_pin < c.first_pin || !bars.try_get(c.bar, &region)) continue;
    any = add(region, c.first_pin, c.last_pin - c.first_pin + 1u, c.groups) || any;
  }
  return any;
}

const IntelPchGpio::Community* IntelPchGpio::find(uint32_t gpio, uint32_t* pad_index) const {
  for (uint32_t i = 0; i < count_; ++i) {
    const Community& c = communities_[i];
    if (c.groups.empty()) {
      if (gpio >= c.first_pin && gpio - c.first_pin < c.pins) {
        *pad_index = gpio - c.first_pin;
        return &c;
      }
      continue;
    }
    for (const IntelGpioGroup& g : c.groups) {
      if (g.gpio_base < 0 || g.last_pin < g.first_pin || g.first_pin < c.first_pin) continue;
      const uint32_t base = static_cast<uint32_t>(g.gpio_base);
      if (gpio < base || gpio - base > static_cast<uint32_t>(g.last_pin - g.first_pin)) continue;
      const uint32_t index = g.first_pin - c.first_pin + (gpio - base);
      if (index >= c.pins) return nullptr;
      *pad_index = index;
      return &c;
    }
  }
  return nullptr;
}

bool IntelPchGpio::has_pin(uint32_t gpio) const {
  uint32_t index = 0;
  return find(gpio, &index) != nullptr;
}

bool IntelPchGpio::pad(uint32_t gpio, IntelGpioPad* out) const {
  uint32_t index = 0;
  const Community* c = find(gpio, &index);
  if (!c) return false;
  out->padcfg0 = c->region.read32(c->pad_offset + uint64_t{index} * c->stride);
  return true;
}

bool IntelPchGpio::level(uint32_t gpio, bool* high) const {
  IntelGpioPad p;
  if (!pad(gpio, &p)) return false;
  *high = p.rx_state();
  return true;
}

}  // namespace hardware
