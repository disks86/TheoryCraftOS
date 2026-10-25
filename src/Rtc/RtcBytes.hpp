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

// Big-endian (network order) helpers for the wire formats of WebRTC (ByteBuffer's own put16/put32 are little-endian).
#pragma once
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace rtc {

inline bool put_be16(ByteBuffer* out, uint32_t v) { return out->put8(static_cast<uint8_t>(v >> 8)) && out->put8(static_cast<uint8_t>(v)); }
inline bool put_be24(ByteBuffer* out, uint32_t v) { return out->put8(static_cast<uint8_t>(v >> 16)) && put_be16(out, v); }
inline bool put_be32(ByteBuffer* out, uint32_t v) { return put_be16(out, v >> 16) && put_be16(out, v); }
inline bool put_be64(ByteBuffer* out, uint64_t v) { return put_be32(out, static_cast<uint32_t>(v >> 32)) && put_be32(out, static_cast<uint32_t>(v)); }

// Reads at `at` (the caller has checked there is room: use the checked forms below for input).
inline uint16_t load_be16(ConstByteView d, uint64_t at) { return static_cast<uint16_t>((d[at] << 8) | d[at + 1]); }
inline uint32_t load_be32(ConstByteView d, uint64_t at) { return (uint32_t{load_be16(d, at)} << 16) | load_be16(d, at + 2); }
inline void store_be16(ByteView d, uint64_t at, uint32_t v) {
  d[at] = static_cast<uint8_t>(v >> 8);
  d[at + 1] = static_cast<uint8_t>(v);
}
inline void store_be32(ByteView d, uint64_t at, uint32_t v) {
  store_be16(d, at, v >> 16);
  store_be16(d, at + 2, v);
}

}  // namespace rtc
