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

bool IntelPchGpio::add_community(const MmioRegion& region, uint32_t pin_base, uint32_t pins) {
  if (count_ >= kMaxCommunities || !region.valid() || pins == 0) return false;
  const uint32_t pad_offset = region.read32(kPadbar);
  // the pads must lie inside the window (and a window that reads as all ones is no GPIO community)
  if (pad_offset == 0xFFFFFFFFu || pad_offset < 0x10 || pad_offset > region.length() || static_cast<uint64_t>(pins) * kPadStride > region.length() - pad_offset) return false;
  Community& c = communities_[count_++];
  c.region = region;
  c.pin_base = pin_base;
  c.pins = pins;
  c.pad_offset = pad_offset;
  return true;
}

const IntelPchGpio::Community* IntelPchGpio::find(uint32_t pin) const {
  for (uint32_t i = 0; i < count_; ++i)
    if (pin >= communities_[i].pin_base && pin - communities_[i].pin_base < communities_[i].pins) return &communities_[i];
  return nullptr;
}

bool IntelPchGpio::level(uint32_t pin, bool* high) const {
  const Community* c = find(pin);
  if (!c) return false;
  *high = (c->region.read32(c->pad_offset + (pin - c->pin_base) * kPadStride) & kRxState) != 0;
  return true;
}

}  // namespace hardware
