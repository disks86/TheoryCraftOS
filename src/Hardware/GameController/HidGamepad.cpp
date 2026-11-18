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

#include "Hardware/GameController/HidGamepad.hpp"

namespace hardware {
namespace hid {

namespace {
constexpr uint16_t kUsageZ = 0x32, kUsageRx = 0x33, kUsageRy = 0x34, kUsageRz = 0x35, kUsageHat = 0x39;
constexpr uint16_t kUsageAccelerator = 0xC4, kUsageBrake = 0xC5;

// The standard button each HID button (1-based) stands for; the triggers (7, 8) are handled apart.
constexpr uint32_t kButtonMap[GamepadDecoder::kMaxButtons] = {
    gamepad::kSouth, gamepad::kEast, gamepad::kWest, gamepad::kNorth, gamepad::kLeftShoulder, gamepad::kRightShoulder,
    0, 0, gamepad::kBack, gamepad::kStart, gamepad::kLeftStick, gamepad::kRightStick, gamepad::kGuide};

bool is_signed(const InputField& f) { return f.logical_min < 0; }
uint32_t field_end(const InputField& f) { return f.bit_offset + f.bit_size * (f.count ? f.count : 1); }
}  // namespace

const InputReport* find_gamepad_report(const ReportMap& map) {
  if (const InputReport* r = map.find_application(kPageGenericDesktop, kUsageGamepad)) return r;
  return map.find_application(kPageGenericDesktop, kUsageJoystick);
}

bool GamepadDecoder::init(const InputReport& report) {
  *this = GamepadDecoder();
  Axis x, y, z, rx, ry, rz, accel, brake;
  for (uint32_t i = 0; i < report.field_count; ++i) {
    const InputField& f = report.fields[i];
    if (f.bit_size == 0 || f.bit_size > 32) continue;
    if (f.page == kPageGenericDesktop && f.variable && f.usage_min == f.usage_max) {
      Axis* slot = nullptr;
      switch (f.usage_min) {
        case kUsageX: slot = &x; break;
        case kUsageY: slot = &y; break;
        case kUsageZ: slot = &z; break;
        case kUsageRx: slot = &rx; break;
        case kUsageRy: slot = &ry; break;
        case kUsageRz: slot = &rz; break;
        case kUsageHat: slot = &hat_; break;
        default: break;
      }
      if (slot && !slot->present) { slot->present = true; slot->field = f; }
    } else if (f.page == kPageSimulation && f.variable && f.usage_min == f.usage_max) {
      if (f.usage_min == kUsageAccelerator && !accel.present) { accel.present = true; accel.field = f; }
      if (f.usage_min == kUsageBrake && !brake.present) { brake.present = true; brake.field = f; }
    } else if (f.page == kPageButton && f.variable && f.bit_size == 1) {
      if (f.usage_min == f.usage_max && f.count == 1) {          // a button on its own
        if (f.usage_min >= 1 && f.usage_min <= kMaxButtons && !has_button_bit_[f.usage_min - 1]) {
          has_button_bit_[f.usage_min - 1] = true;
          button_bits_[f.usage_min - 1] = f;
        }
      } else if (!has_buttons_ && f.usage_min >= 1) {
        has_buttons_ = true;
        buttons_ = f;
      }
    } else {
      continue;
    }
    const uint32_t end = field_end(f);
    if ((end + 7) / 8 > min_bytes_) min_bytes_ = (end + 7) / 8;
  }
  lx_ = x;
  ly_ = y;
  if (z.present && rz.present) {
    rx_ = z;
    ry_ = rz;
    lt_ = rx;
    rt_ = ry;
  } else if (rx.present && ry.present) {
    rx_ = rx;
    ry_ = ry;
    lt_ = z;                                   // (a lone Z: one combined trigger axis on some pads; treat it as the left one)
  }
  if (!lt_.present && brake.present) lt_ = brake;
  if (!rt_.present && accel.present) rt_ = accel;
  bool any_button = has_buttons_;
  for (bool b : has_button_bit_) any_button = any_button || b;
  valid_ = (lx_.present && ly_.present) || any_button;
  return valid_;
}

uint32_t GamepadDecoder::capabilities() const {
  uint32_t caps = 0;
  if (lt_.present && rt_.present) caps |= gamepad::kCapTriggers;
  if (rx_.present && ry_.present) caps |= gamepad::kCapRightStick;
  return caps;
}

int32_t GamepadDecoder::read(ConstByteView data, const InputField& f) const {
  return extract_bits(data, f.bit_offset, f.bit_size, is_signed(f));
}

int16_t GamepadDecoder::stick(ConstByteView data, const Axis& a, bool flip) const {
  if (!a.present) return 0;
  return gamepad::axis_from_range(read(data, a.field), a.field.logical_min, a.field.logical_max, flip);
}

uint8_t GamepadDecoder::trigger(ConstByteView data, const Axis& a) const {
  if (!a.present) return 0;
  return gamepad::trigger_from_range(read(data, a.field), a.field.logical_min, a.field.logical_max);
}

bool GamepadDecoder::decode(ConstByteView data, gamepad::State* out) const {
  if (!valid_ || data.size() < min_bytes_) return false;
  gamepad::State s;
  s.battery_kind = gamepad::BatteryKind::Wired;
  s.left_x = stick(data, lx_, false);
  s.left_y = stick(data, ly_, true);           // HID: down is positive
  s.right_x = stick(data, rx_, false);
  s.right_y = stick(data, ry_, true);
  s.left_trigger = trigger(data, lt_);
  s.right_trigger = trigger(data, rt_);
  if (hat_.present) {
    const int32_t value = read(data, hat_.field) - hat_.field.logical_min;   // out of range (the null state): centred
    const int32_t span = hat_.field.logical_max - hat_.field.logical_min;
    if (value >= 0 && value <= span && span == 7) s.buttons |= gamepad::hat_to_dpad(static_cast<uint32_t>(value));
    else if (value >= 0 && value <= span && span == 3) s.buttons |= gamepad::hat_to_dpad(static_cast<uint32_t>(value) * 2);   // a four-way hat
  }
  bool pressed[kMaxButtons] = {};
  if (has_buttons_) {
    for (uint32_t i = 0; i < buttons_.count; ++i) {
      const uint32_t usage = buttons_.usage_min + i;
      if (usage < 1 || usage > kMaxButtons || usage > buttons_.usage_max) break;
      pressed[usage - 1] = extract_bits(data, buttons_.bit_offset + i, 1, false) != 0;
    }
  }
  for (uint32_t b = 0; b < kMaxButtons; ++b)
    if (has_button_bit_[b]) pressed[b] = pressed[b] || extract_bits(data, button_bits_[b].bit_offset, 1, false) != 0;
  for (uint32_t b = 0; b < kMaxButtons; ++b)
    if (pressed[b]) s.buttons |= kButtonMap[b];
  if (!lt_.present && pressed[6]) s.left_trigger = 255;      // digital triggers: buttons 7 and 8
  if (!rt_.present && pressed[7]) s.right_trigger = 255;
  *out = s;
  return true;
}

}  // namespace hid
}  // namespace hardware
