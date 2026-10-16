#pragma once
#include <stdint.h>

namespace hardware {

// Where one value lives inside an input report.
struct HidField {
  bool valid = false;
  uint32_t bit_offset = 0;
  uint8_t bit_size = 0;
  bool is_signed = false;
  bool relative = false;  // moves since the last report, vs an absolute position
  int32_t logical_min = 0;
  int32_t logical_max = 0;
};

// What a pointing device's report descriptor says about its input reports:
// button bits plus X, Y and wheel fields. Works for relative mice and for
// absolute tablets alike.
struct HidPointerLayout {
  bool valid = false;  // has at least X and Y
  uint8_t report_id = 0;  // 0 = reports carry no ID byte
  HidField x, y, wheel;
  HidField buttons[3];  // left, right, middle
};

// Parses a HID report descriptor (USB HID 1.11, section 6.2.2). Items for other
// usages are skipped over, so composite descriptors are fine.
bool parse_hid_pointer(const uint8_t* descriptor, uint16_t length, HidPointerLayout* out);

// Reads a field out of a report as a (sign-extended when signed) integer.
// Returns 0 when the field is invalid or lies outside the report.
int32_t hid_extract(const uint8_t* report, uint16_t report_length, const HidField& field,
                    uint8_t report_id);

}  // namespace hardware
