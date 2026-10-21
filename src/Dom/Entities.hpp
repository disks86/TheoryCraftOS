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

// Character references and UTF-8 for the HTML reader and the renderer: &amp; &#65; &#x41; decoded into UTF-8 (the document keeps the characters it was
// given), and the way a character that the 8x16 screen font cannot show is replaced by ASCII that reads the same ("é" as "e", "—" as "-", "©" as
// "(c)").
#pragma once
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace dom {

// Decodes the character reference that starts at s[at] == '&'; appends what it stands for (UTF-8) to *out and returns how many characters it
// takes. A '&' that starts no reference stands for itself (1 is returned, "&" appended). `in_attribute`: a reference without ";" that is followed by
// a letter, digit or "=" is left alone, as the standard says for attribute values.
uint64_t decode_reference(StringView s, uint64_t at, bool in_attribute, String* out);

void append_utf8(uint32_t code_point, String* out);
// The code point starting at s[*at] (UTF-8), moving *at past it; invalid bytes come back as U+FFFD one at a time.
uint32_t next_code_point(StringView s, uint64_t* at);
// ASCII text that stands for the code point on a screen that has only ASCII ("" for characters with no use, like a zero width space); nullptr if
// there is nothing better than "?".
const char* ascii_look_alike(uint32_t code_point);

}  // namespace dom
