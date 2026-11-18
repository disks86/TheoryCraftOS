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

// What a game controller reports and what can be sent back to it, in one model for every kind of pad (Xbox, PlayStation, Nintendo, generic HID).
//
// The layout follows the W3C Gamepad "standard gamepad" and XInput, which agree: four face buttons named by POSITION (south, east, west,
// north; A/B/X/Y on an Xbox pad, cross/circle/square/triangle on a PlayStation one, B/A/Y/X on a Nintendo one), two shoulder buttons, two
// analog triggers, two clickable sticks, a d-pad, back/start and a guide (home) button. Sticks are signed 16-bit with up positive, as XInput
// has them; triggers are 0..255 like XInput's. Whatever a pad has beyond that (a touchpad click, the capture or mute button) is in the extra bits.
#pragma once
#include <stdint.h>

namespace hardware {
namespace gamepad {

// Button bits. The first sixteen are XInput's XINPUT_GAMEPAD_* values, so the Windows layer passes them on as they are.
enum Button : uint32_t {
  kDpadUp = 0x0001,
  kDpadDown = 0x0002,
  kDpadLeft = 0x0004,
  kDpadRight = 0x0008,
  kStart = 0x0010,        // Menu, Options, +
  kBack = 0x0020,         // View, Share/Create, -
  kLeftStick = 0x0040,
  kRightStick = 0x0080,
  kLeftShoulder = 0x0100,
  kRightShoulder = 0x0200,
  kGuide = 0x0400,        // Xbox, PS, Home (XInputGetStateEx reports it)
  kSouth = 0x1000,        // A, cross, B (Nintendo)
  kEast = 0x2000,         // B, circle, A (Nintendo)
  kWest = 0x4000,         // X, square, Y (Nintendo)
  kNorth = 0x8000,        // Y, triangle, X (Nintendo)
  // Beyond XInput:
  kTouchpad = 0x10000,    // the PlayStation touchpad pressed
  kMisc = 0x20000,        // capture (Nintendo), mute (DualSense), share (Xbox Series)
  kPaddle1 = 0x40000,     // Elite paddles / back buttons
  kPaddle2 = 0x80000,
  kPaddle3 = 0x100000,
  kPaddle4 = 0x200000,
};
constexpr uint32_t kXInputButtonMask = 0xF7FF;   // (bit 0x0800 is unused by XInput)

// The d-pad from a HID hat switch: 0 north, clockwise in eighths, anything else (8, 15, a value out of range) centred.
inline uint32_t hat_to_dpad(uint32_t hat) {
  static constexpr uint32_t kHat[8] = {kDpadUp, kDpadUp | kDpadRight, kDpadRight, kDpadDown | kDpadRight,
                                       kDpadDown, kDpadDown | kDpadLeft, kDpadLeft, kDpadUp | kDpadLeft};
  return hat < 8 ? kHat[hat] : 0;
}

// An 8-bit unsigned axis (0..255, centre 128, down/right positive as HID has it) as a signed 16-bit one; `flip` turns "down positive" into
// "up positive" for Y axes.
inline int16_t axis_from_u8(uint8_t value, bool flip) {
  int32_t v = (static_cast<int32_t>(value) - 128) * 257;   // -32896 .. 32639
  if (flip) v = -v - 1;                                    // (-1 so that 0 and 255 map onto the range's ends)
  if (v < -32768) v = -32768;
  if (v > 32767) v = 32767;
  return static_cast<int16_t>(v);
}
// A value in logical_min..logical_max (any HID axis) as a signed 16-bit one. A degenerate range is centred.
inline int16_t axis_from_range(int32_t value, int32_t min, int32_t max, bool flip) {
  if (max <= min) return 0;
  if (value < min) value = min;
  if (value > max) value = max;
  const int64_t span = int64_t{max} - min;
  int64_t v = (int64_t{value} - min) * 65535 / span - 32768;   // -32768 .. 32767
  if (flip) v = -v - 1;
  if (v < -32768) v = -32768;
  if (v > 32767) v = 32767;
  return static_cast<int16_t>(v);
}
inline uint8_t trigger_from_range(int32_t value, int32_t min, int32_t max) {
  if (max <= min) return 0;
  if (value < min) value = min;
  if (value > max) value = max;
  return static_cast<uint8_t>((int64_t{value} - min) * 255 / (int64_t{max} - min));
}

enum class BatteryKind : uint8_t { Unknown, Wired, Alkaline, Rechargeable };

struct State {
  uint32_t buttons = 0;                       // Button bits
  int16_t left_x = 0, left_y = 0;             // up and right positive
  int16_t right_x = 0, right_y = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  BatteryKind battery_kind = BatteryKind::Unknown;
  uint8_t battery_percent = 0;                // meaningful for Alkaline/Rechargeable
  bool charging = false;
};

// What the pad's motors and lights should do. Motors are 0..65535 like XInput's (the low-frequency, heavy one is on the left).
struct Output {
  uint16_t low_frequency = 0, high_frequency = 0;
  uint8_t red = 0, green = 0, blue = 0;       // a light bar (PlayStation)
  uint8_t player = 0;                         // 1..4: the player LEDs; 0 leaves them alone
};

enum class Protocol : uint8_t {
  None,
  Xbox360,          // XInput over USB (vendor class 0xFF, subclass 0x5D, protocol 0x01): Xbox 360 pads and most PC pads
  XboxOne,          // GIP (vendor class 0xFF, subclass 0x47, protocol 0xD0): Xbox One and Series pads
  DualShock4,       // Sony, USB HID report 0x01
  DualSense,        // Sony PS5, USB HID report 0x01
  SwitchPro,        // Nintendo Switch Pro (and wired third-party copies), USB HID report 0x30 after a handshake
  Hid,              // any other HID joystick or gamepad, decoded from its report descriptor
};
const char* protocol_name(Protocol protocol);

enum Capability : uint32_t {
  kCapRumble = 1,
  kCapLightBar = 2,
  kCapPlayerLeds = 4,
  kCapBattery = 8,
  kCapTriggers = 16,      // analog triggers (a generic pad may have digital ones only)
  kCapRightStick = 32,
};

struct Info {
  Protocol protocol = Protocol::None;
  uint16_t vendor_id = 0, product_id = 0;
  uint32_t capabilities = 0;
  char name[48] = {};
};

}  // namespace gamepad
}  // namespace hardware
