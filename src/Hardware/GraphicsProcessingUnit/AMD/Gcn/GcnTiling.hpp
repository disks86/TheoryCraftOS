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


// The memory layout of a surface (a colour target, a depth or stencil plane, a texture): where pixel (x, y) is. A surface names its layout by an index into the GB_TILE_MODE table the
// driver filled (the golden values of the VEGAM are in AmdGfx8Tables.inc); the entry says how the pixels are arranged. Two arrangements are implemented:
//   - ARRAY_LINEAR_ALIGNED: rows of `pitch` pixels one after another;
//   - ARRAY_1D_TILED_THIN1: 8 by 8 pixel micro tiles, laid out row by row (`pitch / 8` of them to a row), the pixels inside a micro tile in the order of the entry's micro tile mode
//     (DISPLAY for what the display engine scans out, THIN and DEPTH for the rest). The order is the bit interleaving of AMD's address library (ComputePixelIndexWithinMicroTile).
// Not done: the 2D tiled modes (pipe and bank swizzling, macro tiles), which the table's depth entries and most of its colour entries use, and the thick modes. A mode that is not
// implemented is refused, not guessed. None of it has been compared with a card; the orders are those of the address library as remembered.
#pragma once

#include <stdint.h>

namespace gcn {

struct TileLayout {
  enum class Kind : uint8_t { Linear, Tiled1D };
  enum class Micro : uint8_t { Display, Thin, Depth };
  Kind kind = Kind::Linear;
  Micro micro = Micro::Display;
};

// The golden GB_TILE_MODE register for `index` (0 to 31); 0 beyond.
uint32_t tile_mode_register(uint32_t index);

// The layout a GB_TILE_MODE value names; false for a mode that is not implemented.
bool resolve_tile_mode(uint32_t gb_tile_mode, TileLayout* out);
inline bool resolve_tile_index(uint32_t index, TileLayout* out) { return resolve_tile_mode(tile_mode_register(index), out); }

// The byte offset of pixel (x, y) of a surface of `bytes` per pixel (1, 2, 4, 8 or 16) and a row of `pitch` pixels (a multiple of 8 for a tiled one); the offset of a pixel inside
// the padded surface. 0 for a size that is not one of those (check `surface_valid`).
bool surface_valid(const TileLayout& layout, uint32_t bytes, uint32_t pitch);
uint64_t surface_offset(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t x, uint32_t y);
// The bytes a surface of `height` rows occupies (the rows padded to a multiple of 8)
uint64_t surface_bytes(uint32_t bytes, uint32_t pitch, uint32_t height);

}  // namespace gcn
