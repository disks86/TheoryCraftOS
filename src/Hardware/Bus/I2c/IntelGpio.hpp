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


// The pads of the Intel PCH (the "Sunrise Point-LP GPIO" of the Dell XPS 15 9575, ACPI INT344B), read for what the pin is doing: the interrupt pin of the touchpad and the touchscreen
// goes to such a pad. The pads are in communities, each a memory range of its own; a community's pad configuration starts at the offset its PADBAR register (+0x0C) says, and a pad has
// two 32 bit registers; bit 1 of the first is the level on the pin (pinctrl-intel.c, pinctrl-sunrisepoint.c). The ACPI table names a pin by its number over all the communities:
// pins 0-47 are in the first memory range of the device's _CRS, 48-119 in the second, 120-151 in the third.
//
// Only reading: the firmware has set the pads up. Not done: interrupts (the pin is polled), changing a pad.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/I2c/IAttentionLine.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {

class IntelPchGpio {
 public:
  static constexpr uint32_t kMaxCommunities = 4;
  static constexpr uint32_t kPadbar = 0x0C, kPadStride = 8, kRxState = 1u << 1;

  // The ACPI IDs (_HID) of the controllers this class knows the layout of
  static bool is_sunrise_point_lp(const char* hid);
  // A community's registers and the pins in it. False if the PADBAR register does not point inside the window.
  bool add_community(const MmioRegion& region, uint32_t pin_base, uint32_t pins);
  // The Sunrise Point-LP communities: the memory ranges of the device's _CRS in order.
  static constexpr uint32_t kSptLpCommunities = 3;
  static uint32_t sunrise_point_lp_pin_base(uint32_t community) { return community == 0 ? 0 : community == 1 ? 48 : 120; }
  static uint32_t sunrise_point_lp_pins(uint32_t community) { return community == 0 ? 48 : community == 1 ? 72 : 32; }

  bool has_pin(uint32_t pin) const { return find(pin) != nullptr; }
  // The level on the pin (true: high). False if the pin is in no community.
  bool level(uint32_t pin, bool* high) const;

 private:
  struct Community {
    MmioRegion region;
    uint32_t pin_base = 0, pins = 0, pad_offset = 0;
  };
  const Community* find(uint32_t pin) const;
  Community communities_[kMaxCommunities];
  uint32_t count_ = 0;
};

// An attention line on a pad: asserted when the pin is at its active level.
class GpioAttentionLine final : public IAttentionLine {
 public:
  GpioAttentionLine(const IntelPchGpio& gpio, uint32_t pin, bool active_low) : gpio_(gpio), pin_(pin), active_low_(active_low) {}
  bool asserted() override {
    bool high = false;
    return gpio_.level(pin_, &high) && high != active_low_;
  }

 private:
  const IntelPchGpio& gpio_;
  uint32_t pin_;
  bool active_low_;
};

}  // namespace hardware
