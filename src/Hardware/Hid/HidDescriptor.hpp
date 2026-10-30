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

// HID report descriptors (USB HID 1.11 section 6.2.2, the same format over Bluetooth) taken apart per input report, and decoders for the input reports of keyboards and pointing devices.
//
// The older HidReportParser (Bus/Usb/Hid) reads one pointer out of a descriptor with at most one report id carried in the report's first byte. This one keeps every input report of a
// device apart, as Bluetooth devices need: over GATT a report's id is not in its data (it is in the characteristic's report reference), and one device often has a keyboard, a mouse and
// consumer keys as separate reports. Offsets are in bits from the start of the report's data (after the id byte, if there is one).
//
// The descriptor is untrusted: every length is checked, what does not fit the tables is left out (`truncated`), and a descriptor that ends in the middle of an item ends the walk.
#pragma once
#include <stdint.h>
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace hid {

constexpr uint16_t kPageGenericDesktop = 0x01, kPageKeyboard = 0x07, kPageLed = 0x08, kPageButton = 0x09, kPageConsumer = 0x0C;
constexpr uint16_t kUsagePointer = 0x01, kUsageMouse = 0x02, kUsageKeyboard = 0x06, kUsageX = 0x30, kUsageY = 0x31, kUsageWheel = 0x38;

// A run of input bits with one meaning. A variable field of `count` elements has usages usage_min..usage_max (one per element); an array field's elements each select one usage from
// that range (0 meaning none), the range being what the array can name.
struct InputField {
  uint16_t page = 0;
  uint16_t usage_min = 0, usage_max = 0;
  uint32_t bit_offset = 0, bit_size = 0, count = 1;
  int32_t logical_min = 0, logical_max = 0;
  bool variable = false, relative = false;
};

struct InputReport {
  static constexpr uint32_t kMaxFields = 32;
  uint8_t id = 0;                       // 0: the device numbers its reports not at all
  uint16_t app_page = 0, app_usage = 0; // the application collection the report belongs to (page 1 usage 6: a keyboard, page 1 usage 2: a mouse)
  uint32_t bits = 0;                    // the report's size in bits (without the id)
  InputField fields[kMaxFields];
  uint32_t field_count = 0;
  bool truncated = false;               // fields were left out
  uint32_t bytes() const { return (bits + 7) / 8; }
};

struct ReportMap {
  static constexpr uint32_t kMaxReports = 12;
  InputReport reports[kMaxReports];
  uint32_t report_count = 0;
  bool truncated = false;
  // The input report with this id, or null. (An id of 0 is the report of a device that uses none.)
  const InputReport* find(uint8_t id) const;
  const InputReport* find_application(uint16_t page, uint16_t usage) const;
};

bool parse_report_map(ConstByteView descriptor, ReportMap* out);

// Reads `bits` bits at `bit_offset` from a report as an unsigned number (sign-extended when `is_signed`); 0 for a field outside the data.
int32_t extract_bits(ConstByteView data, uint32_t bit_offset, uint32_t bits, bool is_signed);

// The keys down in a keyboard report.
struct KeyboardState {
  static constexpr uint32_t kMaxKeys = 32;
  uint8_t modifiers = 0;                // modifier:: bits
  uint8_t keys[kMaxKeys] = {};          // usages (page 7) of the keys held, in no order
  uint32_t key_count = 0;
};

class KeyboardDecoder {
 public:
  // Finds the modifier byte and the key array (or the key bitmap of a device that reports every key as a bit) in a keyboard's report; false if it has neither.
  bool init(const InputReport& report);
  bool valid() const { return valid_; }
  // The state a report describes; false if the report is too short for its layout. Rollover error codes (1 to 3) are not keys.
  bool decode(ConstByteView data, KeyboardState* out) const;

 private:
  bool valid_ = false;
  InputField modifiers_, array_, bitmap_;
  bool has_modifiers_ = false, has_array_ = false, has_bitmap_ = false;
};

// Turns a keyboard's successive states into key events (presses and releases, modifiers included), remembering the last state.
class KeyboardDiff {
 public:
  // Calls `emit(usage, pressed)` for every change from the previous state to `now`.
  template <class F>
  void update(const KeyboardState& now, F&& emit) {
    for (int bit = 0; bit < 8; ++bit) {
      const bool is = now.modifiers & (1u << bit), was = previous_.modifiers & (1u << bit);
      if (is != was) emit(static_cast<uint8_t>(0xE0 + bit), is);
    }
    for (uint32_t i = 0; i < now.key_count; ++i)
      if (!has(previous_, now.keys[i])) emit(now.keys[i], true);
    for (uint32_t i = 0; i < previous_.key_count; ++i)
      if (!has(now, previous_.keys[i])) emit(previous_.keys[i], false);
    previous_ = now;
  }
  // Calls `emit(usage, false)` for everything held and forgets it.
  template <class F>
  void release_all(F&& emit) {
    update(KeyboardState(), static_cast<F&&>(emit));
  }

 private:
  static bool has(const KeyboardState& s, uint8_t usage) {
    for (uint32_t i = 0; i < s.key_count; ++i)
      if (s.keys[i] == usage) return true;
    return false;
  }
  KeyboardState previous_;
};

class PointerDecoder {
 public:
  // Finds the buttons, the X and Y axes and the wheel in a pointing device's report.
  bool init(const InputReport& report);
  bool valid() const { return valid_; }
  bool absolute() const { return !relative_; }
  uint32_t max_x() const { return relative_ ? 0 : static_cast<uint32_t>(x_.logical_max); }
  uint32_t max_y() const { return relative_ ? 0 : static_cast<uint32_t>(y_.logical_max); }
  // The event a report describes (buttons as the first three of the device's, movement relative or absolute); false if the report is too short.
  bool decode(ConstByteView data, MouseEvent* out) const;

 private:
  bool valid_ = false, relative_ = true, has_wheel_ = false;
  InputField x_, y_, wheel_;
  InputField buttons_[3];
  bool has_button_[3] = {};
  uint32_t min_bytes_ = 0;
};

}  // namespace hid
}  // namespace hardware
