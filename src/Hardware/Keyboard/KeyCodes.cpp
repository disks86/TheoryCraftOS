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

#include "Hardware/Keyboard/KeyCodes.hpp"

namespace hardware {

namespace {
// Indexed by usage 0x04..0x38; unshifted then shifted.
constexpr char kPlain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
constexpr char kShifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
constexpr uint8_t kFirst = 0x04;
constexpr uint8_t kLast = 0x38;
}  // namespace

namespace {
// Scan code set 1 make codes 0x01..0x58 -> HID usage (0 = unmapped).
constexpr uint8_t kSet1[0x59] = {
    0x00, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,  // 00-0F
    0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,  // 10-1F
    0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,  // 20-2F
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55, 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,  // 30-3F
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,  // 40-4F
    0x5A, 0x5B, 0x62, 0x63, 0x00, 0x00, 0x64, 0x44, 0x45,                                            // 50-58
};

// After a 0xE0 prefix.
uint8_t extended_usage(uint8_t code) {
  switch (code) {
    case 0x1C: return 0x58;  // keypad enter
    case 0x1D: return 0xE4;  // right control
    case 0x35: return 0x54;  // keypad /
    case 0x37: return 0x46;  // print screen
    case 0x38: return 0xE6;  // right alt
    case 0x47: return 0x4A;  // home
    case 0x48: return 0x52;  // up
    case 0x49: return 0x4B;  // page up
    case 0x4B: return 0x50;  // left
    case 0x4D: return 0x4F;  // right
    case 0x4F: return 0x4D;  // end
    case 0x50: return 0x51;  // down
    case 0x51: return 0x4E;  // page down
    case 0x52: return 0x49;  // insert
    case 0x53: return 0x4C;  // delete
    case 0x5B: return 0xE3;  // left gui
    case 0x5C: return 0xE7;  // right gui
    case 0x5D: return 0x65;  // application
    default:   return 0;
  }
}
}  // namespace

uint8_t usage_from_scan_code_set1(uint8_t code, bool extended) {
  if (extended) return extended_usage(code);
  return code < sizeof(kSet1) ? kSet1[code] : 0;
}

uint8_t usage_from_linux_key(uint16_t code) {
  if (code < sizeof(kSet1)) return kSet1[code];  // codes 1-88 match set-1 make codes
  switch (code) {
    case 96:  return 0x58;  // keypad enter
    case 97:  return 0xE4;  // right control
    case 98:  return 0x54;  // keypad /
    case 99:  return 0x46;  // print screen / sysrq
    case 100: return 0xE6;  // right alt
    case 102: return 0x4A;  // home
    case 103: return 0x52;  // up
    case 104: return 0x4B;  // page up
    case 105: return 0x50;  // left
    case 106: return 0x4F;  // right
    case 107: return 0x4D;  // end
    case 108: return 0x51;  // down
    case 109: return 0x4E;  // page down
    case 110: return 0x49;  // insert
    case 111: return 0x4C;  // delete
    case 125: return 0xE3;  // left meta
    case 126: return 0xE7;  // right meta
    case 127: return 0x65;  // compose / application
    default:  return 0;
  }
}

char key_to_ascii(uint8_t usage, uint8_t modifiers) {
  if (usage < kFirst || usage > kLast) return 0;
  const bool shift = modifiers & modifier::kShift;
  return (shift ? kShifted : kPlain)[usage - kFirst];
}

}  // namespace hardware
