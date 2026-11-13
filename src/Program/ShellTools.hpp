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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// The text filters shell scripts pipe through, for the built-in shell (AppRunScript.hpp): enough of GNU sed, grep, tr, head, tail, sort, uniq, cut,
// wc and awk's '{print $N}' for the one-line pipelines install scripts use to pick a download link out of a web page or a JSON answer
// (`curl ... | sed 's/[()",{} ]/\n/g' | grep -oi "https.*appimage$" | grep -vi "arm64" | head -1`).
//
// Regular expressions are POSIX basic (BRE) or, with -E / -r, extended (ERE), translated to the syntax of the OS's regex engine (src/Regex): bracket
// expressions with [:classes:], \( \) \{ \} \| \+ \? in basic ones, back-references, ^ and $. Each tool reads its input line by line, as the real ones do.
namespace desktop::shell_tools {

// Runs the tool `name` with `arguments` (not counting the name) over `input`, appending what it writes to *out. *status is its exit status (0, 1 for
// "nothing matched", 2 for a usage error). False if there is no such tool here.
[[nodiscard]] bool run(allocators::IAllocator& allocator, StringView name, ArrayView<const StringView> arguments, StringView input, String* out, int* status);

// A POSIX regular expression (`extended`: ERE) in the regex engine's syntax. False if it is malformed.
[[nodiscard]] bool translate_regex(StringView posix, bool extended, String* out);

// GNU sort -V's order of two strings: runs of digits compare as numbers.
int version_compare(StringView a, StringView b);

}  // namespace desktop::shell_tools
