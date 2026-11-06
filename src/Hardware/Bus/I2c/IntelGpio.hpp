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


// The GPIO pads of an Intel PCH or SoC (pinctrl-intel.c), read for what the pin is doing: the interrupt pin of a touchpad or touchscreen goes to such a pad.
// The pads are in communities, each a memory range of its own. A community's pad configuration starts at the offset its PADBAR register (+0x0C) says,
// and each pad has two 32-bit registers (four on controllers whose REVID is 0x94 or later: Ice Lake on). PADCFG0 bit 1 is the level on the pin, bit 0
// what the pad drives, bits 13:10 the function (0: GPIO). ACPI names a pin by its GPIO number, which the controller's layout
// (Chipset/Intel/IntelGpioLayouts) maps to a pad.
//
// Only reading: the firmware has set the pads up. Not done: interrupts (the pin is polled), changing a pad.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/I2c/IAttentionLine.hpp"
#include "Hardware/Chipset/Intel/IntelGpioLayouts.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {

// What one pad's PADCFG0 says.
struct IntelGpioPad {
  uint32_t padcfg0 = 0;
  uint8_t mode() const { return (padcfg0 >> 10) & 0xF; }   // 0: GPIO, 1..: a native function
  bool gpio() const { return mode() == 0; }
  bool rx_state() const { return padcfg0 & (1u << 1); }
  bool tx_state() const { return padcfg0 & (1u << 0); }
  bool rx_disabled() const { return padcfg0 & (1u << 9); }
  bool tx_disabled() const { return padcfg0 & (1u << 8); }
};

class IntelPchGpio {
 public:
  static constexpr uint32_t kMaxCommunities = 8;
  static constexpr uint32_t kRevid = 0x00, kPadbar = 0x0C, kPadStride = 8, kRxState = 1u << 1;
  static constexpr uint32_t kRevidWithFourRegisters = 0x94;

  // The ACPI IDs (_HID) of the controllers this class knows the layout of
  static bool is_sunrise_point_lp(const char* hid);
  // The communities of `layout` from the controller's memory ranges (`bars`, in _CRS order). Communities whose range is missing or invalid are skipped;
  // false if none could be added.
  bool attach(const IntelGpioLayout& layout, ArrayView<const MmioRegion> bars);
  // A community whose GPIO numbers are its pin numbers (the Sunrise Point-LP numbering). False if the PADBAR register does not point inside the window.
  bool add_community(const MmioRegion& region, uint32_t pin_base, uint32_t pins);
  // The Sunrise Point-LP communities: the memory ranges of the device's _CRS in order.
  static constexpr uint32_t kSptLpCommunities = 3;
  static uint32_t sunrise_point_lp_pin_base(uint32_t community) { return community == 0 ? 0 : community == 1 ? 48 : 120; }
  static uint32_t sunrise_point_lp_pins(uint32_t community) { return community == 0 ? 48 : community == 1 ? 72 : 32; }

  uint32_t community_count() const { return count_; }
  bool has_pin(uint32_t gpio) const;
  // The level on the pin with this GPIO number (true: high). False if no community has it.
  bool level(uint32_t gpio, bool* high) const;
  bool pad(uint32_t gpio, IntelGpioPad* out) const;

 private:
  struct Community {
    MmioRegion region;
    uint32_t first_pin = 0, pins = 0, pad_offset = 0, stride = kPadStride;
    ArrayView<const IntelGpioGroup> groups;   // empty: GPIO number == pin number
  };
  bool add(const MmioRegion& region, uint32_t first_pin, uint32_t pins, ArrayView<const IntelGpioGroup> groups);
  // The community and the pad index in it of a GPIO number.
  const Community* find(uint32_t gpio, uint32_t* pad_index) const;
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
