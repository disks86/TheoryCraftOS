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

// --- Bluetooth. Both pads start with a reduced report 0x01 (sticks, buttons and triggers in the USB report's first ten bytes, no battery) and
// switch to the full one once the host reads a feature report: 0x02 (DualShock 4: then report 0x11, the USB layout two bytes on) or 0x05
// (DualSense: report 0x31, the USB layout one byte on). Output reports are 78 bytes ending in a CRC-32 over 0xA2 and the report.
constexpr uint8_t kDualShock4BtInput = 0x11, kDualSenseBtInput = 0x31, kDualShock4BtOutput = 0x11, kDualSenseBtOutput = 0x31;
constexpr uint8_t kDualShock4BtEnableFeature = 0x02, kDualSenseBtEnableFeature = 0x05;
constexpr uint32_t kBtOutputSize = 78;
[[nodiscard]] bool decode_bt_reduced(ConstByteView report, gamepad::State* state);     // report 0x01 of either pad over Bluetooth
[[nodiscard]] bool decode_dualshock4_bt(ConstByteView report, gamepad::State* state);   // report 0x11
[[nodiscard]] bool decode_dualsense_bt(ConstByteView report, gamepad::State* state);    // report 0x31
uint32_t encode_dualshock4_bt_output(const gamepad::Output& output, ByteView out);
uint32_t encode_dualsense_bt_output(const gamepad::Output& output, uint8_t sequence, ByteView out);
// Whether a Bluetooth output report's CRC is right (for tests and for checking what a pad echoes).
bool bt_output_crc_ok(ConstByteView report);

}  // namespace sony
}  // namespace hardware
