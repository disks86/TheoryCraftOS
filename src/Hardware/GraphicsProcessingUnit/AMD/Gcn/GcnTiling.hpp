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
//   - ARRAY_2D_TILED_THIN1: macro tiles of micro tiles spread over the memory pipes and banks (the pipe configuration, bank width / height / count and macro tile aspect of the
//     GB_TILE_MODE / GB_MACROTILE_MODE entries, tile splits for micro tiles larger than the split size), as AMD's address library places them for a single slice and sample on the
//     VEGAM (16 pipes in the P16_32x32_16x16 configuration with that chip's rotation of the pipe bits; GB_ADDR_CONFIG 0x22011003: 256 byte pipe interleave, bank interleave 1, 4 KB
//     rows). The surface is padded to whole macro tiles (`surface_pitch_align`, `surface_height_align`) and its base is aligned to `surface_base_align`.
// Not done: the thick, 3D and PRT modes, more than one slice or sample. A mode that is not implemented is refused, not guessed. The micro tile pixel orders and the 2D arithmetic were
// written from the address library's source (egbaddrlib.cpp, siaddrlib.cpp, ciaddrlib.cpp); none of it has been compared with a card.
#pragma once

#include <stdint.h>

namespace gcn {

struct TileLayout {
  enum class Kind : uint8_t { Linear, Tiled1D, Tiled2D };
  enum class Micro : uint8_t { Display, Thin, Depth };
  Kind kind = Kind::Linear;
  Micro micro = Micro::Display;
  uint32_t gb_tile_mode = 0;           // the GB_TILE_MODE entry (the 2D layout reads its pipe configuration and tile split from it)
};

// The golden GB_TILE_MODE register for `index` (0 to 31); 0 beyond.
uint32_t tile_mode_register(uint32_t index);

// The layout a GB_TILE_MODE value names; false for a mode that is not implemented.
bool resolve_tile_mode(uint32_t gb_tile_mode, TileLayout* out);
inline bool resolve_tile_index(uint32_t index, TileLayout* out) { return resolve_tile_mode(tile_mode_register(index), out); }

// A surface of `bytes` per pixel (1, 2, 4, 8 or 16) with a row of `pitch` pixels: a tiled one has a pitch that is a multiple of 8 (a 2D tiled one of `surface_pitch_align`), its rows are
// padded to `surface_height_align` rows, and its base address is a multiple of `surface_base_align`.
bool surface_valid(const TileLayout& layout, uint32_t bytes, uint32_t pitch);
uint32_t surface_pitch_align(const TileLayout& layout, uint32_t bytes);
uint32_t surface_height_align(const TileLayout& layout, uint32_t bytes);
uint64_t surface_base_align(const TileLayout& layout, uint32_t bytes);
// The byte offset of pixel (x, y) in the padded surface of `height` rows (the height matters to a 2D tiled one only); 0 for a surface that is not valid.
uint64_t surface_offset(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t x, uint32_t y, uint32_t height = 0);
// The bytes a surface of `height` rows occupies (the rows padded to the height alignment)
uint64_t surface_bytes(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t height);

}  // namespace gcn
