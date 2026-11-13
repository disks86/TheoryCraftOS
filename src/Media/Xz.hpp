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

// CRC-64 as .xz uses it (ECMA-182 polynomial, reflected). Pass the previous result as `crc` to continue.
[[nodiscard]] uint64_t crc64(ConstByteView data, uint64_t crc = 0);

// Decodes a whole .xz file - one or more streams, with stream padding between them - appending to `out`. Blocks may use
// LZMA2 (any dictionary size; the output itself is the dictionary, so nothing beyond the output is allocated for it) behind
// up to three of the filters x86, PowerPC, IA-64, ARM, ARM-Thumb, SPARC, ARM64, RISC-V (BCJ) and Delta. Header, index and
// footer CRC32s are checked, and the block check (CRC32, CRC64, SHA-256; other check types are skipped unverified). False
// - with `out` left as it was - if the data is corrupt, truncated or would grow past `max_output` bytes.
[[nodiscard]] bool xz_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output);

// Decodes the legacy .lzma ("LZMA alone") format - a 13 byte header (properties, dictionary size, uncompressed size or
// all ones for "ends with an end marker") and raw LZMA data - appending to `out`. What squashfs "lzma" compression uses.
// False, with `out` unchanged, on corrupt or truncated data or output past `max_output` bytes.
[[nodiscard]] bool lzma_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output);

}  // namespace media
