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

#include "Hardware/GameController/SonyProtocol.hpp"

namespace hardware {
namespace sony {

namespace {
// The byte both pads share: the hat in the low nibble, then square, cross, circle, triangle.
uint32_t hat_and_face(uint8_t b) {
  uint32_t buttons = gamepad::hat_to_dpad(b & 0x0F);
  if (b & 0x10) buttons |= gamepad::kWest;    // square
  if (b & 0x20) buttons |= gamepad::kSouth;   // cross
  if (b & 0x40) buttons |= gamepad::kEast;    // circle
  if (b & 0x80) buttons |= gamepad::kNorth;   // triangle
  return buttons;
}
// And the next one: L1, R1, L2, R2 (digital), share/create, options, L3, R3.
uint32_t shoulders(uint8_t b) {
  uint32_t buttons = 0;
  if (b & 0x01) buttons |= gamepad::kLeftShoulder;
  if (b & 0x02) buttons |= gamepad::kRightShoulder;
  if (b & 0x10) buttons |= gamepad::kBack;
  if (b & 0x20) buttons |= gamepad::kStart;
  if (b & 0x40) buttons |= gamepad::kLeftStick;
  if (b & 0x80) buttons |= gamepad::kRightStick;
  return buttons;
}
uint8_t level_percent(uint8_t level) { return level >= 10 ? 100 : static_cast<uint8_t>(level * 10); }
}  // namespace

bool decode_dualshock4(ConstByteView r, gamepad::State* state) {
  if (r.size() < 31 || r[0] != kInputReport) return false;
  state->left_x = gamepad::axis_from_u8(r[1], false);
  state->left_y = gamepad::axis_from_u8(r[2], true);
  state->right_x = gamepad::axis_from_u8(r[3], false);
  state->right_y = gamepad::axis_from_u8(r[4], true);
  uint32_t buttons = hat_and_face(r[5]) | shoulders(r[6]);
  if (r[7] & 0x01) buttons |= gamepad::kGuide;
  if (r[7] & 0x02) buttons |= gamepad::kTouchpad;
  state->buttons = buttons;
  state->left_trigger = r[8];
  state->right_trigger = r[9];
  const uint8_t power = r[30];                 // bit 4: the cable is in; low nibble: 0..10 on battery, 0..11 while charging (11: full)
  const bool cable = power & 0x10;
  const uint8_t level = power & 0x0F;
  state->battery_kind = gamepad::BatteryKind::Rechargeable;
  state->charging = cable && level < 11;
  state->battery_percent = level_percent(level);
  return true;
}

bool decode_dualsense(ConstByteView r, gamepad::State* state) {
  if (r.size() < 54 || r[0] != kInputReport) return false;
  state->left_x = gamepad::axis_from_u8(r[1], false);
  state->left_y = gamepad::axis_from_u8(r[2], true);
  state->right_x = gamepad::axis_from_u8(r[3], false);
  state->right_y = gamepad::axis_from_u8(r[4], true);
  state->left_trigger = r[5];
  state->right_trigger = r[6];
  uint32_t buttons = hat_and_face(r[8]) | shoulders(r[9]);
  const uint8_t misc = r[10];
  if (misc & 0x01) buttons |= gamepad::kGuide;
  if (misc & 0x02) buttons |= gamepad::kTouchpad;
  if (misc & 0x04) buttons |= gamepad::kMisc;      // mute
  if (misc & 0x10) buttons |= gamepad::kPaddle3;   // (Edge) left function button
  if (misc & 0x20) buttons |= gamepad::kPaddle4;   // (Edge) right function button
  if (misc & 0x40) buttons |= gamepad::kPaddle1;   // (Edge) left back paddle
  if (misc & 0x80) buttons |= gamepad::kPaddle2;   // (Edge) right back paddle
  state->buttons = buttons;
  const uint8_t power = r[53];                     // low nibble: 0..10; high nibble: 0 on battery, 1 charging, 2 full
  const uint8_t status = power >> 4;
  state->battery_kind = gamepad::BatteryKind::Rechargeable;
  state->charging = status == 1;
  state->battery_percent = status == 2 ? 100 : level_percent(power & 0x0F);
  return true;
}

uint32_t encode_dualshock4_output(const gamepad::Output& o, ByteView out) {
  if (out.size() < kDualShock4OutputSize) return 0;
  ByteView r = out.first(kDualShock4OutputSize);
  r.fill(0);
  r[0] = kDualShock4Output;
  r[1] = 0x01 | 0x02;                                   // motors and light bar valid
  r[4] = static_cast<uint8_t>(o.high_frequency >> 8);   // the small (right) motor
  r[5] = static_cast<uint8_t>(o.low_frequency >> 8);    // the big (left) motor
  r[6] = o.red;
  r[7] = o.green;
  r[8] = o.blue;
  return kDualShock4OutputSize;
}

uint32_t encode_dualsense_output(const gamepad::Output& o, ByteView out) {
  if (out.size() < kDualSenseOutputSize) return 0;
  ByteView r = out.first(kDualSenseOutputSize);
  r.fill(0);
  r[0] = kDualSenseOutput;
  r[1] = 0x01 | 0x02;                                   // compatible vibration, haptics select
  r[2] = 0x04 | (o.player ? 0x10 : 0x00);               // light bar, player LEDs
  r[3] = static_cast<uint8_t>(o.high_frequency >> 8);   // right motor
  r[4] = static_cast<uint8_t>(o.low_frequency >> 8);    // left motor
  r[39] = 0x02;                                         // light bar setup valid
  r[42] = 0x02;                                         // light bar setup: fade the start-up light out, so ours shows
  // The five player LEDs: the patterns the console uses for players 1..4 (centre; two inner; outer and centre; four).
  static constexpr uint8_t kPlayers[5] = {0x00, 0x04, 0x0A, 0x15, 0x1B};
  r[44] = o.player <= 4 ? kPlayers[o.player] : 0x1F;
  r[45] = o.red;
  r[46] = o.green;
  r[47] = o.blue;
  return kDualSenseOutputSize;
}

}  // namespace sony
}  // namespace hardware
