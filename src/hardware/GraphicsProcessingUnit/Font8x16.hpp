#pragma once
#include <stdint.h>

namespace hardware {

// Fixed 8x16 font. glyph() returns 16 rows, one byte each, MSB = leftmost
// pixel; characters outside printable ASCII render as '?'.
struct Font8x16 {
  static constexpr uint32_t kWidth = 8;
  static constexpr uint32_t kHeight = 16;
  static const uint8_t* glyph(char c);
};

}  // namespace hardware
