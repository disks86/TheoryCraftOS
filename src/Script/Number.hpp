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

// Numbers the way JavaScript reads and writes them, without a C library: text to double (also hex, octal and binary), double to the shortest text
// that reads back the same (up to 15 digits; 17 when that is not enough), and the fixed and radix forms.
#pragma once
#include "Utility/String.hpp"

namespace script {

// A decimal literal: digits with an optional fraction and exponent ("12", "1.5e3", ".5"), no sign, no blanks. False for anything else.
bool parse_decimal(StringView text, double* out);
// What Number("...") makes of text: blanks around it are ignored, "" is 0, "Infinity", hex / octal / binary with a prefix, a sign before a decimal.
// False when the result is NaN.
bool string_to_number(StringView text, double* out);

void number_to_string(double value, String* out);
// 10 for the ordinary form, 2..36 for toString(radix).
void number_to_radix(double value, int radix, String* out);
// toFixed(digits): 0..100 digits after the point.
void number_to_fixed(double value, int digits, String* out);

double power_of_ten(int exponent);

}  // namespace script
