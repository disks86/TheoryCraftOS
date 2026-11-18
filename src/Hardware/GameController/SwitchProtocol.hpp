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

// The Nintendo Switch Pro Controller over USB (and the wired pads that copy its protocol). It is a HID device, but says nothing useful until
// the host has shaken hands: output reports 0x80 0x02 (handshake), 0x80 0x03 (fast link), 0x80 0x02 again and 0x80 0x04 (USB only, no
// Bluetooth timeout); after that (and subcommand 0x03, to be sure of the mode) it sends the full input report 0x30 every 8 ms. Player LEDs are subcommand 0x30 inside output report 0x01.
// Sticks are 12-bit and calibrated per pad in its SPI flash, which is not read: a nominal centre and range are assumed (kStickCentre/Range).
#pragma once
#include "Hardware/GameController/GamepadTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace nintendo {

constexpr uint16_t kVendor = 0x057E, kSwitchPro = 0x2009;
constexpr uint8_t kFullReport = 0x30;
constexpr int32_t kStickCentre = 2048, kStickRange = 1600;   // (a typical factory calibration: about 1600 counts each way)

// The handshake: `step` 0..3, each a two-byte output report written to `out`; returns its size (0 past the last step or if `out` is too small).
uint32_t encode_handshake(uint32_t step, ByteView out);
constexpr uint32_t kHandshakeSteps = 4;

// Decodes the full input report 0x30 (report id included). False for other reports (0x81 handshake replies, 0x21 subcommand replies) and short ones.
[[nodiscard]] bool decode_full_report(ConstByteView report, gamepad::State* state);

// Output report 0x01: neutral rumble and one subcommand with a one-byte argument. `counter` is the sender's running count (low 4 bits).
// Returns the report's size (0 if `out` is too small).
uint32_t encode_subcommand(uint8_t counter, uint8_t subcommand, uint8_t argument, ByteView out);
// Subcommand 0x03: report in full mode (0x30), sent after the handshake.
uint32_t encode_full_report_mode(uint8_t counter, ByteView out);
// Subcommand 0x30: player 1..4's LEDs lit (0: all off).
uint32_t encode_player_leds(uint8_t counter, uint8_t player, ByteView out);

}  // namespace nintendo
}  // namespace hardware
