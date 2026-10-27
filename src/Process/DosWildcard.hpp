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
#include "Utility/StringView.hpp"

namespace process {

// Windows' file name patterns, as FindFirstFile uses them. A pattern is first normalised the way Win32 does it (trailing spaces and dots), then turned into an NT expression
// ('*' before '.' and '.' after '*' or '?' or at the end become the DOS wildcards '<' and '"', '?' becomes '>'), and matched like FsRtlIsNameInExpression does:
//   *  any run of characters           ?  one character           <  characters up to and including the last '.'
//   >  one character other than '.', or none before a '.' or the end (and then all the '>' after it are used up)
//   "  a '.', or nothing at the end of the name
// A name matches if it does, or if its 8.3 short name does (a name that is not a valid 8.3 name has one, made the way NTFS makes the first alias). The directory entries "."
// and ".." match by their own rules (as the empty name, as ".", or as ".." for the plain pattern).
bool dos_wildcard_matches(StringView pattern, StringView name);

// The 8.3 alias of `name` (at most 12 characters, upper case) if it needs one; its length, or 0 if the name is a valid 8.3 name already (or cannot have an alias).
uint32_t dos_short_name(StringView name, char* out, uint32_t capacity);

}  // namespace process
