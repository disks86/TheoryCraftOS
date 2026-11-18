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

// Any HID joystick or gamepad (generic desktop usage 4 or 5) that has no protocol of its own here: its input report, as its report descriptor
// lays it out, mapped onto the standard layout of GamepadTypes.hpp.
//
// HID says what each axis is but not where it is on the pad, so the mapping is the convention most PC pads follow (DirectInput mode):
//   - X/Y the left stick; Z/Rz the right stick when both are there, else Rx/Ry; whichever pair is left over the triggers (Rx left, Ry right),
//     as are the simulation page's brake and accelerator;
//   - the hat switch the d-pad;
//   - buttons 1..4 south, east, west, north, 5/6 the shoulders, 7/8 the triggers when the pad has no analog ones, 9 back, 10 start,
//     11/12 the stick clicks, 13 guide; buttons beyond that are left out.
// A pad whose labels differ (a "1 2 3 4" pad, say) still works, with its buttons where the convention puts them.
#pragma once
#include "Hardware/GameController/GamepadTypes.hpp"
#include "Hardware/Hid/HidDescriptor.hpp"

namespace hardware {
namespace hid {

constexpr uint16_t kUsageJoystick = 0x04, kUsageGamepad = 0x05;
constexpr uint16_t kPageSimulation = 0x02;

// The joystick or gamepad report of a device, or null.
const InputReport* find_gamepad_report(const ReportMap& map);

class GamepadDecoder {
 public:
  static constexpr uint32_t kMaxButtons = 13;

  // Finds the axes, the hat and the buttons; false if the report has neither two axes nor a button.
  [[nodiscard]] bool init(const InputReport& report);
  bool valid() const { return valid_; }
  uint32_t capabilities() const;   // gamepad::Capability bits the layout supports
  // The state a report (its data, after the id byte) describes; false if the report is too short for the layout.
  [[nodiscard]] bool decode(ConstByteView data, gamepad::State* out) const;

 private:
  struct Axis {
    bool present = false;
    InputField field;
  };
  int32_t read(ConstByteView data, const InputField& f) const;
  int16_t stick(ConstByteView data, const Axis& a, bool flip) const;
  uint8_t trigger(ConstByteView data, const Axis& a) const;

  bool valid_ = false;
  Axis lx_, ly_, rx_, ry_, lt_, rt_, hat_;
  InputField buttons_;              // the button page's variable field(s): one range ...
  bool has_buttons_ = false;
  InputField button_bits_[kMaxButtons];   // ... or one field per button, as a descriptor with single usages has them
  bool has_button_bit_[kMaxButtons] = {};
  uint32_t min_bytes_ = 0;
};

}  // namespace hid
}  // namespace hardware
