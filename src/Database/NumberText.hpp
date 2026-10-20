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

// Numbers as the text databases send and read them.
#pragma once
#include "Utility/String.hpp"

namespace database {

// Reads a whole decimal integer ("-42", "+7"). False if `text` is not one or does not fit.
bool parse_int64(StringView text, int64_t* out);
// Reads a decimal number with an optional fraction and exponent, or NaN, Infinity and -Infinity. False if `text` is not one.
bool parse_double(StringView text, double* out);
// Appends `value` as text a database reads back (17 significant digits, no trailing zeros; NaN and Infinity as the SQL names).
bool append_double(String* out, double value);

}  // namespace database
