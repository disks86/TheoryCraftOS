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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnPm4.hpp"

namespace gcn {

using namespace pm4;

namespace {

#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Tables.inc"

// The pixel index inside a micro tile from the bits of x (x0 x1 x2) and y (y0 y1 y2): six selectors, the lowest bit of the index first. 0..2 are x0..x2, 3..5 are y0..y2.
struct Order { uint8_t bit[6]; };
constexpr Order kDisplay8 = {{0, 1, 2, 4, 3, 5}};          // x0 x1 x2 y1 y0 y2
constexpr Order kDisplay16 = {{0, 1, 2, 3, 4, 5}};         // x0 x1 x2 y0 y1 y2
constexpr Order kDisplay32 = {{0, 1, 3, 2, 4, 5}};         // x0 x1 y0 x2 y1 y2
constexpr Order kDisplay64 = {{0, 3, 1, 2, 4, 5}};         // x0 y0 x1 x2 y1 y2
constexpr Order kDisplay128 = {{3, 0, 1, 2, 4, 5}};        // y0 x0 x1 x2 y1 y2
constexpr Order kZOrder = {{0, 3, 1, 4, 2, 5}};            // x0 y0 x1 y1 x2 y2

const Order& order_for(TileLayout::Micro micro, uint32_t bytes) {
  if (micro != TileLayout::Micro::Display) return kZOrder;
  switch (bytes) {
    case 1: return kDisplay8;
    case 2: return kDisplay16;
    case 4: return kDisplay32;
    case 8: return kDisplay64;
    default: return kDisplay128;
  }
}

}  // namespace

uint32_t tile_mode_register(uint32_t index) { return index < 32 ? kTileModes[index] : 0; }

bool resolve_tile_mode(uint32_t gb, TileLayout* out) {
  const uint32_t array = (gb & GB_TILE_MODE0__ARRAY_MODE_MASK) >> GB_TILE_MODE0__ARRAY_MODE__SHIFT;
  const uint32_t micro = (gb & GB_TILE_MODE0__MICRO_TILE_MODE_NEW_MASK) >> GB_TILE_MODE0__MICRO_TILE_MODE_NEW__SHIFT;
  if (array == kARRAY_LINEAR_ALIGNED) {
    out->kind = TileLayout::Kind::Linear;
    out->micro = TileLayout::Micro::Display;
    return true;
  }
  if (array != kARRAY_1D_TILED_THIN1) return false;
  out->kind = TileLayout::Kind::Tiled1D;
  switch (micro) {
    case kADDR_SURF_DISPLAY_MICRO_TILING: out->micro = TileLayout::Micro::Display; return true;
    case kADDR_SURF_THIN_MICRO_TILING: out->micro = TileLayout::Micro::Thin; return true;
    case kADDR_SURF_DEPTH_MICRO_TILING: out->micro = TileLayout::Micro::Depth; return true;
    default: return false;
  }
}

bool surface_valid(const TileLayout& layout, uint32_t bytes, uint32_t pitch) {
  const bool size_ok = bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 || bytes == 16;
  return size_ok && pitch != 0 && pitch <= 16384 && (layout.kind == TileLayout::Kind::Linear || pitch % 8 == 0);
}

uint64_t surface_offset(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t x, uint32_t y) {
  if (!surface_valid(layout, bytes, pitch)) return 0;
  if (layout.kind == TileLayout::Kind::Linear) return (static_cast<uint64_t>(y) * pitch + x) * bytes;
  const Order& order = order_for(layout.micro, bytes);
  const uint32_t bits[6] = {x & 1, x >> 1 & 1, x >> 2 & 1, y & 1, y >> 1 & 1, y >> 2 & 1};
  uint32_t index = 0;
  for (uint32_t k = 0; k < 6; ++k) index |= bits[order.bit[k]] << k;
  const uint64_t tile = static_cast<uint64_t>(y / 8) * (pitch / 8) + x / 8;
  return (tile * 64 + index) * bytes;
}

uint64_t surface_bytes(uint32_t bytes, uint32_t pitch, uint32_t height) { return static_cast<uint64_t>(pitch) * ((static_cast<uint64_t>(height) + 7) / 8 * 8) * bytes; }

}  // namespace gcn
