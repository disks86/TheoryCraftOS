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

#include "Hardware/LinuxGamepads.hpp"
#include "Process/LinuxInput.hpp"

namespace hardware {

namespace {

// The process layer's button bits are the service's (it does not include the hardware headers, so they are checked here).
static_assert(uint32_t(process::kPadDpadUp) == gamepad::kDpadUp && uint32_t(process::kPadDpadDown) == gamepad::kDpadDown &&
              uint32_t(process::kPadDpadLeft) == gamepad::kDpadLeft && uint32_t(process::kPadDpadRight) == gamepad::kDpadRight &&
              uint32_t(process::kPadStart) == gamepad::kStart && uint32_t(process::kPadBack) == gamepad::kBack &&
              uint32_t(process::kPadLeftStick) == gamepad::kLeftStick && uint32_t(process::kPadRightStick) == gamepad::kRightStick &&
              uint32_t(process::kPadLeftShoulder) == gamepad::kLeftShoulder && uint32_t(process::kPadRightShoulder) == gamepad::kRightShoulder &&
              uint32_t(process::kPadGuide) == gamepad::kGuide && uint32_t(process::kPadSouth) == gamepad::kSouth &&
              uint32_t(process::kPadEast) == gamepad::kEast && uint32_t(process::kPadWest) == gamepad::kWest &&
              uint32_t(process::kPadNorth) == gamepad::kNorth && uint32_t(process::kPadMisc) == gamepad::kMisc);
static_assert(process::kInputPadSlots == GameControllerService::kMaxPlayers);

bool state_of(void* context, uint32_t slot, process::InputPadState* out) {
  gamepad::State s;
  if (!static_cast<GameControllerService*>(context)->snapshot(slot, &s, nullptr)) return false;
  out->buttons = s.buttons;
  out->left_x = s.left_x;
  out->left_y = s.left_y;
  out->right_x = s.right_x;
  out->right_y = s.right_y;
  out->left_trigger = s.left_trigger;
  out->right_trigger = s.right_trigger;
  return true;
}

bool info_of(void* context, uint32_t slot, process::InputPadInfo* out) {
  gamepad::Info info;
  if (!static_cast<GameControllerService*>(context)->info(slot, &info)) return false;
  out->vendor = info.vendor_id;
  out->product = info.product_id;
  out->version = 0x0100;      // (the service does not keep bcdDevice)
  out->rumble = (info.capabilities & gamepad::kCapRumble) != 0;
  switch (info.protocol) {
    case gamepad::Protocol::DualShock4:
    case gamepad::Protocol::DualSense: out->layout = process::InputPadLayout::PlayStation; break;
    case gamepad::Protocol::SwitchPro: out->layout = process::InputPadLayout::Nintendo; break;
    default: out->layout = process::InputPadLayout::Xpad; break;
  }
  uint64_t length = 0;
  while (length < sizeof info.name && info.name[length] != 0) ++length;
  if (!out->name.assign(StringView(info.name, length))) out->name.clear();
  return true;
}

bool rumble_of(void* context, uint32_t slot, uint16_t strong, uint16_t weak) {
  return static_cast<GameControllerService*>(context)->set_vibration(slot, strong, weak);
}

process::InputPadProvider g_provider;      // (written once at start-up, before any program runs)

}  // namespace

void install_linux_gamepads(GameControllerService* service) {
  if (!service) {
    process::set_input_pad_provider(nullptr);
    return;
  }
  g_provider.context = service;
  g_provider.state = &state_of;
  g_provider.info = &info_of;
  g_provider.rumble = &rumble_of;
  process::set_input_pad_provider(&g_provider);
}

}  // namespace hardware
