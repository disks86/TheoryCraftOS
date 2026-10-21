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
#include "Utility/ByteBuffer.hpp"

namespace media {

// Adler-32 (RFC 1950), continuing from `adler` (1 to start).
uint32_t adler32(ConstByteView data, uint32_t adler = 1);

// Inflates a zlib stream (RFC 1950: a two byte header, deflate data, an Adler-32 check) - or, with `raw`, bare
// deflate data (RFC 1951) - appending to `out`. Stored, fixed-Huffman and dynamic-Huffman blocks are handled. False
// if the data is corrupt, ends early, fails its checksum, or would grow past `max_output` bytes (a guard against
// decompression bombs). `consumed` (optional) gets the input bytes used.
bool inflate(ConstByteView in, ByteBuffer* out, uint64_t max_output, bool raw = false, uint64_t* consumed = nullptr);

// Deflates `in` into a zlib stream appended to `out`: LZ77 matching over a 32 KiB window (hash chains) coded with the
// fixed Huffman codes of the standard - always valid, compresses repetition well but not as tightly as a dynamic
// code. `level` 0 stores, 1 to 9 search harder. False if memory ran out.
bool deflate(ConstByteView in, ByteBuffer* out, uint32_t level, allocators::IAllocator& allocator, bool raw = false);

// Inflates what deflate_message() makes - or the data of a permessage-deflate message from anyone else, with the 00 00 FF FF of its sync flush put back:
// raw deflate blocks that need not include a final one, ending where the input ends. Appends to `out`; false if the data is corrupt or would grow past
// `max_output` bytes.
bool inflate_message(ConstByteView in, ByteBuffer* out, uint64_t max_output);

// A message for permessage-deflate (RFC 7692): raw deflate data in non-final blocks, ended by the empty stored block of a sync flush, with that block's
// last four bytes (00 00 FF FF) left off. The receiver puts them back and inflates. `level` as for deflate(); matches reach back at most 2^window_bits bytes.
bool deflate_message(ConstByteView in, ByteBuffer* out, uint32_t level, allocators::IAllocator& allocator, uint32_t window_bits = 15);

}  // namespace media
