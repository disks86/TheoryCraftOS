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
#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/GameController/HidGamepad.hpp"
#include "Hardware/GameController/SonyProtocol.hpp"
#include "Hardware/GameController/SwitchProtocol.hpp"
#include "Hardware/GameController/Usb/UsbGameController.hpp"
#include "Hardware/GameController/WindowsGamepadService.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

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
  bool present = true, out_supported = true;
  uint32_t generation = 1;
  std::deque<Bytes> in_queue;
  std::vector<Bytes> sent;           // interrupt OUT
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
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView buffer, uint16_t* length) override {
    if (!present || in_queue.empty()) return false;
    const Bytes b = in_queue.front();
    in_queue.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_interrupt_out(uint32_t, uint8_t, uint16_t, uint8_t) override { ++out_started; return present && out_supported; }
  bool interrupt_out(uint32_t, uint8_t, ConstByteView data) override {
    if (!present || !out_supported) return false;
    sent.push_back(Bytes(data.data(), data.data() + data.size()));
    return true;
  }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
  uint32_t port_generation(uint32_t) const override { return generation; }

  void plug(uint16_t v, uint16_t p, const Bytes& c) { vendor = v; product = p; config = c; present = true; ++generation; sent.clear(); set_reports.clear(); }
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
  printf("GameControllerTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
