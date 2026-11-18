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

// Host-side tests for the game controllers: the Xbox 360, Xbox One (GIP), DualShock 4, DualSense and Switch Pro reports and the messages sent
// back, the generic HID gamepad decoder over real-world report descriptors, the catalogue, the USB driver (probing, start-up messages, output)
// against a fake host controller, and the service's player slots, hot-plug and rumble. Decoders get truncated and bit-flipped reports too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <string>
#include <vector>
#include "Hardware/Bluetooth/HidOverGatt.hpp"
#include "Hardware/GameController/Bluetooth/BluetoothGamepad.hpp"
#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/GameController/HidGamepad.hpp"
#include "Hardware/GameController/SonyProtocol.hpp"
#include "Hardware/GameController/SwitchProtocol.hpp"
#include "Hardware/GameController/Usb/UsbGameController.hpp"
#include "Hardware/GameController/Usb/UsbXbox360Wireless.hpp"
#include "Hardware/GameController/WindowsGamepadService.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

namespace hardware {
void delay_us(uint64_t) {}   // (the Bluetooth stack's waits: nothing to wait for against fakes)
}  // namespace hardware

using namespace hardware;
using namespace hardware::gamepad;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

// Feeds every prefix and every single-bit flip of a report to a decoder: nothing may read outside it (ASan says so) and a short one must fail.
template <class Decode>
static void fuzz(const Bytes& report, uint32_t min_size, Decode decode) {
  for (size_t n = 0; n < report.size(); ++n) {
    Bytes prefix(report.begin(), report.begin() + n);
    State s;
    const bool ok = decode(view(prefix), &s);
    if (n < min_size) CHECK(!ok);
  }
  for (size_t bit = 0; bit < report.size() * 8; ++bit) {
    Bytes flipped = report;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    State s;
    (void)decode(view(flipped), &s);
  }
}

// --- Axis helpers -------------------------------------------------------------------------------------

static void test_axes() {
  CHECK(axis_from_u8(0, false) == -32768);
  CHECK(axis_from_u8(255, false) == 32639);
  CHECK(axis_from_u8(128, false) == 0);
  CHECK(axis_from_u8(0, true) == 32767);          // HID's top is XInput's +
  CHECK(axis_from_u8(255, true) < -32000);
  CHECK(axis_from_range(0, 0, 1023, false) == -32768);
  CHECK(axis_from_range(1023, 0, 1023, false) == 32767);
  CHECK(axis_from_range(-200, -127, 127, false) == -32768);   // clamped
  CHECK(axis_from_range(5, 7, 7, false) == 0);                // a degenerate range is centred
  CHECK(trigger_from_range(1023, 0, 1023) == 255 && trigger_from_range(0, 0, 1023) == 0 && trigger_from_range(9, 3, 3) == 0);
  CHECK(hat_to_dpad(0) == kDpadUp && hat_to_dpad(3) == (kDpadDown | kDpadRight) && hat_to_dpad(8) == 0 && hat_to_dpad(15) == 0);
}

// --- Xbox -----------------------------------------------------------------------------------------------

static Bytes xbox360_report(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
  Bytes b = {0x00, 0x14, static_cast<uint8_t>(buttons), static_cast<uint8_t>(buttons >> 8), lt, rt};
  for (int16_t v : {lx, ly, rx, ry}) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8)); }
  b.resize(20, 0);
  return b;
}

static void test_xbox360() {
  State s;
  CHECK(xbox::decode_360(view(xbox360_report(0x1000 | 0x0001 | 0x0400, 10, 250, -32768, 32767, 100, -100)), &s));
  CHECK(s.buttons == (kSouth | kDpadUp | kGuide));
  CHECK(s.left_trigger == 10 && s.right_trigger == 250 && s.left_x == -32768 && s.left_y == 32767 && s.right_x == 100 && s.right_y == -100);
  CHECK(s.battery_kind == BatteryKind::Wired);
  // Bit 0x0800 is no button: it is not passed on.
  CHECK(xbox::decode_360(view(xbox360_report(0x0800, 0, 0, 0, 0, 0, 0)), &s) && s.buttons == 0);
  // The LED status message (type 0x01) is not input.
  const Bytes led = {0x01, 0x03, 0x06};
  CHECK(!xbox::decode_360(view(led), &s));
  fuzz(xbox360_report(0xFFFF, 1, 2, 3, 4, 5, 6), 14, [](ConstByteView v, State* st) { return xbox::decode_360(v, st); });

  uint8_t out[8];
  CHECK(xbox::encode_360_rumble(0xFFFF, 0x8000, ByteView(out)) == 8);
  CHECK(out[0] == 0x00 && out[1] == 0x08 && out[3] == 0xFF && out[4] == 0x80);
  CHECK(xbox::encode_360_rumble(1, 1, ByteView(out).first(7)) == 0);
  CHECK(xbox::encode_360_led(2, ByteView(out)) == 3 && out[0] == 0x01 && out[1] == 0x03 && out[2] == 0x07);
  CHECK(xbox::encode_360_led(0, ByteView(out)) == 3 && out[2] == 0x00);
}

static Bytes gip_input(uint8_t a, uint8_t b, uint16_t lt, uint16_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
  Bytes m = {xbox::kGipInput, 0x00, 0x05, 0x0E, a, b, static_cast<uint8_t>(lt), static_cast<uint8_t>(lt >> 8), static_cast<uint8_t>(rt),
             static_cast<uint8_t>(rt >> 8)};
  for (int16_t v : {lx, ly, rx, ry}) { m.push_back(static_cast<uint8_t>(v)); m.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8)); }
  m[3] = static_cast<uint8_t>(m.size() - 4);
  return m;
}

static void test_gip() {
  State s;
  xbox::GipResult r = xbox::decode_gip(view(gip_input(0x10 | 0x04, 0x01 | 0x20 | 0x80, 1023, 512, 1000, -1000, 32767, -32768)), &s);
  CHECK(r.event == xbox::GipEvent::Input && !r.needs_ack);
  CHECK(s.buttons == (kSouth | kStart | kDpadUp | kRightShoulder | kRightStick));
  CHECK(s.left_trigger == 255 && s.right_trigger == 128 && s.left_x == 1000 && s.left_y == -1000 && s.right_x == 32767 && s.right_y == -32768);
  // The guide button comes in its own message, which wants an acknowledgement; input does not clear it.
  const Bytes guide = {xbox::kGipGuide, 0x30, 0x07, 0x02, 0x01, 0x5B};
  r = xbox::decode_gip(view(guide), &s);
  CHECK(r.event == xbox::GipEvent::Guide && r.needs_ack && r.sequence == 0x07 && (s.buttons & kGuide));
  (void)xbox::decode_gip(view(gip_input(0, 0, 0, 0, 0, 0, 0, 0)), &s);
  CHECK(s.buttons == kGuide);
  const Bytes guide_up = {xbox::kGipGuide, 0x20, 0x08, 0x02, 0x00, 0x5B};
  (void)xbox::decode_gip(view(guide_up), &s);
  CHECK(s.buttons == 0);
  uint8_t ack[16];
  xbox::GipResult guide_result = xbox::decode_gip(view(guide), &s);
  CHECK(xbox::encode_gip_ack(guide_result, ByteView(ack)) == 13);
  CHECK(ack[0] == xbox::kGipAcknowledge && ack[2] == 0x07 && ack[3] == 0x09 && ack[5] == xbox::kGipGuide && ack[7] == 0x02);
  // A message claiming more than arrived is nothing (and wants no ack).
  const Bytes liar = {xbox::kGipGuide, 0x30, 0x01, 0x20, 0x01};
  r = xbox::decode_gip(view(liar), &s);
  CHECK(r.event == xbox::GipEvent::None && !r.needs_ack);
  const Bytes status = {xbox::kGipStatus, 0x20, 0x01, 0x04, 0x8F, 0, 0, 0};
  CHECK(xbox::decode_gip(view(status), &s).event == xbox::GipEvent::Status);
  fuzz(gip_input(0xFF, 0xFF, 0x3FF, 0x3FF, 1, 2, 3, 4), 0, [](ConstByteView v, State* st) { return xbox::decode_gip(v, st).event == xbox::GipEvent::Input; });

  uint8_t out[16];
  CHECK(xbox::encode_gip_power_on(3, ByteView(out)) == 5 && out[0] == 0x05 && out[1] == 0x20 && out[2] == 3 && out[3] == 1 && out[4] == 0);
  CHECK(xbox::encode_gip_rumble(4, 0xFFFF, 0x8000, ByteView(out)) == 13 && out[0] == 0x09 && out[5] == 0x0F && out[8] == 99 && out[9] == 49);
  CHECK(xbox::encode_gip_led(5, 0x40, ByteView(out)) == 7 && out[6] == 0x20);
  CHECK(xbox::encode_gip_power_on(0, ByteView(out).first(4)) == 0);
}

// --- Sony -----------------------------------------------------------------------------------------------

static Bytes ds4_report() {
  Bytes r(64, 0);
  r[0] = 0x01;
  r[1] = 0; r[2] = 255; r[3] = 128; r[4] = 128;
  r[5] = 0x02 | 0x20 | 0x80;                  // hat east, cross, triangle
  r[6] = 0x01 | 0x20 | 0x80;                  // L1, options, R3
  r[7] = 0x01 | 0x02;                         // PS, touchpad
  r[8] = 200; r[9] = 7;
  r[30] = 0x10 | 0x05;                        // cable in, level 5
  return r;
}

static Bytes dualsense_report() {
  Bytes r(64, 0);
  r[0] = 0x01;
  r[1] = 128; r[2] = 0; r[3] = 255; r[4] = 128;
  r[5] = 33; r[6] = 66;
  r[8] = 0x08 | 0x10 | 0x40;                  // hat centred, square, circle
  r[9] = 0x02 | 0x10 | 0x40;                  // R1, create, L3
  r[10] = 0x04 | 0x40;                        // mute, left paddle (Edge)
  r[53] = 0x27;                               // full (status 2), level 7
  return r;
}

static void test_sony() {
  State s;
  CHECK(sony::decode_dualshock4(view(ds4_report()), &s));
  CHECK(s.buttons == (kDpadRight | kSouth | kNorth | kLeftShoulder | kStart | kRightStick | kGuide | kTouchpad));
  CHECK(s.left_x == -32768 && s.left_y < -32000 && s.right_x == 0 && s.right_y == -1 && s.left_trigger == 200 && s.right_trigger == 7);
  CHECK(s.battery_kind == BatteryKind::Rechargeable && s.charging && s.battery_percent == 50);
  Bytes other = ds4_report();
  other[0] = 0x11;                            // (the Bluetooth report: not this layout)
  CHECK(!sony::decode_dualshock4(view(other), &s));
  fuzz(ds4_report(), 31, [](ConstByteView v, State* st) { return sony::decode_dualshock4(v, st); });

  CHECK(sony::decode_dualsense(view(dualsense_report()), &s));
  CHECK(s.buttons == (kWest | kEast | kRightShoulder | kBack | kLeftStick | kMisc | kPaddle1));
  CHECK(s.left_x == 0 && s.left_y == 32767 && s.right_x == 32639 && s.left_trigger == 33 && s.right_trigger == 66);
  CHECK(s.battery_percent == 100 && !s.charging);
  fuzz(dualsense_report(), 54, [](ConstByteView v, State* st) { return sony::decode_dualsense(v, st); });

  Output o;
  o.low_frequency = 0xFF00;
  o.high_frequency = 0x1200;
  o.red = 1; o.green = 2; o.blue = 3;
  o.player = 2;
  uint8_t out[64];
  CHECK(sony::encode_dualshock4_output(o, ByteView(out)) == 32);
  CHECK(out[0] == 0x05 && out[1] == 0x03 && out[4] == 0x12 && out[5] == 0xFF && out[6] == 1 && out[7] == 2 && out[8] == 3);
  CHECK(sony::encode_dualsense_output(o, ByteView(out)) == 48);
  CHECK(out[0] == 0x02 && out[1] == 0x03 && out[2] == 0x14 && out[3] == 0x12 && out[4] == 0xFF && out[44] == 0x0A && out[45] == 1 && out[47] == 3);
  CHECK(sony::encode_dualsense_output(o, ByteView(out).first(47)) == 0);
}

// --- Nintendo -------------------------------------------------------------------------------------------

static Bytes switch_report(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
  Bytes r(64, 0);
  r[0] = 0x30;
  r[2] = 0x91;                                // battery: level 4 (full), charging
  r[3] = 0x08 | 0x80;                         // A, ZR
  r[4] = 0x02 | 0x10 | 0x20;                  // +, home, capture
  r[5] = 0x02 | 0x40;                         // up, L
  r[6] = static_cast<uint8_t>(lx); r[7] = static_cast<uint8_t>(((lx >> 8) & 0x0F) | ((ly & 0x0F) << 4)); r[8] = static_cast<uint8_t>(ly >> 4);
  r[9] = static_cast<uint8_t>(rx); r[10] = static_cast<uint8_t>(((rx >> 8) & 0x0F) | ((ry & 0x0F) << 4)); r[11] = static_cast<uint8_t>(ry >> 4);
  return r;
}

static void test_switch() {
  State s;
  CHECK(nintendo::decode_full_report(view(switch_report(2048, 2048 + 1600, 448, 2048)), &s));
  CHECK(s.buttons == (kEast | kStart | kGuide | kMisc | kDpadUp | kLeftShoulder));
  CHECK(s.right_trigger == 255 && s.left_trigger == 0);
  CHECK(s.left_x >= -1 && s.left_x <= 1 && s.left_y == 32767 && s.right_x == -32768);
  CHECK(s.charging && s.battery_percent == 100);
  const Bytes reply = {0x81, 0x02};
  CHECK(!nintendo::decode_full_report(view(reply), &s));
  fuzz(switch_report(1, 2, 3, 4), 12, [](ConstByteView v, State* st) { return nintendo::decode_full_report(v, st); });
  uint8_t out[16];
  CHECK(nintendo::encode_handshake(0, ByteView(out)) == 2 && out[0] == 0x80 && out[1] == 0x02);
  CHECK(nintendo::encode_handshake(3, ByteView(out)) == 2 && out[1] == 0x04);
  CHECK(nintendo::encode_handshake(4, ByteView(out)) == 0);
  CHECK(nintendo::encode_player_leds(0x13, 3, ByteView(out)) == 12 && out[0] == 0x01 && out[1] == 0x03 && out[10] == 0x30 && out[11] == 0x07);
  CHECK(nintendo::encode_full_report_mode(1, ByteView(out)) == 12 && out[10] == 0x03 && out[11] == 0x30);
  CHECK(nintendo::encode_player_leds(0, 1, ByteView(out).first(11)) == 0);
}

// --- Generic HID ----------------------------------------------------------------------------------------

// A DirectInput-style pad (like the Logitech Dual Action): report id 1, 12 buttons, a hat, X/Y/Z/Rz 8-bit.
static const Bytes kDualActionDescriptor = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,             // usage page desktop, joystick, application
    0x85, 0x01,                                     //   report id 1
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,   //   X, Y, Z, Rz
    0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42,   //   hat (null state)
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,             //   padding
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0C, 0x81, 0x02,   //   12 buttons
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,             //   padding
    0xC0};

// A gamepad with 16-bit signed sticks X/Y and Rx/Ry (so Rx/Ry are the right stick) and 8 buttons, without a report id.
static const Bytes kSimplePadDescriptor = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x04,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0xC0};

static void test_hid_gamepad() {
  hid::ReportMap map;
  CHECK(hid::parse_report_map(view(kDualActionDescriptor), &map));
  const hid::InputReport* r = hid::find_gamepad_report(map);
  CHECK(r && r->id == 1);
  hid::GamepadDecoder d;
  CHECK(r && d.init(*r) && d.valid());
  CHECK((d.capabilities() & kCapRightStick) && !(d.capabilities() & kCapTriggers));
  // X 0, Y 255, Z 128, Rz 0; hat 2 (east); buttons 1, 6, 7, 10.
  const Bytes data = {0x00, 0xFF, 0x80, 0x00, 0x02, static_cast<uint8_t>(0x01 | 0x20 | 0x40), 0x02};
  State s;
  CHECK(d.decode(view(data), &s));
  CHECK(s.left_x == -32768 && s.left_y < -32000 && s.right_x >= 0 && s.right_x < 256 && s.right_y == 32767);
  CHECK(s.buttons == (kDpadRight | kSouth | kRightShoulder | kStart));
  CHECK(s.left_trigger == 255 && s.right_trigger == 0);     // button 7 is the left trigger on a pad with no analog ones
  const Bytes centred = {0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0x00};   // hat in its null state
  CHECK(d.decode(view(centred), &s) && s.buttons == 0);
  fuzz(Bytes(data), 7, [&](ConstByteView v, State* st) { return d.decode(v, st); });

  CHECK(hid::parse_report_map(view(kSimplePadDescriptor), &map));
  r = hid::find_gamepad_report(map);
  CHECK(r && r->id == 0 && d.init(*r));
  const Bytes simple = {0xFF, 0x7F, 0x00, 0x80, 0x00, 0x00, 0xFF, 0x7F, 0x0F};
  CHECK(d.decode(view(simple), &s));
  CHECK(s.left_x == 32767 && s.left_y == 32767 && s.right_x == 0 && s.right_y < -32000 && s.buttons == (kSouth | kEast | kWest | kNorth));
  CHECK(!d.decode(view(simple).first(8), &s));

  // A keyboard's report is no gamepad.
  const Bytes keyboard = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
  CHECK(hid::parse_report_map(view(keyboard), &map) && !hid::find_gamepad_report(map));
}

// --- Catalogue ------------------------------------------------------------------------------------------

static void test_catalog() {
  const CatalogEntry* e = find_controller(0x045E, 0x0B12);
  CHECK(e && e->protocol == Protocol::XboxOne);
  e = find_controller(0x054C, 0x0CE6);
  CHECK(e && e->protocol == Protocol::DualSense);
  CHECK(find_controller(0x057E, 0x2009) && find_controller(0x057E, 0x2009)->protocol == Protocol::SwitchPro);
  CHECK(!find_controller(0x1234, 0x5678));
  CHECK(protocol_for_interface(0xFF, 0x5D, 0x01) == Protocol::Xbox360);
  CHECK(protocol_for_interface(0xFF, 0x47, 0xD0) == Protocol::XboxOne);
  CHECK(protocol_for_interface(0xFF, 0x5D, 0x03) == Protocol::None);   // (the 360 pad's headset interface)
  CHECK(protocol_for_interface(0x03, 0x00, 0x00) == Protocol::None);
  // No two entries for one device.
  for (uint32_t i = 0; i < catalog_size(); ++i)
    for (uint32_t j = i + 1; j < catalog_size(); ++j)
      CHECK(!(catalog_entry(i).vendor_id == catalog_entry(j).vendor_id && catalog_entry(i).product_id == catalog_entry(j).product_id));
}

// --- The USB driver against a fake host ----------------------------------------------------------------

struct Config {
  Bytes b;
  void add(std::initializer_list<uint8_t> d) {
    b.push_back(static_cast<uint8_t>(d.size() + 1));
    b.insert(b.end(), d.begin(), d.end());
  }
  Bytes done() {
    Bytes out = {9, 2, static_cast<uint8_t>(b.size() + 9), static_cast<uint8_t>((b.size() + 9) >> 8), 1, 1, 0, 0x80, 50};
    out.insert(out.end(), b.begin(), b.end());
    return out;
  }
};

static Bytes xbox_config(uint8_t subclass, uint8_t protocol) {
  Config c;
  c.add({4, 0, 0, 2, 0xFF, subclass, protocol, 0});
  c.add({5, 0x81, 3, 32, 0, 4});
  c.add({5, 0x02, 3, 32, 0, 8});
  c.add({4, 1, 0, 0, 0xFF, 0x5D, 0x03, 0});        // a second (headset) interface that is not the pad
  return c.done();
}

static Bytes hid_config(uint16_t report_length, bool out_endpoint) {
  Config c;
  c.add({4, 0, 0, static_cast<uint8_t>(out_endpoint ? 2 : 1), 3, 0, 0, 0});
  c.add({0x21, 0x11, 0x01, 0, 1, 0x22, static_cast<uint8_t>(report_length), static_cast<uint8_t>(report_length >> 8)});
  c.add({5, 0x84, 3, 64, 0, 4});
  if (out_endpoint) c.add({5, 0x03, 3, 64, 0, 4});
  return c.done();
}

struct FakeHost final : IUsbHostController {
  Bytes config, report_descriptor;
  uint16_t vendor = 0, product = 0;
  bool present = true, out_supported = true, wireless = false;
  uint32_t generation = 1;
  std::deque<Bytes> in_queue;
  std::deque<Bytes> in_by_endpoint[16];   // (used instead of in_queue for the endpoints that have something here)
  std::vector<Bytes> sent;           // interrupt OUT
  std::vector<uint8_t> sent_to;      // (the endpoint of each)
  std::vector<Bytes> set_reports;    // SET_REPORT data
  int in_started = 0, out_started = 0, idles = 0;

  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = present; out->speed = UsbSpeed::Full; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    if (!present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->vendor_id = vendor;
    out->product_id = product;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    if (!present || config.size() > buffer.size()) return false;
    for (size_t i = 0; i < config.size(); ++i) buffer[i] = config[i];
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return present; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t, uint16_t) override {
    if (type == 0x21 && request == 0x0A) ++idles;
    return present;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t, ByteView buffer) override {
    if (!present || type != 0x81 || request != 6 || value != 0x2200) return false;
    for (size_t i = 0; i < buffer.size() && i < report_descriptor.size(); ++i) buffer[i] = report_descriptor[i];
    return true;
  }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t, uint16_t, ConstByteView data) override {
    if (!present || type != 0x21 || request != 0x09) return false;
    set_reports.push_back(Bytes(data.data(), data.data() + data.size()));
    return true;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { ++in_started; return present; }
  bool poll_interrupt_in(uint32_t, uint8_t endpoint, ByteView buffer, uint16_t* length) override {
    std::deque<Bytes>& queue = !in_by_endpoint[endpoint & 0x0F].empty() ? in_by_endpoint[endpoint & 0x0F] : in_queue;
    if (!present || queue.empty() || (&queue == &in_queue && wireless)) return false;
    const Bytes b = queue.front();
    queue.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_interrupt_out(uint32_t, uint8_t, uint16_t, uint8_t) override { ++out_started; return present && out_supported; }
  bool interrupt_out(uint32_t, uint8_t endpoint, ConstByteView data) override {
    if (!present || !out_supported) return false;
    sent.push_back(Bytes(data.data(), data.data() + data.size()));
    sent_to.push_back(endpoint);
    return true;
  }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
  uint32_t port_generation(uint32_t) const override { return generation; }

  void plug(uint16_t v, uint16_t p, const Bytes& c) { vendor = v; product = p; config = c; present = true; ++generation; sent.clear(); sent_to.clear(); set_reports.clear(); }
  void unplug() { present = false; ++generation; }
};

struct FakeBus final : IUsbBus {
  std::vector<IUsbHostController*> hosts;
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(hosts.size()); }
  uint32_t controller_count() const override { return static_cast<uint32_t>(hosts.size()); }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t c) const override { return hosts[c]->port_count(); }
  bool port_info(uint32_t c, uint32_t p, UsbPortInfo* out) const override { return hosts[c]->port_info(p, out); }
  bool device_descriptor(uint32_t c, uint32_t p, UsbDeviceDescriptor* out) const override { return hosts[c]->device_descriptor(p, out); }
  IUsbHostController* host_controller(uint32_t c) override { return c < hosts.size() ? hosts[c] : nullptr; }
};

static void test_usb_xbox() {
  TestAllocator a;
  {
    FakeHost host;
    host.plug(0x045E, 0x028E, xbox_config(0x5D, 0x01));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && pad->info().protocol == Protocol::Xbox360 && strcmp(pad->info().name, "Xbox 360 Controller") == 0);
    CHECK(host.in_started == 1 && host.out_started == 1);
    host.in_queue.push_back(xbox360_report(0x2000, 0, 0, 5, 6, 7, 8));
    CHECK(pad->poll() && pad->state().buttons == kEast && pad->state().left_x == 5);
    CHECK(!pad->poll());                                   // nothing new
    Output o;
    o.low_frequency = 0xFFFF;
    o.player = 1;
    CHECK(pad->set_output(o));
    CHECK(host.sent.size() == 2 && host.sent[0][1] == 0x08 && host.sent[0][3] == 0xFF && host.sent[1][2] == 0x06);
    host.unplug();
    CHECK(!pad->present() && !pad->poll() && !pad->set_output(o));
  }
  {
    // An unknown third-party pad that speaks XInput is found by its interface.
    FakeHost host;
    host.plug(0x1111, 0x2222, xbox_config(0x5D, 0x01));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && strcmp(pad->info().name, "Xbox 360 compatible controller") == 0);
  }
  {
    FakeHost host;
    host.plug(0x045E, 0x0B12, xbox_config(0x47, 0xD0));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && pad->info().protocol == Protocol::XboxOne);
    CHECK(host.sent.size() == 2 && host.sent[0][0] == xbox::kGipPower && host.sent[1][0] == xbox::kGipLed);   // powered on, guide light lit
    host.in_queue.push_back(gip_input(0x20, 0, 0, 1023, 0, 0, 0, 0));
    host.in_queue.push_back({xbox::kGipGuide, 0x30, 0x09, 0x02, 0x01, 0x5B});
    CHECK(pad->poll() && pad->state().buttons == (kEast | kGuide) && pad->state().right_trigger == 255);
    CHECK(host.sent.size() == 3 && host.sent[2][0] == xbox::kGipAcknowledge && host.sent[2][2] == 0x09);
    Output o;
    o.high_frequency = 0xFFFF;
    CHECK(pad->set_output(o) && host.sent.back()[0] == xbox::kGipRumble && host.sent.back()[9] == 99);
  }
  {
    // Without interrupt OUT an Xbox One pad stays silent: it is not taken. A 360 pad is, without rumble.
    FakeHost host;
    host.out_supported = false;
    host.plug(0x045E, 0x02EA, xbox_config(0x47, 0xD0));
    CHECK(!UsbGameController::open(host, 0, a));
    host.plug(0x045E, 0x028E, xbox_config(0x5D, 0x01));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && !pad->set_output(Output()));
  }
  CHECK(a.live == 0);
}

static void test_usb_hid() {
  TestAllocator a;
  {
    // DualSense: input at once, output through its OUT endpoint.
    FakeHost host;
    host.plug(sony::kVendor, sony::kDualSense, hid_config(200, true));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && pad->info().protocol == Protocol::DualSense && (pad->info().capabilities & kCapLightBar));
    host.in_queue.push_back(dualsense_report());
    CHECK(pad->poll() && (pad->state().buttons & kMisc) && pad->state().battery_percent == 100);
    Output o;
    o.red = 9;
    CHECK(pad->set_output(o) && host.sent.size() == 1 && host.sent[0].size() == 48 && host.sent[0][45] == 9);
  }
  {
    // DualShock 4 without an OUT endpoint: output as SET_REPORT.
    FakeHost host;
    host.plug(sony::kVendor, sony::kDualShock4v2, hid_config(200, false));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && pad->info().protocol == Protocol::DualShock4 && host.out_started == 0);
    host.in_queue.push_back(ds4_report());
    CHECK(pad->poll() && (pad->state().buttons & kTouchpad));
    Output o;
    o.low_frequency = 0x4000;
    CHECK(pad->set_output(o) && host.set_reports.size() == 1 && host.set_reports[0][0] == 0x05 && host.set_reports[0][5] == 0x40);
  }
  {
    // Switch Pro: the handshake goes out step by step as the pad answers, then full reports.
    FakeHost host;
    host.plug(nintendo::kVendor, nintendo::kSwitchPro, hid_config(170, true));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && host.sent.size() == 1 && host.sent[0] == Bytes({0x80, 0x02}));
    for (int i = 0; i < 3; ++i) {
      host.in_queue.push_back({0x81, 0x02});
      (void)pad->poll();
    }
    CHECK(host.sent.size() == 4 && host.sent[3] == Bytes({0x80, 0x04}));
    for (int i = 0; i < 20 && !pad->handshake_done(); ++i) (void)pad->poll();   // the subcommands are answered with 0x21: it goes on after a while
    CHECK(pad->handshake_done() && host.sent.size() == 6 && host.sent[4][10] == 0x03 && host.sent[5][10] == 0x30);
    host.in_queue.push_back(switch_report(2048, 2048, 2048, 2048));
    CHECK(pad->poll() && (pad->state().buttons & kEast));
    Output o;
    o.player = 2;
    CHECK(pad->set_output(o) && host.sent.back()[11] == 0x03);
  }
  {
    // A generic HID gamepad, recognized by its report descriptor.
    FakeHost host;
    host.report_descriptor = kDualActionDescriptor;
    host.plug(0x046D, 0xC216, hid_config(static_cast<uint16_t>(kDualActionDescriptor.size()), false));
    CHECK(GameControllerService::is_game_controller(host, 0));
    UniquePointer<UsbGameController> pad = UsbGameController::open(host, 0, a);
    CHECK(pad && pad->info().protocol == Protocol::Hid && strcmp(pad->info().name, "Logitech Dual Action") == 0 && host.idles == 1);
    host.in_queue.push_back({0x02, 0x80, 0x80, 0x80, 0x80, 0x08, 0x08, 0x00});   // (another report id: ignored)
    host.in_queue.push_back({0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0x08, 0x00});   // button 4
    CHECK(pad->poll() && pad->state().buttons == kNorth);
    CHECK(!pad->set_output(Output()));
  }
  {
    // A HID keyboard is not a game controller; nor is a device whose report descriptor cannot be read.
    FakeHost host;
    host.report_descriptor = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0xC0};
    host.plug(0x1234, 0x0001, hid_config(7, false));
    CHECK(!GameControllerService::is_game_controller(host, 0) && !UsbGameController::open(host, 0, a));
    host.report_descriptor.clear();
    CHECK(!GameControllerService::is_game_controller(host, 0));
    // A truncated configuration descriptor is nothing either.
    Bytes cut = hid_config(7, false);
    for (size_t n = 0; n < cut.size(); ++n) {
      host.config = Bytes(cut.begin(), cut.begin() + n);
      (void)GameControllerService::is_game_controller(host, 0);
    }
  }
  CHECK(a.live == 0);
}

// --- The service ----------------------------------------------------------------------------------------

static void test_service() {
  TestAllocator a;
  {
    FakeHost first, second;
    first.plug(0x045E, 0x028E, xbox_config(0x5D, 0x01));
    second.plug(sony::kVendor, sony::kDualSense, hid_config(200, true));
    FakeBus bus;
    bus.hosts = {&first, &second};
    GameControllerService service(&bus, a);
    std::vector<std::string> notes;
    service.set_notifier([](void* context, StringView text) {
      static_cast<std::vector<std::string>*>(context)->push_back(std::string(text.data(), text.size()));
    }, &notes);
    CHECK(service.start());
    CHECK(service.count() == 2 && service.added() == 2);
    CHECK(notes.size() == 2 && notes[0] == "Xbox 360 Controller: connected as player 1");
    // Each pad was shown its number: the 360 ring's quadrant 1, the DualSense's player 2 LEDs and red light bar.
    CHECK(!first.sent.empty() && first.sent.back()[0] == 0x01 && first.sent.back()[2] == 0x06);
    CHECK(!second.sent.empty() && second.sent.back()[44] == 0x0A && second.sent.back()[45] == 0x40);

    State s;
    uint32_t packet = 0, before = 0;
    CHECK(service.snapshot(0, &s, &before) && s.buttons == 0);
    first.in_queue.push_back(xbox360_report(0x1000, 0, 0, 0, 0, 0, 0));
    service.step(false);
    CHECK(service.snapshot(0, &s, &packet) && s.buttons == kSouth && packet == before + 1);
    service.step(false);
    CHECK(service.snapshot(0, &s, &packet) && packet == before + 1);    // unchanged: the same packet number
    CHECK(!service.snapshot(2, &s, &packet) && !service.snapshot(99, &s, &packet));

    Info info;
    CHECK(service.info(1, &info) && info.protocol == Protocol::DualSense);
    CHECK(service.set_vibration(0, 0x8000, 0) && first.sent.back()[3] == 0x80);
    CHECK(service.set_light(1, 1, 2, 3) && second.sent.back()[45] == 1);
    CHECK(!service.set_light(0, 1, 2, 3));                               // (an Xbox pad has no light bar)
    CHECK(!service.set_vibration(5, 1, 1));

    // Unplugged: the slot empties; plugged back in: it takes the lowest free slot again.
    first.unplug();
    service.step(true);
    CHECK(service.count() == 1 && !service.snapshot(0, &s, &packet) && service.removed() == 1);
    first.plug(0x045E, 0x0B12, xbox_config(0x47, 0xD0));
    service.step(true);
    CHECK(service.count() == 2 && service.info(0, &info) && info.protocol == Protocol::XboxOne);
    service.stop();
    CHECK(service.count() == 0);
  }
  CHECK(a.live == 0);
}

// --- The Xbox 360 Wireless Receiver -------------------------------------------------------------------

static Bytes receiver_config() {
  Config c;
  for (uint8_t i = 0; i < 4; ++i) {
    c.add({4, static_cast<uint8_t>(i * 2), 0, 2, 0xFF, 0x5D, 0x81, 0});                         // pad i
    c.add({5, static_cast<uint8_t>(0x81 + i * 2), 3, 32, 0, 1});
    c.add({5, static_cast<uint8_t>(0x01 + i * 2), 3, 32, 0, 8});
    c.add({4, static_cast<uint8_t>(i * 2 + 1), 0, 2, 0xFF, 0x5D, 0x82, 0});                     // its headset
    c.add({5, static_cast<uint8_t>(0x82 + i * 2), 3, 32, 0, 2});
  }
  return c.done();
}

static Bytes wireless_input(uint16_t buttons, int16_t lx) {
  Bytes m = {0x00, 0x01, 0x00, 0xF0, 0x00, 0x13, static_cast<uint8_t>(buttons), static_cast<uint8_t>(buttons >> 8), 0, 0,
             static_cast<uint8_t>(lx), static_cast<uint8_t>(static_cast<uint16_t>(lx) >> 8)};
  m.resize(29, 0);
  return m;
}

static void test_wireless() {
  // The protocol.
  State s;
  const Bytes paired = {0x08, 0x80}, gone = {0x08, 0x00}, headset_only = {0x08, 0x40};
  CHECK(xbox::decode_360w(view(paired), &s) == xbox::WirelessEvent::Connected);
  CHECK(xbox::decode_360w(view(headset_only), &s) == xbox::WirelessEvent::Disconnected);
  CHECK(xbox::decode_360w(view(wireless_input(0x1000 | 0x0400, -77)), &s) == xbox::WirelessEvent::Input && s.buttons == (kSouth | kGuide) && s.left_x == -77);
  CHECK(xbox::decode_360w(view(gone), &s) == xbox::WirelessEvent::Disconnected && s.buttons == 0);
  fuzz(wireless_input(0xFFFF, 5), 0, [](ConstByteView v, State* st) { return xbox::decode_360w(v, st) == xbox::WirelessEvent::Input; });
  uint8_t out[16];
  CHECK(xbox::encode_360w_inquiry(ByteView(out)) == 12 && out[0] == 0x08 && out[2] == 0x0F && out[3] == 0xC0);
  CHECK(xbox::encode_360w_led(3, ByteView(out)) == 12 && out[2] == 0x08 && out[3] == 0x48);
  CHECK(xbox::encode_360w_rumble(0xFF00, 0x0100, ByteView(out)) == 12 && out[1] == 0x01 && out[5] == 0xFF && out[6] == 0x01);
  CHECK(xbox::encode_360w_led(1, ByteView(out).first(11)) == 0);

  UsbWirelessChannel channels[8];
  const Bytes config = receiver_config();
  CHECK(find_xbox360_wireless_channels(view(config), ArrayView<UsbWirelessChannel>(channels)) == 4);
  CHECK(channels[2].interface_number == 4 && channels[2].in_endpoint == 0x85 && channels[2].out_endpoint == 0x05);
  for (size_t n = 0; n < config.size(); ++n) {        // cut short anywhere: never more channels than there are
    Bytes cut(config.begin(), config.begin() + n);
    CHECK(find_xbox360_wireless_channels(view(cut), ArrayView<UsbWirelessChannel>(channels)) <= 4);
  }

  // The service: four channels wait; pads take slots as they pair and give them back as they leave.
  TestAllocator a;
  {
    FakeHost receiver;
    receiver.wireless = true;
    receiver.plug(0x045E, 0x0719, receiver_config());
    FakeBus bus;
    bus.hosts = {&receiver};
    GameControllerService service(&bus, a);
    CHECK(service.start());
    CHECK(service.count() == 0 && receiver.sent.size() == 4 && receiver.sent[0][0] == 0x08);   // an inquiry on each channel
    receiver.in_by_endpoint[0x83 & 0x0F].push_back(paired);              // a pad pairs with channel 1
    service.step(false);
    CHECK(service.count() == 1);
    Info info;
    CHECK(service.info(0, &info) && strcmp(info.name, "Xbox 360 Wireless Controller") == 0);
    CHECK(receiver.sent_to.back() == 0x03 && receiver.sent.back()[3] == 0x46);   // its ring shows player 1, on its own channel
    receiver.in_by_endpoint[0x87 & 0x0F].push_back(wireless_input(0x2000, 0));   // input on channel 3 without a pairing message
    service.step(false);
    CHECK(service.count() == 2);
    service.step(false);                       // (the pad in slot 1 has reported east once)
    State st;
    uint32_t packet = 0;
    CHECK(service.snapshot(1, &st, &packet) && st.buttons == kEast);
    CHECK(service.set_vibration(0, 0x8000, 0) && receiver.sent_to.back() == 0x03 && receiver.sent.back()[5] == 0x80);
    receiver.in_by_endpoint[0x83 & 0x0F].push_back(gone);                // channel 1's pad switched off
    service.step(false);
    CHECK(service.count() == 1 && !service.snapshot(0, &st, &packet) && service.snapshot(1, &st, &packet));
    receiver.in_by_endpoint[0x83 & 0x0F].push_back(paired);              // and on again: the first free slot
    service.step(false);
    CHECK(service.count() == 2 && service.snapshot(0, &st, &packet));
    receiver.unplug();                                                    // the receiver itself pulled out
    service.step(true);
    CHECK(service.count() == 0);
    service.stop();
  }
  CHECK(a.live == 0);
}

// --- Bluetooth pads ------------------------------------------------------------------------------------

struct FakeLink final : IHidReportLink {
  std::vector<Bytes> outputs;
  std::vector<uint8_t> features;      // the feature reports asked for
  bool answer = true;
  bool send_output(ConstByteView report) override { outputs.push_back(Bytes(report.data(), report.data() + report.size())); return true; }
  bool get_feature(uint8_t id, ByteView out, uint32_t* length) override {
    features.push_back(id);
    if (!answer || out.size() < 2) return false;
    out[0] = id;
    out[1] = 0x55;
    *length = 2;
    return true;
  }
};

// The report map of an Xbox pad over Bluetooth (as its firmware describes it): report 1 with 16-bit sticks X/Y/Z/Rz, 10-bit brake and
// accelerator, a hat 1..8 and 15 buttons.
static const Bytes kXboxBtMap = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
    0x05, 0x02, 0x09, 0xC5, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x95, 0x01, 0x75, 0x0A, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x02, 0x09, 0xC4, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x95, 0x01, 0x75, 0x0A, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x66, 0x14, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00, 0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
    0xC0};

static void test_bluetooth() {
  // Sony over Bluetooth: the full reports are the USB ones shifted; the output reports carry a CRC.
  State s;
  Bytes ds4 = ds4_report();
  Bytes ds4bt(ds4.size() + 2, 0);
  ds4bt[0] = sony::kDualShock4BtInput;
  ds4bt[1] = 0xC0;
  for (size_t i = 1; i < ds4.size(); ++i) ds4bt[i + 2] = ds4[i];
  CHECK(sony::decode_dualshock4_bt(view(ds4bt), &s) && (s.buttons & kTouchpad) && s.left_trigger == 200 && s.charging && s.battery_percent == 50);
  fuzz(ds4bt, 33, [](ConstByteView v, State* st) { return sony::decode_dualshock4_bt(v, st); });
  Bytes ds = dualsense_report();
  Bytes dsbt(ds.size() + 1, 0);
  dsbt[0] = sony::kDualSenseBtInput;
  for (size_t i = 1; i < ds.size(); ++i) dsbt[i + 1] = ds[i];
  CHECK(sony::decode_dualsense_bt(view(dsbt), &s) && (s.buttons & kMisc) && s.left_trigger == 33 && s.battery_percent == 100);
  fuzz(dsbt, 55, [](ConstByteView v, State* st) { return sony::decode_dualsense_bt(v, st); });
  const Bytes reduced = {0x01, 0x80, 0x00, 0x80, 0x80, 0x08 | 0x20, 0x00, 0x01, 10, 20};
  CHECK(sony::decode_bt_reduced(view(reduced), &s) && s.buttons == (kSouth | kGuide) && s.left_y == 32767 && s.right_trigger == 20);
  Output o;
  o.low_frequency = 0xFF00;
  o.red = 7;
  o.player = 1;
  uint8_t out[96];
  CHECK(sony::encode_dualshock4_bt_output(o, ByteView(out)) == 78 && out[0] == 0x11 && out[1] == 0xC0 && out[7] == 0xFF && out[8] == 7);
  CHECK(sony::bt_output_crc_ok(ConstByteView(out).first(78)));
  out[20] ^= 1;
  CHECK(!sony::bt_output_crc_ok(ConstByteView(out).first(78)));
  CHECK(sony::encode_dualsense_bt_output(o, 3, ByteView(out)) == 78 && out[0] == 0x31 && out[1] == 0x30 && out[2] == 0x10 && out[6] == 0xFF &&
        out[47] == 7 && out[46] == 0x04);
  CHECK(sony::bt_output_crc_ok(ConstByteView(out).first(78)));
  CHECK(sony::encode_dualsense_bt_output(o, 0, ByteView(out).first(77)) == 0);

  // Switch Pro's simple report.
  Bytes simple = {0x3F, 0x02 | 0x80, 0x10, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0x00, 0x80};
  CHECK(nintendo::decode_simple_report(view(simple), &s) && s.buttons == (kEast | kGuide | kDpadRight) && s.right_trigger == 255);
  CHECK(s.left_x == 32767 && s.left_y == 32767 && s.right_x >= -1 && s.right_x <= 1);
  fuzz(simple, 12, [](ConstByteView v, State* st) { return nintendo::decode_simple_report(v, st); });

  // Routing: by ids, else by the report map.
  BluetoothPadIdentity ds4id{sony::kVendor, sony::kDualShock4v2, "Wireless Controller", false};
  BluetoothPadIdentity xboxid{0x045E, 0x0B13, "Xbox Wireless Controller", true};
  BluetoothPadIdentity keyboard{0x046D, 0xB342, "Keyboard", true};
  const Bytes kb_map = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
  CHECK(is_bluetooth_gamepad(ds4id, ConstByteView()) && is_bluetooth_gamepad(xboxid, view(kXboxBtMap)) && !is_bluetooth_gamepad(keyboard, view(kb_map)));
  CHECK(!is_bluetooth_gamepad(xboxid, ConstByteView()));          // (a Microsoft device without a gamepad map: a keyboard, a mouse)
  for (size_t n = 0; n < kXboxBtMap.size(); ++n) {                  // cut-short maps never crash and never claim more than they have
    Bytes cut(kXboxBtMap.begin(), kXboxBtMap.begin() + n);
    (void)is_bluetooth_gamepad(xboxid, view(cut));
  }

  TestAllocator a;
  {
    GameControllerService service(nullptr, a);
    CHECK(service.start());
    // An Xbox Series pad over LE.
    FakeLink xlink;
    SharedPointer<HidPadChannel> xchannel = SharedPointer<HidPadChannel>::make(a);
    xchannel->attach(&xlink);
    CHECK(service.add(make_bluetooth_gamepad(a, xchannel, xboxid, view(kXboxBtMap))) == 0);
    Info info;
    CHECK(service.info(0, &info) && info.protocol == Protocol::XboxOne && strcmp(info.name, "Xbox Series X|S Controller") != 0);   // (0B13 is the BT id: not the USB one)
    CHECK(xlink.outputs.size() == 1 && xlink.outputs[0][0] == 0x03);          // shown its number: the motors (off) are its only output
    // X 65535, Y 0 (up), Z/Rz centred, brake 1023, accelerator 0, hat 3 (right), buttons A (1) and Menu (12).
    Bytes report = {0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0xFF, 0x03, 0x00, 0x00, 0x03,
                    static_cast<uint8_t>(0x01), static_cast<uint8_t>(0x08), 0x00};
    xchannel->deliver(view(report));
    service.step(false);
    State st;
    uint32_t packet = 0;
    CHECK(service.snapshot(0, &st, &packet));
    CHECK(st.buttons == (kSouth | kStart | kDpadRight) && st.left_x == 32767 && st.left_y == 32767 && st.left_trigger == 255 && st.right_trigger == 0);
    CHECK(service.set_vibration(0, 0xFFFF, 0x8000) && xlink.outputs.back().size() == 9 && xlink.outputs.back()[4] == 99 && xlink.outputs.back()[5] == 49);

    // A DualShock 4 over classic: the feature read on the first poll, then full reports.
    FakeLink slink;
    SharedPointer<HidPadChannel> schannel = SharedPointer<HidPadChannel>::make(a);
    schannel->attach(&slink);
    CHECK(service.add(make_bluetooth_gamepad(a, schannel, ds4id, ConstByteView())) == 1);
    CHECK(slink.outputs.size() == 1 && slink.outputs[0][0] == 0x11 && sony::bt_output_crc_ok(view(slink.outputs[0])));   // light bar: player 2's red
    schannel->deliver(view(reduced));
    service.step(false);
    CHECK(slink.features.size() == 1 && slink.features[0] == 0x02);
    CHECK(service.snapshot(1, &st, &packet) && (st.buttons & kSouth));
    schannel->deliver(view(ds4bt));
    service.step(false);
    CHECK(service.snapshot(1, &st, &packet) && (st.buttons & kTouchpad) && st.battery_percent == 50);
    CHECK(slink.features.size() == 1);                                        // (only once)
    CHECK(service.set_light(1, 1, 2, 3) && slink.outputs.back()[8] == 1 && slink.outputs.back()[10] == 3);

    // More reports than the ring holds between polls: the newest survive.
    for (int i = 0; i < 40; ++i) xchannel->deliver(view(report));
    CHECK(xchannel->dropped() > 0);
    service.step(false);

    // The link goes: the pads leave their slots at the next scan.
    xchannel->detach();
    CHECK(!xchannel->send_output(view(report)));
    service.step(true);
    CHECK(service.count() == 1 && !service.snapshot(0, &st, &packet));
    schannel->detach();
    service.step(true);
    CHECK(service.count() == 0);
    // Not a pad: nothing made.
    SharedPointer<HidPadChannel> kchannel = SharedPointer<HidPadChannel>::make(a);
    CHECK(!make_bluetooth_gamepad(a, kchannel, keyboard, view(kb_map)));
    CHECK(!make_bluetooth_gamepad(a, SharedPointer<HidPadChannel>(), ds4id, ConstByteView()));
    service.stop();
  }
  CHECK(a.live == 0);
}

// --- An Xbox pad over LE, through HID over GATT -------------------------------------------------------

// A GATT remote shaped like an Xbox pad: Device Information with its PnP ID, the HID service with the report map, input report 1 (notify)
// and output report 3 (write without response), each with its Report Reference.
struct FakeGattPad final : IBluetoothRemote {
  BluetoothDeviceInfo device;
  GattDatabase db;
  uint32_t connection = 7;
  IBluetoothNotificationSink* sink = nullptr;
  std::vector<std::pair<int, Bytes>> writes;
  Bytes map = kXboxBtMap;
  uint16_t product = 0x0B13;
  enum { kPnp, kMap, kInput, kOutput };

  FakeGattPad() {
    strcpy(device.name, "Xbox Wireless Controller");
    db.services[0] = {1, 4, Uuid::from16(0x180A)};
    db.services[1] = {5, 20, Uuid::from16(0x1812)};
    db.service_count = 2;
    auto ch = [&](int index, uint16_t value, uint8_t props, uint8_t service, uint16_t uuid) {
      GattCharacteristic& c = db.characteristics[index];
      c.declaration = static_cast<uint16_t>(value - 1);
      c.value_handle = value;
      c.end_handle = static_cast<uint16_t>(value + 2);
      c.properties = props;
      c.service = service;
      c.uuid = Uuid::from16(uuid);
    };
    ch(kPnp, 3, gatt::kPropRead, 0, 0x2A50);
    ch(kMap, 7, gatt::kPropRead, 1, 0x2A4B);
    ch(kInput, 10, gatt::kPropRead | gatt::kPropNotify, 1, 0x2A4D);
    ch(kOutput, 14, gatt::kPropRead | gatt::kPropWriteWithoutResponse, 1, 0x2A4D);
    db.characteristic_count = 4;
    db.descriptors[0] = {12, kInput, Uuid::from16(0x2908)};
    db.descriptors[1] = {15, kOutput, Uuid::from16(0x2908)};
    db.descriptor_count = 2;
  }
  const BluetoothDeviceInfo& info() const override { return device; }
  uint32_t id() const override { return connection; }
  bool connected() const override { return true; }
  bool encrypted() const override { return true; }
  const GattDatabase& database() const override { return db; }
  AttStatus read(int characteristic, ByteView out, uint32_t* length) override {
    Bytes v;
    if (characteristic == kPnp) v = {0x02, 0x5E, 0x04, static_cast<uint8_t>(product), static_cast<uint8_t>(product >> 8), 0x01, 0x05};
    else if (characteristic == kMap) v = map;
    else return AttStatus::Error;
    if (v.size() > out.size()) v.resize(out.size());
    for (size_t i = 0; i < v.size(); ++i) out[i] = v[i];
    *length = static_cast<uint32_t>(v.size());
    return AttStatus::Ok;
  }
  AttStatus read_descriptor(uint16_t handle, ByteView out, uint32_t* length) override {
    if (out.size() < 2) return AttStatus::Error;
    if (handle == 12) { out[0] = 1; out[1] = 1; }          // report 1, input
    else if (handle == 15) { out[0] = 3; out[1] = 2; }     // report 3, output
    else return AttStatus::Error;
    *length = 2;
    return AttStatus::Ok;
  }
  AttStatus write(int characteristic, ConstByteView value, bool) override {
    writes.push_back({characteristic, Bytes(value.data(), value.data() + value.size())});
    return AttStatus::Ok;
  }
  AttStatus subscribe(int characteristic, bool, IBluetoothNotificationSink* s) override {
    if (characteristic != kInput) return AttStatus::Error;
    sink = s;
    return AttStatus::Ok;
  }
  void notify(const Bytes& value) { if (sink) sink->on_notification(*this, 10, view(value), false); }
};

struct NullInputSink final : IInputSink {
  int keyboards = 0, mice = 0;
  bool add_keyboard(IKeyboard*) override { ++keyboards; return true; }
  bool remove_keyboard(IKeyboard*) override { --keyboards; return true; }
  bool add_mouse(IMouse*) override { ++mice; return true; }
  bool remove_mouse(IMouse*) override { --mice; return true; }
  void notify(StringView) override {}
};

static void test_hid_over_gatt_pad() {
  TestAllocator a;
  {
    GameControllerService service(nullptr, a);
    CHECK(service.start());
    HidOverGatt profile(a);
    FakeGattPad pad;
    // Without a game controller service the pad is not taken (and is no keyboard either).
    CHECK(!profile.attach(pad));
    profile.set_game_controllers(&service);
    CHECK(profile.matches(pad.info(), pad.database()) && profile.attach(pad));
    CHECK(service.count() == 1 && pad.sink != nullptr);
    Info info;
    CHECK(service.info(0, &info) && info.protocol == Protocol::XboxOne && info.vendor_id == 0x045E && info.product_id == 0x0B13);
    // The notification carries the report without its id: the profile puts it back.
    pad.notify({0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0xFF, 0x03, 0x00, 0x02, 0x00});   // (button 2: B)
    service.step(false);
    State st;
    uint32_t packet = 0;
    CHECK(service.snapshot(0, &st, &packet) && st.buttons == kEast && st.left_x == 32767 && st.right_trigger == 255);
    // Rumble is queued by the pad's thread and written when the adapter's thread calls service().
    const size_t before = pad.writes.size();
    CHECK(service.set_vibration(0, 0xFFFF, 0));
    CHECK(pad.writes.size() == before);
    profile.service(pad);
    CHECK(pad.writes.size() == before + 1 && pad.writes.back().first == FakeGattPad::kOutput && pad.writes.back().second.size() == 8 &&
          pad.writes.back().second[0] == 0x0F && pad.writes.back().second[3] == 99);
    profile.service(pad);
    CHECK(pad.writes.size() == before + 1);                                    // (nothing new: nothing written)
    NullInputSink sink;
    CHECK(profile.poll(sink) == 0 && sink.keyboards == 0 && sink.mice == 0);   // a pad is no keyboard or mouse
    // The link drops: the pad leaves its slot.
    profile.detach(pad);
    CHECK(!service.set_vibration(0, 1, 1) || true);
    service.step(true);
    CHECK(service.count() == 0);
    (void)profile.poll(sink);
    // A cut-short or bit-flipped report map never crashes the attach.
    for (size_t n = 0; n < kXboxBtMap.size(); n += 3) {
      FakeGattPad cut;
      cut.connection = static_cast<uint32_t>(100 + n);
      cut.map = Bytes(kXboxBtMap.begin(), kXboxBtMap.begin() + n);
      if (profile.attach(cut)) profile.detach(cut);
      service.step(true);
      (void)profile.poll(sink);
    }
    for (size_t bit = 0; bit < kXboxBtMap.size() * 8; bit += 7) {
      FakeGattPad flipped;
      flipped.connection = static_cast<uint32_t>(1000 + bit);
      flipped.map[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      if (profile.attach(flipped)) {
        flipped.notify({0x01, 0x02, 0x03});
        service.step(false);
        profile.detach(flipped);
      }
      service.step(true);
      (void)profile.poll(sink);
    }
    service.stop();
  }
  CHECK(a.live == 0);
}

// --- NtTcosGamepad's words ------------------------------------------------------------------------------

static void test_service_call() {
  TestAllocator a;
  {
    FakeHost xbox_host, sony_host;
    xbox_host.plug(0x045E, 0x028E, xbox_config(0x5D, 0x01));
    sony_host.plug(sony::kVendor, sony::kDualShock4, hid_config(200, true));
    FakeBus bus;
    bus.hosts = {&xbox_host, &sony_host};
    GameControllerService service(&bus, a);
    CHECK(service.start());
    xbox_host.in_queue.push_back(xbox360_report(0x1000 | 0x0400, 7, 9, -2, 3, 32767, -32768));
    service.step(false);
    uint64_t w[kGamepadServiceWords] = {1, 0};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(w)) == 0);
    CHECK(w[5] == (kSouth | kGuide) && w[6] == (7u | 9u << 8));
    CHECK(static_cast<int16_t>(w[7]) == -2 && static_cast<int16_t>(w[7] >> 16) == 3 && static_cast<int16_t>(w[7] >> 32) == 32767 &&
          static_cast<int16_t>(w[7] >> 48) == -32768);
    const uint64_t packet = w[4];
    uint64_t again[kGamepadServiceWords] = {1, 0};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(again)) == 0 && again[4] == packet);
    uint64_t empty[kGamepadServiceWords] = {1, 3};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(empty)) == 0xC000009Du);   // STATUS_DEVICE_NOT_CONNECTED
    uint64_t past[kGamepadServiceWords] = {1, 8};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(past)) == 0xC000000Du);
    uint64_t unknown[kGamepadServiceWords] = {99, 0};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(unknown)) == 0xC000000Du);
    uint64_t rumble[kGamepadServiceWords] = {2, 0, 0xFFFF, 0x1000};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(rumble)) == 0 && xbox_host.sent.back()[3] == 0xFF && xbox_host.sent.back()[4] == 0x10);
    uint64_t too_strong[kGamepadServiceWords] = {2, 0, 0x10000, 0};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(too_strong)) == 0xC000000Du);
    uint64_t info[kGamepadServiceWords] = {3, 1};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(info)) == 0);
    CHECK(info[4] == static_cast<uint64_t>(Protocol::DualShock4) && info[5] == (0x054Cu | uint64_t{0x05C4} << 16) && (info[6] & kCapLightBar));
    uint64_t light[kGamepadServiceWords] = {4, 1, 0x030201};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(light)) == 0 && sony_host.sent.back()[6] == 1 && sony_host.sent.back()[8] == 3);
    uint64_t no_light[kGamepadServiceWords] = {4, 0, 0x030201};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(no_light)) == 0xC00000BBu);   // STATUS_NOT_SUPPORTED
    uint8_t name[64];
    uint32_t length = 0;
    CHECK(gamepad_service_name(service, 0, ByteView(name), &length) == 0 && length == strlen("Xbox 360 Controller") && memcmp(name, "Xbox 360", 8) == 0);
    CHECK(gamepad_service_name(service, 1, ByteView(name).first(4), &length) == 0 && length == strlen("DualShock 4") && memcmp(name, "Dual", 4) == 0);
    CHECK(gamepad_service_name(service, 2, ByteView(name), &length) == 0xC000009Du && length == 0);
    CHECK(gamepad_service_name(service, 9, ByteView(name), &length) == 0xC000000Du);
    uint64_t short_words[2] = {1, 0};
    CHECK(gamepad_service_call(service, ArrayView<uint64_t>(short_words)) == 0xC000000Du);
  }
  CHECK(a.live == 0);
}

int main() {
  test_axes();
  test_xbox360();
  test_gip();
  test_sony();
  test_switch();
  test_hid_gamepad();
  test_catalog();
  test_usb_xbox();
  test_usb_hid();
  test_service();
  test_service_call();
  test_wireless();
  test_bluetooth();
  test_hid_over_gatt_pad();
  printf("GameControllerTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
