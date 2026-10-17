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
#include "Media/Image.hpp"

namespace media {
namespace webp_internal {

// Decodes the alpha plane of an ALPH chunk (`alph` is the chunk's payload) for a picture of width x height into `alpha`
// (width * height bytes).
bool decode_alpha_plane(allocators::IAllocator& allocator, ConstByteView alph, uint32_t width, uint32_t height, uint8_t* alpha);

// Decodes a VP8 key frame (the payload of a "VP8 " chunk), with the payload of an ALPH chunk if `alph` is not empty.
bool decode_vp8(ConstByteView frame, ConstByteView alph, Image* out);

}  // namespace webp_internal
}  // namespace media
