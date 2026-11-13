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

// XXH64 of `data` (the hash the zstd content checksum takes the low 32 bits of).
[[nodiscard]] uint64_t xxh64(ConstByteView data, uint64_t seed = 0);

// Decompresses Zstandard data (RFC 8878): one or more concatenated frames, skippable frames skipped, appending what
// they hold to `out`. Raw, RLE and compressed blocks; Huffman-coded literals (one or four streams, reused tables);
// FSE-coded sequences in predefined, RLE, compressed and repeat modes; repeat offsets; any window size (the whole
// frame is kept in `out`, so matches may reach back to its start). False - with `out` put back to the size it had -
// if the data is corrupt or ends early, a content checksum or content size does not match, a frame needs a
// dictionary (not supported), the input holds no frame at all, or the output would grow past `max_output` bytes
// (counted from what `out` held on entry: a guard against decompression bombs). Working memory (about 12 KiB of
// tables, plus up to 128 KiB of literals) comes from `out`'s allocator.
[[nodiscard]] bool zstd_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output);

}  // namespace media
