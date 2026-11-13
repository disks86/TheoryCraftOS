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

namespace media {

// Decodes LZO1X data (what lzo1x_decompress_safe() accepts, from any of the lzo1x_1 ... lzo1x_999 compressors; what
// squashfs "lzo" stores), appending to `out`. The input must end exactly with the end marker. False - with `out`
// unchanged - on corrupt or truncated data, trailing bytes, or output past `max_output` bytes. The kernel's LZO-RLE
// variant (a stream starting 0x11 followed by a version byte) is not accepted.
[[nodiscard]] bool lzo1x_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output);

}  // namespace media
