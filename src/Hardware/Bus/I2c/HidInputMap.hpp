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


// The input fields of a HID report descriptor (USB HID 1.11, 6.2.2), in general: every variable input item with its usage page and usage, its place in the report, its logical and
// physical range, and for the multi-touch devices the finger it belongs to (the n-th collection of usage Digitizers / Finger inside the report). HidReportParser.hpp finds the one X /
// Y / wheel / buttons layout of a mouse; this is what the touchpad and the touchscreen of an I2C-HID device need (Windows Precision Touchpad: a finger collection per contact with
// tip switch, confidence, contact id, X and Y; a contact count, a scan time and the click button in the report's top level).
//
// Bit offsets count from the first byte of the report, the report id byte included (as hid_extract wants them). The descriptor comes from a device: it is walked with bounds on
// everything (items, collection depth, pushed states, fields, usages of one item); a descriptor that breaks a bound is refused.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/Hid/HidReportParser.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace hid_usage {
constexpr uint16_t kPageGenericDesktop = 0x01, kPageButton = 0x09, kPageDigitizer = 0x0D;
constexpr uint16_t kPageSensor = 0x20;
constexpr uint16_t kPointer = 0x01, kMouse = 0x02, kX = 0x30, kY = 0x31;
constexpr uint16_t kPen = 0x02, kStylus = 0x20, kInRange = 0x32, kTipPressure = 0x30, kBarrelSwitch = 0x44, kInvert = 0x3C, kEraser = 0x45, kSecondaryBarrel = 0x5A, kXTilt = 0x3D, kYTilt = 0x3E;
constexpr uint16_t kTouchPad = 0x05, kTouchScreen = 0x04, kFinger = 0x22, kTipSwitch = 0x42, kConfidence = 0x47, kContactId = 0x51, kContactCount = 0x54, kScanTime = 0x56, kWidth = 0x48, kHeight = 0x49;
}  // namespace hid_usage

struct HidInputField {
  uint8_t report_id = 0;
  uint16_t usage_page = 0, usage = 0;
  uint32_t bit_offset = 0;
  uint8_t bit_size = 0;
  bool is_signed = false;
  bool relative = false;
  int32_t logical_min = 0, logical_max = 0;
  int32_t physical_min = 0, physical_max = 0;
  int32_t unit = 0, unit_exponent = 0;
  uint8_t application = 0;             // index of the top level (application) collection it is in
  int8_t slot = -1;                    // the finger collection (counted within the report id) the field is in, -1 outside any
  HidField field() const {
    HidField f;
    f.valid = bit_size != 0;
    f.bit_offset = bit_offset;
    f.bit_size = bit_size;
    f.is_signed = is_signed;
    f.relative = relative;
    f.logical_min = logical_min;
    f.logical_max = logical_max;
    return f;
  }
};

// A feature item: what the host can set on the device. A variable item is one field (owner = its own usage); an array item (the selectors of "reporting state": No Events, All
// Events, ...) is one field whose value is the index in `usages`, and its owner is the logical collection around it.
struct HidFeature {
  uint8_t report_id = 0;
  uint8_t application = 0;             // as HidInputField::application
  uint16_t owner_page = 0, owner_usage = 0;
  uint32_t bit_offset = 0;
  uint8_t bit_size = 0;
  bool is_array = false;
  int32_t logical_min = 0, logical_max = 0;
  uint16_t usages[8] = {};             // the selectors of an array item (usage page = the owner's)
  uint8_t usage_count = 0;
};

struct HidApplication {
  uint16_t usage_page = 0, usage = 0;
};

class HidInputMap {
 public:
  static constexpr uint32_t kMaxFields = 512, kMaxApplications = 8, kMaxSlots = 16;

  explicit HidInputMap(allocators::IAllocator& allocator) : fields_(allocator), features_(allocator) {}

  bool parse(ConstByteView descriptor);

  ArrayView<const HidInputField> fields() const { return ArrayView<const HidInputField>(fields_.view().data(), fields_.count()); }
  ArrayView<const HidFeature> features() const { return ArrayView<const HidFeature>(features_.view().data(), features_.count()); }
  // The bytes of the feature report with this id (the id byte included).
  uint32_t feature_report_bytes(uint8_t report_id) const { return (feature_bits_[report_id] + 7) / 8; }
  // The field with this usage in the finger slot (-1: outside the fingers) of the report, or nullptr.
  const HidInputField* find(uint8_t report_id, uint16_t usage_page, uint16_t usage, int8_t slot) const;
  uint32_t slots(uint8_t report_id) const;                // fingers in the report
  bool uses_report_ids() const { return uses_ids_; }
  // The bytes of the report with this id (the id byte included), the longest input report with id 0 (any).
  uint32_t report_bytes(uint8_t report_id) const;
  uint32_t application_count() const { return application_count_; }
  const HidApplication& application(uint32_t i) const { return applications_[i < kMaxApplications ? i : 0]; }
  bool has_application(uint16_t usage_page, uint16_t usage) const;

 private:
  List<HidInputField> fields_;
  List<HidFeature> features_;
  uint32_t feature_bits_[256] = {};
  HidApplication applications_[kMaxApplications] = {};
  uint32_t application_count_ = 0;
  bool uses_ids_ = false;
  uint32_t report_bits_[256] = {};
  uint8_t slots_[256] = {};
};

}  // namespace hardware
