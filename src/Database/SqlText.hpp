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

// Helpers for SQL text that drivers share.
#pragma once
#include "Utility/String.hpp"

namespace database {

// The marker a database wants for a parameter.
enum class PlaceholderStyle : uint8_t {
  Numbered,   // $1, $2, ... (PostgreSQL)
  Question,   // ? (left as it is; only "??" becomes "?")
};

// Rewrites the portable "?" parameter markers of `sql` into the database's own and writes the result to `out`; `count` is the number of markers.
// "??" stands for a literal question mark. Nothing inside quotes ('text', "identifier", $tag$text$tag$, E'text with \' escapes') or comments (-- to
// the end of the line, /* nested */) is touched. False only when memory ran out.
bool translate_placeholders(StringView sql, PlaceholderStyle style, String* out, uint32_t* count);

}  // namespace database
