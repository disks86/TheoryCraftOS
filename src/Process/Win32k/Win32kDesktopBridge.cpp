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

#include "Process/Win32k/Win32kDesktopBridge.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Process/Win32k/Win32kUserMisc.hpp"

namespace process::win32k {

namespace {

// USB HID usages 0x04.. of the keyboard page and the virtual key / set-1 scan code / E0 prefix of each (US layout). 0: no key.
struct KeyRow { uint8_t vk, scan; bool extended; };
constexpr KeyRow kKeys[] = {
    /* 0x00 */ {0, 0, false}, {0, 0, false}, {0, 0, false}, {0, 0, false},
    /* 0x04 A..Z */ {'A', 0x1E, false}, {'B', 0x30, false}, {'C', 0x2E, false}, {'D', 0x20, false}, {'E', 0x12, false}, {'F', 0x21, false}, {'G', 0x22, false},
    {'H', 0x23, false}, {'I', 0x17, false}, {'J', 0x24, false}, {'K', 0x25, false}, {'L', 0x26, false}, {'M', 0x32, false}, {'N', 0x31, false}, {'O', 0x18, false},
    {'P', 0x19, false}, {'Q', 0x10, false}, {'R', 0x13, false}, {'S', 0x1F, false}, {'T', 0x14, false}, {'U', 0x16, false}, {'V', 0x2F, false}, {'W', 0x11, false},
    {'X', 0x2D, false}, {'Y', 0x15, false}, {'Z', 0x2C, false},
    /* 0x1E 1..9 0 */ {'1', 0x02, false}, {'2', 0x03, false}, {'3', 0x04, false}, {'4', 0x05, false}, {'5', 0x06, false}, {'6', 0x07, false}, {'7', 0x08, false},
    {'8', 0x09, false}, {'9', 0x0A, false}, {'0', 0x0B, false},
    /* 0x28 Enter, Escape, Backspace, Tab, Space */ {0x0D, 0x1C, false}, {0x1B, 0x01, false}, {0x08, 0x0E, false}, {0x09, 0x0F, false}, {0x20, 0x39, false},
    /* 0x2D - = [ ] \ (non-US #) ; ' ` , . / */ {0xBD, 0x0C, false}, {0xBB, 0x0D, false}, {0xDB, 0x1A, false}, {0xDD, 0x1B, false}, {0xDC, 0x2B, false},
    {0xDC, 0x2B, false}, {0xBA, 0x27, false}, {0xDE, 0x28, false}, {0xC0, 0x29, false}, {0xBC, 0x33, false}, {0xBE, 0x34, false}, {0xBF, 0x35, false},
    /* 0x39 Caps Lock */ {0x14, 0x3A, false},
    /* 0x3A F1..F12 */ {0x70, 0x3B, false}, {0x71, 0x3C, false}, {0x72, 0x3D, false}, {0x73, 0x3E, false}, {0x74, 0x3F, false}, {0x75, 0x40, false},
    {0x76, 0x41, false}, {0x77, 0x42, false}, {0x78, 0x43, false}, {0x79, 0x44, false}, {0x7A, 0x57, false}, {0x7B, 0x58, false},
    /* 0x46 Print Screen, Scroll Lock, Pause */ {0x2C, 0x37, true}, {0x91, 0x46, false}, {0x13, 0x45, false},
    /* 0x49 Insert, Home, Page Up, Delete, End, Page Down */ {0x2D, 0x52, true}, {0x24, 0x47, true}, {0x21, 0x49, true}, {0x2E, 0x53, true}, {0x23, 0x4F, true},
    {0x22, 0x51, true},
    /* 0x4F Right, Left, Down, Up */ {0x27, 0x4D, true}, {0x25, 0x4B, true}, {0x28, 0x50, true}, {0x26, 0x48, true},
    /* 0x53 Num Lock, keypad / * - + Enter */ {0x90, 0x45, false}, {0x6F, 0x35, true}, {0x6A, 0x37, false}, {0x6D, 0x4A, false}, {0x6B, 0x4E, false},
    {0x0D, 0x1C, true},
    /* 0x59 keypad 1..9 0 . */ {0x61, 0x4F, false}, {0x62, 0x50, false}, {0x63, 0x51, false}, {0x64, 0x4B, false}, {0x65, 0x4C, false}, {0x66, 0x4D, false},
    {0x67, 0x47, false}, {0x68, 0x48, false}, {0x69, 0x49, false}, {0x60, 0x52, false}, {0x6E, 0x53, false},
    /* 0x64 non-US \, Application */ {0xE2, 0x56, false}, {0x5D, 0x5D, true},
};
// 0xE0..0xE7: left Control, Shift, Alt, GUI, right Control, Shift, Alt, GUI
constexpr KeyRow kModifiers[] = {{0xA2, 0x1D, false}, {0xA0, 0x2A, false}, {0xA4, 0x38, false}, {0x5B, 0x5B, true},
                                 {0xA3, 0x1D, true}, {0xA1, 0x36, false}, {0xA5, 0x38, true}, {0x5C, 0x5C, true}};

}  // namespace

bool windows_key_of_usage(uint8_t usage, WindowsKey* out) {
  KeyRow row{0, 0, false};
  if (usage < sizeof kKeys / sizeof kKeys[0]) row = kKeys[usage];
  else if (usage >= 0x68 && usage <= 0x73) row = KeyRow{uint8_t(0x7C + (usage - 0x68)), uint8_t(0x64 + (usage - 0x68)), false};      // F13..F24
  else if (usage >= 0xE0 && usage <= 0xE7) row = kModifiers[usage - 0xE0];
  if (row.vk == 0) return false;
  out->vk = row.vk;
  out->scan = row.scan;
  out->extended = row.extended;
  return true;
}

void DesktopBridge::attach() {
  user_misc::SystemLock lock;
  gdi::set_screen_sink(this);
  attached_ = true;
}

void DesktopBridge::detach() {
  if (!attached_) return;
  user_misc::SystemLock lock;
  gdi::set_screen_sink(nullptr);
  attached_ = false;
}

void DesktopBridge::screen_changed(const gdi::Rect& dirty) {
  (void)dirty;      // (the desktop takes the whole screen: copying it is cheap next to drawing it)
  __atomic_store_n(&changed_, true, __ATOMIC_RELEASE);
}

uint32_t DesktopBridge::screen_width() {
  user_misc::SystemLock lock;
  return gdi::screen_width();
}

uint32_t DesktopBridge::screen_height() {
  user_misc::SystemLock lock;
  return gdi::screen_height();
}

bool DesktopBridge::set_screen_size(uint32_t width, uint32_t height) {
  user_misc::SystemLock lock;
  if (width == gdi::screen_width() && height == gdi::screen_height()) return true;
  return gdi::set_screen_size(width, height);
}

// GDI's surface is B, G, R, X bytes; the desktop wants opaque 0xAARRGGBB.
bool DesktopBridge::copy_screen(ArrayView<uint32_t> out, uint32_t* width, uint32_t* height) {
  user_misc::SystemLock lock;
  const uint32_t w = gdi::screen_width(), h = gdi::screen_height();
  *width = w;
  *height = h;
  const uint64_t count = uint64_t{w} * h;
  const ConstByteView pixels = gdi::screen_pixels();
  if (out.size() < count || pixels.size() < count * 4) return false;
  for (uint64_t i = 0; i < count; ++i) {
    const ConstByteView p = pixels.subview(i * 4, 4);
    out[i] = 0xFF000000u | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
  }
  return true;
}

// A key press types a character too (win32k's TranslateMessage leaves that to the input path): the US layout's, Ctrl+letter its control character.
void DesktopBridge::key(uint8_t hid_usage, bool pressed, uint8_t modifiers) {
  WindowsKey k;
  if (!windows_key_of_usage(hid_usage, &k)) return;
  user::inject_key(k.vk, k.scan, pressed, k.extended);
  if (!pressed) return;
  char c = hardware::key_to_ascii(hid_usage, modifiers);
  const bool control = (modifiers & hardware::modifier::kControl) != 0;
  if (control && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = static_cast<char>(c & 0x1F);
  else if (control) c = 0;
  if (c != 0) user::inject_char(char32_t(static_cast<uint8_t>(c)));
}

void DesktopBridge::mouse_motion(int32_t dx, int32_t dy) { user::inject_mouse_motion(dx, dy); }
void DesktopBridge::mouse_position(int32_t x, int32_t y) { user::inject_mouse_move(x, y); }
void DesktopBridge::mouse_button(uint32_t button, bool down) { user::inject_mouse_button(button, down); }
void DesktopBridge::mouse_wheel(int32_t notches) { user::inject_mouse_wheel(notches * 120, false); }      // (WHEEL_DELTA per notch)
void DesktopBridge::cursor(int32_t* x, int32_t* y) { user::cursor_position(x, y); }
bool DesktopBridge::cursor_confined() { return user::cursor_confined(); }

}  // namespace process::win32k
