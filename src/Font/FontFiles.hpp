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
#include "FileSystem/IFileSystem.hpp"
#include "Utility/StringView.hpp"

namespace font {

// The OS's own fonts (BundledFonts.hpp: TCOS Sans, Serif and Mono in four styles each, and the wide-coverage fallback) as TrueType files in `folder`
// of `fs` (made, with its parents, if missing), for programs that look for font files themselves: Linux programs' fontconfig reads
// /usr/share/fonts. Each is decoded from its WOFF and written only if no file of that name is there yet, so a call that finds them all costs a stat
// each. False if a folder or file could not be made (what could be written stays).
[[nodiscard]] bool write_bundled_fonts(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView folder);

}  // namespace font
