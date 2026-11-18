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

#include "Hardware/GameController/WindowsGamepadService.hpp"

namespace hardware {

namespace {
constexpr uint32_t kSuccess = 0, kInvalidParameter = 0xC000000Du, kNotSupported = 0xC00000BBu, kDeviceNotConnected = 0xC000009Du;
enum : uint64_t { kGetState = 1, kSetVibration = 2, kGetInfo = 3, kSetLight = 4 };

uint64_t u16(int16_t v) { return static_cast<uint16_t>(v); }
}  // namespace

uint32_t gamepad_service_call(GameControllerService& service, ArrayView<uint64_t> w) {
  if (w.size() < kGamepadServiceWords) return kInvalidParameter;
  const uint64_t user = w[1];
  if (user >= GameControllerService::kMaxPlayers) return kInvalidParameter;
  const uint32_t player = static_cast<uint32_t>(user);
  switch (w[0]) {
    case kGetState: {
      gamepad::State s;
      uint32_t packet = 0;
      if (!service.snapshot(player, &s, &packet)) return kDeviceNotConnected;
      w[4] = packet;
      w[5] = s.buttons;
      w[6] = uint64_t{s.left_trigger} | uint64_t{s.right_trigger} << 8;
      w[7] = u16(s.left_x) | u16(s.left_y) << 16 | u16(s.right_x) << 32 | u16(s.right_y) << 48;
      return kSuccess;
    }
    case kSetVibration: {
      if (w[2] > 0xFFFF || w[3] > 0xFFFF) return kInvalidParameter;
      gamepad::Info info;
      if (!service.info(player, &info)) return kDeviceNotConnected;
      if (!(info.capabilities & gamepad::kCapRumble)) return kNotSupported;
      // (A transfer that failed is the pad's business: XInput reports success for a connected pad.)
      (void)service.set_vibration(player, static_cast<uint16_t>(w[2]), static_cast<uint16_t>(w[3]));
      return kSuccess;
    }
    case kGetInfo: {
      gamepad::Info info;
      gamepad::State s;
      if (!service.info(player, &info) || !service.snapshot(player, &s, nullptr)) return kDeviceNotConnected;
      w[4] = static_cast<uint64_t>(info.protocol);
      w[5] = uint64_t{info.vendor_id} | uint64_t{info.product_id} << 16;
      w[6] = info.capabilities;
      w[7] = static_cast<uint64_t>(s.battery_kind) | uint64_t{s.battery_percent} << 8 | uint64_t{s.charging ? 1u : 0u} << 16;
      return kSuccess;
    }
    case kSetLight: {
      gamepad::Info info;
      if (!service.info(player, &info)) return kDeviceNotConnected;
      if (!(info.capabilities & gamepad::kCapLightBar)) return kNotSupported;
      (void)service.set_light(player, static_cast<uint8_t>(w[2]), static_cast<uint8_t>(w[2] >> 8), static_cast<uint8_t>(w[2] >> 16));
      return kSuccess;
    }
    default:
      return kInvalidParameter;
  }
}

uint32_t gamepad_service_name(GameControllerService& service, uint64_t player, ByteView out, uint32_t* length) {
  *length = 0;
  if (player >= GameControllerService::kMaxPlayers) return kInvalidParameter;
  gamepad::Info info;
  if (!service.info(static_cast<uint32_t>(player), &info)) return kDeviceNotConnected;
  uint32_t n = 0;
  while (n < sizeof(info.name) && info.name[n]) ++n;
  *length = n;
  const ConstByteView name(reinterpret_cast<const uint8_t*>(info.name), n);   // (char and uint8_t share their representation)
  out.copy_from(name.at_most(out.size()));
  return kSuccess;
}

}  // namespace hardware
