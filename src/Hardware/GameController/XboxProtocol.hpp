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

// The two USB protocols of Xbox controllers.
//
//   - Xbox 360 ("XInput"): vendor-specific interface class 0xFF, subclass 0x5D, protocol 0x01. The pad reports at once: a 20-byte message of
//     type 0x00 whose buttons are XInput's own bits. Rumble and the ring of LEDs are messages to the interrupt OUT endpoint. Most PC pads
//     (Logitech F310/F710 in X mode, PowerA, PDP, Hori, 8BitDo in X mode) speak this.
//   - Xbox One and Series ("GIP", the Gaming Input Protocol): class 0xFF, subclass 0x47, protocol 0xD0. Every message has a 4-byte header
//     (command, flags, sequence, length). The pad stays silent until it is told to power on; then command 0x20 carries the input, 0x07 the
//     guide button (which wants an acknowledgement), and 0x09 sent to it drives the four motors.
//
// The messages come from the device and are untrusted: every decoder checks the length first and leaves the state as it was on a short one.
#pragma once
#include "Hardware/GameController/GamepadTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace xbox {

constexpr uint8_t kInterfaceClass = 0xFF;
constexpr uint8_t kXbox360Subclass = 0x5D, kXbox360Protocol = 0x01;
constexpr uint8_t kGipSubclass = 0x47, kGipProtocol = 0xD0;

// The Xbox 360 Wireless Receiver (045E:0719, 045E:0291): one interface of class 0xFF, subclass 0x5D, protocol 0x81 per pad it can pair (four),
// each with its own interrupt IN and OUT endpoints. A pad's interface stays silent until it is asked (encode_360w_inquiry); then it says when
// a pad pairs or leaves (a message with bit 3 of byte 0 set; bit 7 of byte 1: a pad is there) and carries the pad's input as the wired
// report's body from byte 4 on (byte 1 bit 0 set).
constexpr uint8_t kXbox360WirelessProtocol = 0x81;
enum class WirelessEvent : uint8_t { None, Connected, Disconnected, Input };
[[nodiscard]] WirelessEvent decode_360w(ConstByteView message, gamepad::State* state);
uint32_t encode_360w_inquiry(ByteView out);                                          // "is a pad there?" (12 bytes)
uint32_t encode_360w_led(uint8_t player, ByteView out);                              // the ring of a paired pad (12 bytes)
uint32_t encode_360w_rumble(uint16_t low_frequency, uint16_t high_frequency, ByteView out);   // (12 bytes)

// Xbox 360: decodes an input message (type 0x00, length 0x14). False for the other messages (LED and rumble status) and short ones.
[[nodiscard]] bool decode_360(ConstByteView message, gamepad::State* state);
// The rumble message (8 bytes) for the two motors; returns the bytes written, 0 if `out` is too small.
uint32_t encode_360_rumble(uint16_t low_frequency, uint16_t high_frequency, ByteView out);
// The LED message (3 bytes): player 1..4 lights that quadrant of the ring, 0 turns it off.
uint32_t encode_360_led(uint8_t player, ByteView out);

// Xbox pads over Bluetooth (One S, Series, Elite 2): a HID gamepad (HidGamepad decodes it) whose buttons come in this order, and rumble as
// output report 3: motor enables, the trigger motors, the main motors (0..100), duration, delay and repeat.
constexpr uint32_t kBluetoothButtons = 16;
extern const uint32_t kBluetoothButtonMap[kBluetoothButtons];
uint32_t encode_bt_rumble(uint16_t low_frequency, uint16_t high_frequency, ByteView out);   // (9 bytes, report id first)

// GIP commands.
constexpr uint8_t kGipAcknowledge = 0x01, kGipStatus = 0x03, kGipPower = 0x05, kGipGuide = 0x07, kGipRumble = 0x09, kGipLed = 0x0A,
                  kGipInput = 0x20;
constexpr uint8_t kGipFlagNeedsAck = 0x10, kGipFlagInternal = 0x20;

// What decode_gip found in one message.
enum class GipEvent : uint8_t { None, Input, Guide, Status };
struct GipResult {
  GipEvent event = GipEvent::None;
  bool needs_ack = false;      // the pad wants an acknowledgement of this message (encode_gip_ack)
  uint8_t command = 0, sequence = 0, length = 0;
};
// Decodes a GIP message into `state` (input: sticks, triggers, buttons; guide: the guide bit). Unknown commands are GipEvent::None.
[[nodiscard]] GipResult decode_gip(ConstByteView message, gamepad::State* state);

// Messages to the pad; each returns the bytes written (0 if `out` is too small). `sequence` is the sender's running message count; an
// acknowledgement takes the sequence number of the message it answers.
uint32_t encode_gip_power_on(uint8_t sequence, ByteView out);
uint32_t encode_gip_led(uint8_t sequence, uint8_t brightness, ByteView out);     // the guide button's light (brightness 0..0x20)
uint32_t encode_gip_rumble(uint8_t sequence, uint16_t low_frequency, uint16_t high_frequency, ByteView out);
uint32_t encode_gip_ack(const GipResult& acknowledged, ByteView out);

}  // namespace xbox
}  // namespace hardware
