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

// Decodes one raw LZ4 block (the sequence format LZ4_decompress_safe() takes; what squashfs "lz4" stores), appending to
// `out`. Matches may only reach back into what this call wrote. False - with `out` unchanged - on corrupt or truncated
// data, an empty input, or output past `max_output` bytes.
[[nodiscard]] bool lz4_block_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output);

// Decodes LZ4 frames (the .lz4 file format: magic 0x184D2204, descriptor with its checksum, blocks of up to 64 KiB to
// 4 MiB, independent or linked, with optional XXH32 block checksums, content size and content checksum), several in a
// row, skippable frames ignored, appending to `out`. Frames that need an external dictionary are refused. False - with
// `out` unchanged - on corrupt or truncated data, a failed checksum, or output past `max_output` bytes.
[[nodiscard]] bool lz4_frame_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output);

}  // namespace media
