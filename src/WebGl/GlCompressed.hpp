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

// Compressed texture formats WebGL can upload (S3TC, RGTC, ETC1, ETC2/EAC, BPTC, ASTC LDR) and their decoders.
#pragma once
#include "WebGl/GlContext.hpp"

namespace webgl {

enum class Codec : uint8_t { Bc1Rgb, Bc1Rgba, Bc2, Bc3, Bc4U, Bc4S, Bc5U, Bc5S, Etc1, Bc6U, Bc6S, Bc7, Astc, Etc2Rgb, Etc2A1, Etc2Rgba, EacR, EacRs, EacRg, EacRgs };

struct CompressedFormat {
  uint32_t internal;       // the GL enum
  uint8_t block_bytes;     // bytes of one 4 x 4 block
  Codec codec;
  uint32_t decoded;        // the format the decoded image is stored as
  uint8_t bw = 4, bh = 4;  // texels of a block (ASTC has many sizes)
  bool srgb = false;       // ASTC: the colours are sRGB encoded
};

// Whether WebGL allows the format in a TEXTURE_3D (the RGTC and BPTC ones; S3TC, ETC and ASTC do not).
inline bool sliced_3d_codec(Codec c) { return c == Codec::Bc4U || c == Codec::Bc4S || c == Codec::Bc5U || c == Codec::Bc5S || c == Codec::Bc6U || c == Codec::Bc6S || c == Codec::Bc7; }
const CompressedFormat* find_compressed(uint32_t internal);
uint64_t compressed_size(const CompressedFormat& f, uint32_t w, uint32_t h, uint32_t d);
// Decodes `data` (w x h x d texels in blocks) into dst at (xo, yo, zo). False if the data is too short.
bool decode_compressed(const CompressedFormat& f, ConstByteView data, uint32_t w, uint32_t h, uint32_t d, Image* dst, uint32_t xo, uint32_t yo, uint32_t zo);

}  // namespace webgl
