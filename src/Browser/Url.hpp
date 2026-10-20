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

// Addresses as the browser uses them: a web address (http:// or https://) or a file on the system's own volumes (a path starting with '/', or a
// file:// address, which is shown as the path).
#pragma once
#include "Utility/String.hpp"

namespace browser {

bool is_web_address(StringView address);
bool is_file_address(StringView address);

// What was typed in the address box as an address: surrounding blanks go; "file:///x" becomes "/x"; text without a scheme that is not a path gets
// "http://" in front. False for nothing but blanks or if memory ran out.
bool normalize_address(StringView typed, String* out);

// Where `reference` (a link in the page at `base`) leads: an absolute address as it is, otherwise relative to the page ("/x", "x", "../x", "?q",
// "//host/x" for web pages; for files the folder the file is in). A fragment ("#part") is dropped, a reference to only a fragment gives `base`.
// False for references the browser cannot follow (mailto:, javascript:, ...) or on memory shortage.
bool resolve_reference(allocators::IAllocator& allocator, StringView base, StringView reference, String* out);

// The last part of an address, for titles ("/docs/a.html" -> "a.html"; "http://host/dir/" -> "host").
StringView address_name(StringView address);

}  // namespace browser
