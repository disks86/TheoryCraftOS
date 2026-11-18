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

#include "Hardware/GameController/SwitchProtocol.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace nintendo {

namespace {
int16_t stick(int32_t raw, bool flip) {
  return gamepad::axis_from_range(raw, kStickCentre - kStickRange, kStickCentre + kStickRange, flip);
}
}  // namespace

uint32_t encode_handshake(uint32_t step, ByteView out) {
  static constexpr uint8_t kSteps[kHandshakeSteps] = {0x02, 0x03, 0x02, 0x04};
  if (step >= kHandshakeSteps || out.size() < 2) return 0;
  out[0] = 0x80;
  out[1] = kSteps[step];
  return 2;
}

bool decode_full_report(ConstByteView r, gamepad::State* state) {
  if (r.size() < 12 || r[0] != kFullReport) return false;
  const uint8_t right = r[3], shared = r[4], left = r[5];
  uint32_t buttons = 0;
  // Named by position: Nintendo's B is at the bottom, A on the right, Y on the left, X on top.
  if (right & 0x01) buttons |= gamepad::kWest;           // Y
  if (right & 0x02) buttons |= gamepad::kNorth;          // X
  if (right & 0x04) buttons |= gamepad::kSouth;          // B
  if (right & 0x08) buttons |= gamepad::kEast;           // A
  if (right & 0x40) buttons |= gamepad::kRightShoulder;  // R
  if (shared & 0x01) buttons |= gamepad::kBack;          // -
  if (shared & 0x02) buttons |= gamepad::kStart;         // +
  if (shared & 0x04) buttons |= gamepad::kRightStick;
  if (shared & 0x08) buttons |= gamepad::kLeftStick;
  if (shared & 0x10) buttons |= gamepad::kGuide;         // home
  if (shared & 0x20) buttons |= gamepad::kMisc;          // capture
  if (left & 0x01) buttons |= gamepad::kDpadDown;
  if (left & 0x02) buttons |= gamepad::kDpadUp;
  if (left & 0x04) buttons |= gamepad::kDpadRight;
  if (left & 0x08) buttons |= gamepad::kDpadLeft;
  if (left & 0x40) buttons |= gamepad::kLeftShoulder;    // L
  state->buttons = buttons;
  state->left_trigger = (left & 0x80) ? 255 : 0;         // ZL and ZR are buttons on this pad
  state->right_trigger = (right & 0x80) ? 255 : 0;
  const int32_t lx = r[6] | ((r[7] & 0x0F) << 8), ly = (r[7] >> 4) | (r[8] << 4);
  const int32_t rx = r[9] | ((r[10] & 0x0F) << 8), ry = (r[10] >> 4) | (r[11] << 4);
  state->left_x = stick(lx, false);
  state->left_y = stick(ly, false);                      // (up is already the larger value)
  state->right_x = stick(rx, false);
  state->right_y = stick(ry, false);
  const uint8_t battery = r[2] >> 4;                     // bits 3:1 the level (0, 2, 4, 6, 8), bit 0 charging
  state->battery_kind = gamepad::BatteryKind::Rechargeable;
  state->charging = battery & 0x01;
  const uint8_t level = battery >> 1;                    // 0..4
  state->battery_percent = static_cast<uint8_t>(level >= 4 ? 100 : level * 25);
  return true;
}

uint32_t encode_subcommand(uint8_t counter, uint8_t subcommand, uint8_t argument, ByteView out) {
  ByteWriter w(out);
  w.u8(0x01);
  w.u8(counter & 0x0F);
  static constexpr uint8_t kNeutralRumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
  w.bytes(ConstByteView(kNeutralRumble));
  w.u8(subcommand);
  w.u8(argument);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t encode_full_report_mode(uint8_t counter, ByteView out) { return encode_subcommand(counter, 0x03, kFullReport, out); }

uint32_t encode_player_leds(uint8_t counter, uint8_t player, ByteView out) {
  static constexpr uint8_t kPlayers[5] = {0x00, 0x01, 0x03, 0x07, 0x0F};
  return encode_subcommand(counter, 0x30, player <= 4 ? kPlayers[player] : 0x0F, out);
}

}  // namespace nintendo
}  // namespace hardware
