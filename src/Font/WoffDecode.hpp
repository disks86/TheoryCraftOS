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
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace font {

// True if the data starts like a WOFF 1.0 file ('wOFF').
bool is_woff(ConstByteView data);
// True if it starts like a WOFF2 file ('wOF2'); WOFF2 needs Brotli, which the OS does not have, so it is refused.
bool is_woff2(ConstByteView data);

// Unpacks a WOFF 1.0 file into the plain sfnt (TrueType) file it wraps, appended to `sfnt` (zlib inflate through
// media::inflate). False if the header or table directory is inconsistent, a table runs outside the file, a
// table fails to inflate or inflates to a different size than the directory says, or the result would be larger
// than `max_output` bytes (a decompression-bomb guard).
bool decode_woff(ConstByteView woff, ByteBuffer* sfnt, uint64_t max_output);

// Whatever a web font download or a file may be -> an sfnt: a WOFF is decoded, a TrueType file or collection is
// copied as is, WOFF2 and anything unrecognised is refused. `sfnt` should be empty on entry.
bool font_to_sfnt(ConstByteView data, ByteBuffer* sfnt, uint64_t max_output);

}  // namespace font
