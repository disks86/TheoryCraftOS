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

// Host-side tests for dinput8.dll's game controller arithmetic (src/Windows/dinput8/joystick_core.h): matching c_dfDIJoystick, c_dfDIJoystick2
// and hand-made data formats, rejecting formats that do not fit, scaling axes to ranges with dead zones and saturation, the POV hat, filling the
// program's buffer, the buffer of changes, and finding objects for properties.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Windows/dinput8/joystick_core.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static const uint32_t kOpt = 0x80000000u, kAny = 0x00FFFF00u;

// c_dfDIJoystick as dinput8.lib defines it: 6 axes, 2 sliders, 4 POVs, 32 buttons, every entry optional and of any instance.
static std::vector<dj_format_entry> joystick_format(uint32_t buttons) {
  std::vector<dj_format_entry> f;
  const dj_guid* axes[6] = {&dj_guid_x, &dj_guid_y, &dj_guid_z, &dj_guid_rx, &dj_guid_ry, &dj_guid_rz};
  for (uint32_t i = 0; i < 6; ++i) f.push_back({axes[i], i * 4, kOpt | DJ_DIDFT_AXIS | kAny, 0x100});
  f.push_back({&dj_guid_slider, 24, kOpt | DJ_DIDFT_AXIS | kAny, 0x100});
  f.push_back({&dj_guid_slider, 28, kOpt | DJ_DIDFT_AXIS | kAny, 0x100});
  for (uint32_t i = 0; i < 4; ++i) f.push_back({&dj_guid_pov, 32 + i * 4, kOpt | DJ_DIDFT_POV | kAny, 0});
  for (uint32_t i = 0; i < buttons; ++i) f.push_back({nullptr, 48 + i, kOpt | DJ_DIDFT_BUTTON | kAny, 0});
  return f;
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }

static void test_formats() {
  dj_format f{};
  std::vector<dj_format_entry> js = joystick_format(32);
  CHECK(dj_set_format(&f, js.data(), static_cast<uint32_t>(js.size()), 80) == DJ_OK);
  CHECK(f.count == 6 + 1 + 11);                       // the sliders and three POVs and buttons 11..31 have nothing behind them
  // c_dfDIJoystick2 is 272 bytes with 128 buttons; the same objects.
  std::vector<dj_format_entry> js2 = joystick_format(128);
  dj_format f2{};
  CHECK(dj_set_format(&f2, js2.data(), static_cast<uint32_t>(js2.size()), 272) == DJ_OK && f2.count == 18);
  // A format that needs an object the pad does not have (a non-optional slider) fails, and leaves the old one.
  dj_format_entry slider = {&dj_guid_slider, 0, DJ_DIDFT_AXIS | kAny, 0};
  CHECK(dj_set_format(&f, &slider, 1, 4) == DJ_INVALID && f.count == 18);
  // A value past the end, unaligned, or overlapping another; a size that is not a multiple of 4 or is zero.
  dj_format_entry past = {&dj_guid_x, 4, DJ_DIDFT_AXIS | kAny, 0};
  CHECK(dj_set_format(&f, &past, 1, 4) == DJ_INVALID);
  dj_format_entry wrapped = {&dj_guid_x, 0xFFFFFFFEu, DJ_DIDFT_AXIS | kAny, 0};
  CHECK(dj_set_format(&f, &wrapped, 1, 8) == DJ_INVALID);
  dj_format_entry odd = {&dj_guid_x, 2, DJ_DIDFT_AXIS | kAny, 0};
  CHECK(dj_set_format(&f, &odd, 1, 8) == DJ_INVALID);
  dj_format_entry overlap[2] = {{&dj_guid_x, 0, DJ_DIDFT_AXIS | kAny, 0}, {nullptr, 2, DJ_DIDFT_BUTTON | kAny, 0}};
  CHECK(dj_set_format(&f, overlap, 2, 8) == DJ_INVALID);
  CHECK(dj_set_format(&f, js.data(), static_cast<uint32_t>(js.size()), 82) == DJ_INVALID);
  CHECK(dj_set_format(&f, js.data(), 0, 0) == DJ_INVALID);
  // A specific instance: button 3 only, at offset 0.
  dj_format_entry third = {&dj_guid_button, 0, DJ_DIDFT_BUTTON | (3u << 8), 0};
  dj_format one{};
  CHECK(dj_set_format(&one, &third, 1, 4) == DJ_OK && one.count == 1 && one.object[0] == DJ_AXES + 1 + 3);
  // Any axis, in order: X, Y, Z ...
  dj_format_entry any_axes[3] = {{nullptr, 0, DJ_DIDFT_AXIS | kAny, 0}, {nullptr, 4, DJ_DIDFT_AXIS | kAny, 0}, {nullptr, 8, DJ_DIDFT_AXIS | kAny, 0}};
  dj_format three{};
  CHECK(dj_set_format(&three, any_axes, 3, 12) == DJ_OK && three.object[0] == 0 && three.object[1] == 1 && three.object[2] == 2);
}

static void test_values() {
  dj_axis_props p;
  dj_default_props(&p);
  uint32_t v[DJ_OBJECTS];
  dj_pad pad{};
  dj_values(&pad, &p, v);
  CHECK(v[0] == 32767 && v[1] == 32767 && v[2] == 0 && v[5] == 0 && v[DJ_AXES] == 0xFFFFFFFFu);
  pad.left_x = 32767;
  pad.left_y = 32767;              // up: Y's minimum
  pad.right_x = -32768;
  pad.right_y = -32768;            // down: Y's maximum
  pad.left_trigger = 255;
  pad.buttons = 0x1000 | 0x0400 | 0x0001 | 0x0008;   // south, guide, up + right
  dj_values(&pad, &p, v);
  CHECK(v[0] == 65535 && v[1] == 0 && v[3] == 0 && v[4] == 65535 && v[2] == 65535 && v[5] == 0);
  CHECK(v[DJ_AXES] == 4500 && v[DJ_AXES + 1] == 0x80 && v[DJ_AXES + 1 + 10] == 0x80 && v[DJ_AXES + 2] == 0);
  pad.buttons = 0x0002 | 0x0004;   // down + left
  dj_values(&pad, &p, v);
  CHECK(v[DJ_AXES] == 22500);
  pad.buttons = 0x0001 | 0x0002;   // up and down at once: centred
  dj_values(&pad, &p, v);
  CHECK(v[DJ_AXES] == 0xFFFFFFFFu);

  // A range of -1000..1000, a dead zone of 20 % and saturation at 80 %.
  for (int i = 0; i < DJ_AXES; ++i) { p.min[i] = -1000; p.max[i] = 1000; p.deadzone[i] = 2000; p.saturation[i] = 8000; }
  pad = dj_pad{};
  pad.left_x = 6000;                // inside the dead zone
  pad.right_x = 30000;              // past saturation
  dj_values(&pad, &p, v);
  CHECK(static_cast<int32_t>(v[0]) == 0 && static_cast<int32_t>(v[3]) == 1000);
  pad.left_x = 16384;               // half-way: (0.5 - 0.2) / 0.6 = 0.5
  dj_values(&pad, &p, v);
  CHECK(static_cast<int32_t>(v[0]) >= 495 && static_cast<int32_t>(v[0]) <= 505);
  pad.left_x = -16384;
  dj_values(&pad, &p, v);
  CHECK(static_cast<int32_t>(v[0]) <= -495 && static_cast<int32_t>(v[0]) >= -505);
  // A saturation at or below the dead zone: everything outside it is full scale. Extreme ranges do not overflow.
  p.deadzone[0] = 5000;
  p.saturation[0] = 5000;
  pad.left_x = 20000;
  dj_values(&pad, &p, v);
  CHECK(static_cast<int32_t>(v[0]) == 1000);
  p.min[0] = INT32_MIN;
  p.max[0] = INT32_MAX;
  p.deadzone[0] = 0;
  p.saturation[0] = 10000;
  pad.left_x = 32767;
  dj_values(&pad, &p, v);
  CHECK(static_cast<int32_t>(v[0]) == INT32_MAX);
  pad.left_x = -32768;
  dj_values(&pad, &p, v);
  CHECK(static_cast<int32_t>(v[0]) == INT32_MIN);
}

static void test_fill_and_buffer() {
  dj_axis_props p;
  dj_default_props(&p);
  dj_format f{};
  std::vector<dj_format_entry> js = joystick_format(32);
  CHECK(dj_set_format(&f, js.data(), static_cast<uint32_t>(js.size()), 80) == DJ_OK);
  dj_pad pad{};
  pad.buttons = 0x2000;            // east: button 1
  pad.right_trigger = 255;
  uint32_t before[DJ_OBJECTS], now[DJ_OBJECTS];
  dj_values(&pad, &p, now);
  uint8_t state[80];
  memset(state, 0xEE, sizeof state);
  dj_fill_state(&f, now, state);
  CHECK(le32(state) == 32767 && le32(state + 20) == 65535 && le32(state + 24) == 0 && le32(state + 32) == 0xFFFFFFFFu && le32(state + 36) == 0);
  CHECK(state[48] == 0 && state[49] == 0x80 && state[60] == 0 && state[79] == 0);

  static dj_buffer b;
  memset(&b, 0, sizeof b);
  dj_record(&b, &f, now, now, 1);                     // not buffered yet
  CHECK(b.count == 0);
  dj_set_buffer_size(&b, 4);
  memcpy(before, now, sizeof now);
  pad.buttons = 0x1000;            // east up, south down
  dj_values(&pad, &p, now);
  dj_record(&b, &f, before, now, 7);
  CHECK(b.count == 2 && !b.overflow);
  dj_event e[8];
  CHECK(dj_take(&b, e, 8, 1) == 2 && b.count == 2);   // peek
  CHECK(e[0].offset == 48 && e[0].data == 0x80 && e[1].offset == 49 && e[1].data == 0 && e[0].sequence < e[1].sequence && e[0].time == 7);
  CHECK(dj_take(&b, e, 1, 0) == 1 && b.count == 1 && e[0].offset == 48);
  CHECK(dj_take(&b, nullptr, 8, 0) == 1 && b.count == 0);     // (flush)
  // More changes than room: the oldest go and the overflow is noted.
  memcpy(before, now, sizeof now);
  pad = dj_pad{};
  pad.left_x = 100; pad.left_y = 100; pad.right_x = 100; pad.right_y = 100; pad.left_trigger = 9;
  pad.buttons = 0x2000;
  dj_values(&pad, &p, now);
  dj_record(&b, &f, before, now, 8);
  CHECK(b.count == 4 && b.overflow);
  CHECK(dj_take(&b, e, 8, 0) == 4 && e[3].offset == 49);
  dj_set_buffer_size(&b, 100000);
  CHECK(b.capacity == DJ_MAX_BUFFER);
}

static void test_find() {
  dj_format f{};
  std::vector<dj_format_entry> js = joystick_format(32);
  CHECK(dj_set_format(&f, js.data(), static_cast<uint32_t>(js.size()), 80) == DJ_OK);
  CHECK(dj_find_object(&f, 1, 4) == 1);               // by offset: Y
  CHECK(dj_find_object(&f, 1, 24) == -1);             // (the slider: nothing there)
  CHECK(dj_find_object(&f, 2, DJ_DIDFT_ABSAXIS | (5u << 8)) == 5);
  CHECK(dj_find_object(&f, 2, DJ_DIDFT_PSHBUTTON | (2u << 8)) == DJ_AXES + 1 + 2);
  CHECK(dj_find_object(&f, 2, DJ_DIDFT_PSHBUTTON | (40u << 8)) == -1);
  CHECK(dj_find_object(&f, 3, 1 | (0x39u << 16)) == DJ_AXES);   // by usage: the hat
  dj_format none{};
  CHECK(dj_find_object(&none, 1, 0) == -1);
}

int main() {
  test_formats();
  test_values();
  test_fill_and_buffer();
  test_find();
  printf("DirectInputJoystickTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
