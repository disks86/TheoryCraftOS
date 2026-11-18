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

// Sony's PlayStation controllers over USB: the DualShock 4 (PS4) and the DualSense and DualSense Edge (PS5). Both are HID devices that report at
// once without being asked, as input report 0x01 of 64 bytes, and are driven by one output report (0x05 for the DualShock 4, 0x02 for the
// DualSense) carrying the motors, the light bar and (DualSense) the player LEDs. The layouts are the ones the pads use on USB; Bluetooth shifts
// them and adds a CRC, and is not handled here.
#pragma once
#include "Hardware/GameController/GamepadTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace sony {

constexpr uint16_t kVendor = 0x054C;
constexpr uint16_t kDualShock4 = 0x05C4, kDualShock4v2 = 0x09CC, kDualShock4Adapter = 0x0BA0, kDualSense = 0x0CE6, kDualSenseEdge = 0x0DF2;
constexpr uint8_t kInputReport = 0x01, kDualShock4Output = 0x05, kDualSenseOutput = 0x02;
constexpr uint32_t kDualShock4OutputSize = 32, kDualSenseOutputSize = 48;

// Decodes input report 0x01 (report id included in `report`). False if it is not that report or is too short.
[[nodiscard]] bool decode_dualshock4(ConstByteView report, gamepad::State* state);
[[nodiscard]] bool decode_dualsense(ConstByteView report, gamepad::State* state);

// The output report (id included); returns its size, 0 if `out` is too small.
uint32_t encode_dualshock4_output(const gamepad::Output& output, ByteView out);
uint32_t encode_dualsense_output(const gamepad::Output& output, ByteView out);

}  // namespace sony
}  // namespace hardware
