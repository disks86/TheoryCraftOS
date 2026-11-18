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

#include "Hardware/GameController/XboxProtocol.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace xbox {

namespace {
// Y axes come from both protocols up-positive already (as XInput has them).
int16_t le16s(ConstByteView m, uint32_t offset) { return static_cast<int16_t>(m[offset] | (m[offset + 1] << 8)); }
uint16_t le16(ConstByteView m, uint32_t offset) { return static_cast<uint16_t>(m[offset] | (m[offset + 1] << 8)); }

uint32_t finish(const ByteWriter& w) { return w.ok() ? static_cast<uint32_t>(w.position()) : 0; }
}  // namespace

bool decode_360(ConstByteView m, gamepad::State* state) {
  if (m.size() < 14 || m[0] != 0x00 || m[1] < 0x14) return false;
  state->buttons = (state->buttons & ~gamepad::kXInputButtonMask) | (le16(m, 2) & gamepad::kXInputButtonMask);
  state->left_trigger = m[4];
  state->right_trigger = m[5];
  state->left_x = le16s(m, 6);
  state->left_y = le16s(m, 8);
  state->right_x = le16s(m, 10);
  state->right_y = le16s(m, 12);
  state->battery_kind = gamepad::BatteryKind::Wired;
  return true;
}

uint32_t encode_360_rumble(uint16_t low_frequency, uint16_t high_frequency, ByteView out) {
  ByteWriter w(out);
  w.u8(0x00);
  w.u8(0x08);
  w.u8(0x00);
  w.u8(static_cast<uint8_t>(low_frequency >> 8));    // the big (left) motor
  w.u8(static_cast<uint8_t>(high_frequency >> 8));   // the small (right) motor
  w.zeros(3);
  return finish(w);
}

uint32_t encode_360_led(uint8_t player, ByteView out) {
  ByteWriter w(out);
  w.u8(0x01);
  w.u8(0x03);
  w.u8(player >= 1 && player <= 4 ? static_cast<uint8_t>(5 + player) : 0x00);   // 6..9: quadrant 1..4 on; 0: off
  return finish(w);
}

GipResult decode_gip(ConstByteView m, gamepad::State* state) {
  GipResult r;
  if (m.size() < 4) return r;
  r.command = m[0];
  r.needs_ack = (m[1] & kGipFlagNeedsAck) != 0;
  r.sequence = m[2];
  r.length = m[3];
  const uint64_t payload = m.size() - 4;
  if (r.length > payload) {          // a message that claims more than arrived
    r.needs_ack = false;
    return r;
  }
  switch (r.command) {
    case kGipInput: {
      if (r.length < 10) return r;               // (payload: buttons 2, triggers 4, left stick 4, right stick 4)
      const uint8_t a = m[4], b = m[5];
      uint32_t buttons = 0;
      if (a & 0x04) buttons |= gamepad::kStart;          // menu
      if (a & 0x08) buttons |= gamepad::kBack;           // view
      if (a & 0x10) buttons |= gamepad::kSouth;
      if (a & 0x20) buttons |= gamepad::kEast;
      if (a & 0x40) buttons |= gamepad::kWest;
      if (a & 0x80) buttons |= gamepad::kNorth;
      if (b & 0x01) buttons |= gamepad::kDpadUp;
      if (b & 0x02) buttons |= gamepad::kDpadDown;
      if (b & 0x04) buttons |= gamepad::kDpadLeft;
      if (b & 0x08) buttons |= gamepad::kDpadRight;
      if (b & 0x10) buttons |= gamepad::kLeftShoulder;
      if (b & 0x20) buttons |= gamepad::kRightShoulder;
      if (b & 0x40) buttons |= gamepad::kLeftStick;
      if (b & 0x80) buttons |= gamepad::kRightStick;
      constexpr uint32_t kOwn = gamepad::kStart | gamepad::kBack | gamepad::kSouth | gamepad::kEast | gamepad::kWest | gamepad::kNorth |
                                gamepad::kDpadUp | gamepad::kDpadDown | gamepad::kDpadLeft | gamepad::kDpadRight | gamepad::kLeftShoulder |
                                gamepad::kRightShoulder | gamepad::kLeftStick | gamepad::kRightStick;
      state->buttons = (state->buttons & ~kOwn) | buttons;   // (the guide button comes in its own message)
      const uint16_t lt = le16(m, 6) & 0x3FF, rt = le16(m, 8) & 0x3FF;   // 0..1023
      state->left_trigger = static_cast<uint8_t>(lt >> 2);
      state->right_trigger = static_cast<uint8_t>(rt >> 2);
      state->left_x = le16s(m, 10);
      state->left_y = le16s(m, 12);
      state->right_x = r.length >= 14 ? le16s(m, 14) : 0;
      state->right_y = r.length >= 14 ? le16s(m, 16) : 0;
      state->battery_kind = gamepad::BatteryKind::Wired;
      r.event = GipEvent::Input;
      return r;
    }
    case kGipGuide:
      if (r.length < 1) return r;
      if (m[4] & 0x01) state->buttons |= gamepad::kGuide;
      else state->buttons &= ~static_cast<uint32_t>(gamepad::kGuide);
      r.event = GipEvent::Guide;
      return r;
    case kGipStatus:
      r.event = GipEvent::Status;
      return r;
    default:
      return r;
  }
}

uint32_t encode_gip_power_on(uint8_t sequence, ByteView out) {
  ByteWriter w(out);
  w.u8(kGipPower);
  w.u8(kGipFlagInternal);
  w.u8(sequence);
  w.u8(0x01);
  w.u8(0x00);   // power on
  return finish(w);
}

uint32_t encode_gip_led(uint8_t sequence, uint8_t brightness, ByteView out) {
  ByteWriter w(out);
  w.u8(kGipLed);
  w.u8(kGipFlagInternal);
  w.u8(sequence);
  w.u8(0x03);
  w.u8(0x00);
  w.u8(0x01);   // steady
  w.u8(brightness > 0x20 ? 0x20 : brightness);
  return finish(w);
}

uint32_t encode_gip_rumble(uint8_t sequence, uint16_t low_frequency, uint16_t high_frequency, ByteView out) {
  ByteWriter w(out);
  w.u8(kGipRumble);
  w.u8(0x00);
  w.u8(sequence);
  w.u8(0x09);
  w.u8(0x00);
  w.u8(0x0F);                                          // all four motors take part
  w.u8(0x00);                                          // left trigger motor
  w.u8(0x00);                                          // right trigger motor
  w.u8(static_cast<uint8_t>(low_frequency / 656));     // strong (left), 0..100 (65535 / 656 = 99.9)
  w.u8(static_cast<uint8_t>(high_frequency / 656));    // weak (right)
  w.u8(0xFF);                                          // on for as long as possible
  w.u8(0x00);                                          // no delay
  w.u8(0xFF);                                          // repeated
  return finish(w);
}

uint32_t encode_gip_ack(const GipResult& acknowledged, ByteView out) {
  ByteWriter w(out);
  w.u8(kGipAcknowledge);
  w.u8(kGipFlagInternal);
  w.u8(acknowledged.sequence);   // (an acknowledgement carries the sequence number of the message it acknowledges)
  w.u8(0x09);
  w.u8(0x00);
  w.u8(acknowledged.command);
  w.u8(kGipFlagInternal);
  w.u8(acknowledged.length);
  w.zeros(5);
  return finish(w);
}

}  // namespace xbox
}  // namespace hardware
