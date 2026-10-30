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

// How a device tells what it is, without the user interface knowing the device. A driver (or the code that knows its class) hands each fact to an
// IPropertySink as a name and a value; the computer manager's Properties window is one sink. A new driver of an existing class needs nothing - the
// class interface (IAudioDevice, IStorageDevice, ...) already says what the window shows. A driver with more to tell overrides IHardware::describe()
// and adds its own lines.
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace hardware {

class IPropertySink {
 public:
  // One line of the properties; text that does not fit is cut.
  virtual void add(StringView name, StringView value) = 0;

  // Helpers, all in terms of add().
  void add_text(const char* name, const char* value) { if (value && value[0]) add(name, value); }   // (left out when empty)
  void add_flag(const char* name, bool value) { add(name, value ? "Yes" : "No"); }
  // A number with an optional unit: 4096 "MB" -> "4096 MB".
  void add_number(const char* name, uint64_t value, const char* unit = "") {
    char text[48];
    uint32_t n = 0;
    char digits[20];
    uint32_t d = 0;
    do {
      digits[d++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value && d < sizeof digits);
    while (d) text[n++] = digits[--d];
    if (unit[0]) {
      text[n++] = ' ';
      for (uint32_t i = 0; unit[i] && n + 1 < sizeof text; ++i) text[n++] = unit[i];
    }
    add(name, StringView(text, n));
  }
  // A number in hexadecimal with at least `digits` digits: 0x8086.
  void add_hex(const char* name, uint64_t value, uint32_t digits = 4) {
    static const char kDigits[] = "0123456789ABCDEF";
    char text[24];
    uint32_t n = 0;
    text[n++] = '0';
    text[n++] = 'x';
    if (digits > 16) digits = 16;
    uint32_t shown = 1;
    for (uint64_t v = value >> 4; v; v >>= 4) ++shown;
    if (shown < digits) shown = digits;
    for (uint32_t i = shown; i > 0; --i) text[n++] = kDigits[(value >> ((i - 1) * 4)) & 15];
    add(name, StringView(text, n));
  }

 protected:
  constexpr IPropertySink() = default;
  ~IPropertySink() = default;
};

}  // namespace hardware
