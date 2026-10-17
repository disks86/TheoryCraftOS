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

namespace hardware {

// Most USB cameras leave the Huffman tables out of their Motion JPEG frames (the pictures use the standard tables of ITU T.81 annex K), and
// decoders are expected to know them. This puts them in when a frame has none. `buffer` holds the frame in its first `length` bytes and has
// room for kUvcHuffmanBytes more; the new length is returned, or 0 if what is there is not a JPEG.
constexpr uint32_t kUvcHuffmanBytes = 432;
uint32_t uvc_complete_jpeg(ByteView buffer, uint32_t length);

// The standard tables as DHT segments (marker included), 4 of them.
ConstByteView uvc_standard_huffman_tables();

}  // namespace hardware
