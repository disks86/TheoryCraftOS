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

// The VEGAM's GB_ADDR_CONFIG (0x22011003): pipe interleave 256 bytes, bank interleave 1, row size 4 KB
constexpr uint32_t kPipeInterleaveBytes = 256, kBankInterleave = 1, kRowBytes = 4096;
constexpr uint32_t kPipeConfigP16_32x32_16x16 = 0x11;

uint32_t bit(uint32_t v, uint32_t n) { return v >> n & 1; }
uint32_t log2_of(uint32_t v) { uint32_t n = 0; while (v > 1) { v >>= 1; ++n; } return n; }

// What a 2D tiled surface of the entry and pixel size looks like (CiLib::HwlComputeMacroModeIndex)
struct Macro {
  uint32_t pipes, pipe_config, banks, bank_width, bank_height, aspect, tile_split_bytes;
  uint32_t pitch_align() const { return 8 * bank_width * pipes * aspect; }
  uint32_t height_align() const { return 8 * bank_height * banks / aspect; }
};

bool pipes_of(uint32_t config, uint32_t* pipes) {
  switch (config) {
    case 0: *pipes = 2; return true;
    case 4: case 5: case 6: case 7: *pipes = 4; return true;
    case 8: case 9: case 0xa: case 0xb: case 0xc: case 0xd: case 0xe: *pipes = 8; return true;
    case 0x10: case 0x11: *pipes = 16; return true;
    default: return false;
  }
}

bool macro_of(const TileLayout& layout, uint32_t bytes, Macro* m) {
  const uint32_t reg = layout.gb_tile_mode;
  m->pipe_config = (reg & GB_TILE_MODE0__PIPE_CONFIG_MASK) >> GB_TILE_MODE0__PIPE_CONFIG__SHIFT;
  if (!pipes_of(m->pipe_config, &m->pipes)) return false;
  const uint32_t tile_bytes_1x = bytes * 64;
  uint32_t split;
  if (layout.micro == TileLayout::Micro::Depth) split = 64u << ((reg & GB_TILE_MODE0__TILE_SPLIT_MASK) >> GB_TILE_MODE0__TILE_SPLIT__SHIFT);
  else {
    const uint32_t sample_split = 1u << ((reg & GB_TILE_MODE0__SAMPLE_SPLIT_MASK) >> GB_TILE_MODE0__SAMPLE_SPLIT__SHIFT);
    split = sample_split * tile_bytes_1x < 256 ? 256 : sample_split * tile_bytes_1x;
  }
  const uint32_t split_c = split < kRowBytes ? split : kRowBytes;
  uint32_t tile_bytes = split_c < tile_bytes_1x ? split_c : tile_bytes_1x;
  if (tile_bytes < 64) tile_bytes = 64;
  const uint32_t index = log2_of(tile_bytes / 64);
  if (index >= 16) return false;
  const uint32_t mt = kMacrotileModes[index];
  m->bank_width = 1u << ((mt & GB_MACROTILE_MODE0__BANK_WIDTH_MASK) >> GB_MACROTILE_MODE0__BANK_WIDTH__SHIFT);
  m->bank_height = 1u << ((mt & GB_MACROTILE_MODE0__BANK_HEIGHT_MASK) >> GB_MACROTILE_MODE0__BANK_HEIGHT__SHIFT);
  m->aspect = 1u << ((mt & GB_MACROTILE_MODE0__MACRO_TILE_ASPECT_MASK) >> GB_MACROTILE_MODE0__MACRO_TILE_ASPECT__SHIFT);
  m->banks = 1u << (((mt & GB_MACROTILE_MODE0__NUM_BANKS_MASK) >> GB_MACROTILE_MODE0__NUM_BANKS__SHIFT) + 1);
  m->tile_split_bytes = split_c;
  return m->aspect <= m->banks && m->aspect != 0;
}

// SiLib::ComputePipeFromCoord for one slice and no swizzle
uint32_t pipe_of(const Macro& m, uint32_t x, uint32_t y) {
  const uint32_t tx = x / 8, ty = y / 8;
  const uint32_t x3 = bit(tx, 0), x4 = bit(tx, 1), x5 = bit(tx, 2), x6 = bit(tx, 3);
  const uint32_t y3 = bit(ty, 0), y4 = bit(ty, 1), y5 = bit(ty, 2), y6 = bit(ty, 3);
  uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  switch (m.pipe_config) {
    case 0: b0 = x3 ^ y3; break;
    case 4: b0 = x4 ^ y3; b1 = x3 ^ y4; break;
    case 5: b0 = x3 ^ y3 ^ x4; b1 = x4 ^ y4; break;
    case 6: b0 = x3 ^ y3 ^ x4; b1 = x4 ^ y5; break;
    case 7: b0 = x3 ^ y3 ^ x5; b1 = x5 ^ y5; break;
    case 8: b0 = x4 ^ y3 ^ x5; b1 = x3 ^ y5; break;
    case 9: b0 = x4 ^ y3 ^ x5; b1 = x3 ^ y4; b2 = x4 ^ y5; break;
    case 0xa: b0 = x4 ^ y3 ^ x5; b1 = x3 ^ y4; b2 = x5 ^ y5; break;
    case 0xb: b0 = x3 ^ y3 ^ x4; b1 = x5 ^ y4; b2 = x4 ^ y5; break;
    case 0xc: b0 = x3 ^ y3 ^ x4; b1 = x4 ^ y4; b2 = x5 ^ y5; break;
    case 0xd: b0 = x3 ^ y3 ^ x4; b1 = x4 ^ y6; b2 = x5 ^ y5; break;
    case 0xe: b0 = x3 ^ y3 ^ x5; b1 = x6 ^ y5; b2 = x5 ^ y6; break;
    case 0x10: b0 = x4 ^ y3; b1 = x3 ^ y4; b2 = x5 ^ y6; b3 = x6 ^ y5; break;
    default: b0 = x3 ^ y3 ^ x4; b1 = x4 ^ y4; b2 = x5 ^ y6; b3 = x6 ^ y5; break;          // 0x11
  }
  if (m.pipes == 16) {                       // (the VEGAM rotates the pipe bits)
    const uint32_t msb = b0;
    b0 = b1; b1 = b2; b2 = b3; b3 = msb;
  }
  return b0 | b1 << 1 | b2 << 2 | b3 << 3;
}

// EgBasedLib::ComputeBankFromCoord for one slice and no swizzle
uint32_t bank_of(const Macro& m, uint32_t x, uint32_t y, uint32_t tile_split_slice) {
  const uint32_t tx = x / 8 / (m.bank_width * m.pipes), ty = y / 8 / m.bank_height;
  const uint32_t x3 = bit(tx, 0), x4 = bit(tx, 1), x5 = bit(tx, 2), x6 = bit(tx, 3);
  const uint32_t y3 = bit(ty, 0), y4 = bit(ty, 1), y5 = bit(ty, 2), y6 = bit(ty, 3);
  uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  switch (m.banks) {
    case 16: b0 = x3 ^ y6; b1 = x4 ^ y5 ^ y6; b2 = x5 ^ y4; b3 = x6 ^ y3; break;
    case 8: b0 = x3 ^ y5; b1 = x4 ^ y4 ^ y5; b2 = x5 ^ y3; break;
    case 4: b0 = x3 ^ y4; b1 = x4 ^ y3; break;
    default: b0 = x3 ^ y3; break;            // 2
  }
  uint32_t bank = b0 | b1 << 1 | b2 << 2 | b3 << 3;
  // HwlPreAdjustBank
  if ((m.pipe_config == 7 || m.pipe_config == 0xe) && m.bank_width == 1) bank |= bit(bank, 0) ^ bit(x / 8, 1) ^ bit(x / 8, 2);
  bank ^= ((m.banks / 2) + 1) * tile_split_slice;
  return bank & (m.banks - 1);
}

uint64_t offset_2d(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t height, uint32_t x, uint32_t y) {
  Macro m;
  if (!macro_of(layout, bytes, &m)) return 0;
  const Order& order = order_for(layout.micro, bytes);
  const uint32_t bits[6] = {x & 1, x >> 1 & 1, x >> 2 & 1, y & 1, y >> 1 & 1, y >> 2 & 1};
  uint32_t pixel_index = 0;
  for (uint32_t k = 0; k < 6; ++k) pixel_index |= bits[order.bit[k]] << k;
  uint32_t element = pixel_index * bytes;
  uint32_t micro_bytes = 64 * bytes, slices_per_tile = 1, tile_split_slice = 0;
  if (micro_bytes > m.tile_split_bytes) {
    slices_per_tile = micro_bytes / m.tile_split_bytes;
    tile_split_slice = element / m.tile_split_bytes;
    element %= m.tile_split_bytes;
    micro_bytes = m.tile_split_bytes;
  }
  (void)slices_per_tile;
  const uint32_t macro_pitch = m.pitch_align(), macro_height = m.height_align();
  const uint64_t macro_bytes = static_cast<uint64_t>(micro_bytes) * (macro_pitch / 8) * (macro_height / 8) / (m.pipes * m.banks);
  const uint32_t per_row = pitch / macro_pitch;
  const uint64_t macro_offset = (static_cast<uint64_t>(y / macro_height) * per_row + x / macro_pitch) * macro_bytes;
  const uint64_t slice_bytes = static_cast<uint64_t>(per_row) * (height / macro_height) * macro_bytes;
  const uint32_t tile_row = (y / 8) % m.bank_height, tile_column = ((x / 8) / m.pipes) % m.bank_width;
  const uint64_t total = slice_bytes * tile_split_slice + macro_offset + element + (tile_row * m.bank_width + tile_column) * static_cast<uint64_t>(micro_bytes);
  const uint32_t pipe_interleave_bits = log2_of(kPipeInterleaveBytes), bank_interleave_bits = log2_of(kBankInterleave), pipe_bits = log2_of(m.pipes), bank_bits = log2_of(m.banks);
  const uint64_t low = total & (kPipeInterleaveBytes - 1);
  const uint64_t bank_interleave = (total >> pipe_interleave_bits) & (kBankInterleave - 1);
  const uint64_t rest = total >> (pipe_interleave_bits + bank_interleave_bits);
  return low | static_cast<uint64_t>(pipe_of(m, x, y)) << pipe_interleave_bits | bank_interleave << (pipe_interleave_bits + pipe_bits) |
         static_cast<uint64_t>(bank_of(m, x, y, tile_split_slice)) << (pipe_interleave_bits + pipe_bits + bank_interleave_bits) |
         rest << (pipe_interleave_bits + pipe_bits + bank_interleave_bits + bank_bits);
}

}  // namespace

uint32_t tile_mode_register(uint32_t index) { return index < 32 ? kTileModes[index] : 0; }

bool resolve_tile_mode(uint32_t gb, TileLayout* out) {
  const uint32_t array = (gb & GB_TILE_MODE0__ARRAY_MODE_MASK) >> GB_TILE_MODE0__ARRAY_MODE__SHIFT;
  const uint32_t micro = (gb & GB_TILE_MODE0__MICRO_TILE_MODE_NEW_MASK) >> GB_TILE_MODE0__MICRO_TILE_MODE_NEW__SHIFT;
  out->gb_tile_mode = gb;
  if (array == kARRAY_LINEAR_ALIGNED) {
    out->kind = TileLayout::Kind::Linear;
    out->micro = TileLayout::Micro::Display;
    return true;
  }
  if (array == kARRAY_1D_TILED_THIN1) out->kind = TileLayout::Kind::Tiled1D;
  else if (array == kARRAY_2D_TILED_THIN1) out->kind = TileLayout::Kind::Tiled2D;
  else return false;
  switch (micro) {
    case kADDR_SURF_DISPLAY_MICRO_TILING: out->micro = TileLayout::Micro::Display; break;
    case kADDR_SURF_THIN_MICRO_TILING: out->micro = TileLayout::Micro::Thin; break;
    case kADDR_SURF_DEPTH_MICRO_TILING: out->micro = TileLayout::Micro::Depth; break;
    default: return false;
  }
  Macro m;
  return out->kind == TileLayout::Kind::Tiled1D || macro_of(*out, 4, &m);
}

uint32_t surface_pitch_align(const TileLayout& layout, uint32_t bytes) {
  if (layout.kind == TileLayout::Kind::Linear) return 1;
  Macro m;
  return layout.kind == TileLayout::Kind::Tiled2D && macro_of(layout, bytes, &m) ? m.pitch_align() : 8;
}

uint32_t surface_height_align(const TileLayout& layout, uint32_t bytes) {
  if (layout.kind == TileLayout::Kind::Linear) return 1;
  Macro m;
  return layout.kind == TileLayout::Kind::Tiled2D && macro_of(layout, bytes, &m) ? m.height_align() : 8;
}

uint64_t surface_base_align(const TileLayout& layout, uint32_t bytes) {
  Macro m;
  if (layout.kind != TileLayout::Kind::Tiled2D || !macro_of(layout, bytes, &m)) return 256;
  uint32_t tile = 64 * bytes;
  if (tile > m.tile_split_bytes) tile = m.tile_split_bytes;
  const uint64_t a = static_cast<uint64_t>(m.pipes) * m.bank_width * m.banks * m.bank_height * tile;
  return a < 256 ? 256 : a;
}

bool surface_valid(const TileLayout& layout, uint32_t bytes, uint32_t pitch) {
  const bool size_ok = bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 || bytes == 16;
  if (!size_ok || pitch == 0 || pitch > 16384) return false;
  if (layout.kind == TileLayout::Kind::Linear) return true;
  if (layout.kind == TileLayout::Kind::Tiled1D) return pitch % 8 == 0;
  Macro m;
  return macro_of(layout, bytes, &m) && pitch % m.pitch_align() == 0;
}

uint64_t surface_offset(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t x, uint32_t y, uint32_t height) {
  if (!surface_valid(layout, bytes, pitch)) return 0;
  if (layout.kind == TileLayout::Kind::Linear) return (static_cast<uint64_t>(y) * pitch + x) * bytes;
  if (layout.kind == TileLayout::Kind::Tiled2D) {
    const uint32_t align = surface_height_align(layout, bytes);
    return offset_2d(layout, bytes, pitch, (height + align - 1) / align * align, x, y);
  }
  const Order& order = order_for(layout.micro, bytes);
  const uint32_t bits[6] = {x & 1, x >> 1 & 1, x >> 2 & 1, y & 1, y >> 1 & 1, y >> 2 & 1};
  uint32_t index = 0;
  for (uint32_t k = 0; k < 6; ++k) index |= bits[order.bit[k]] << k;
  const uint64_t tile = static_cast<uint64_t>(y / 8) * (pitch / 8) + x / 8;
  return (tile * 64 + index) * bytes;
}

uint64_t surface_bytes(const TileLayout& layout, uint32_t bytes, uint32_t pitch, uint32_t height) {
  const uint64_t align = surface_height_align(layout, bytes);
  return static_cast<uint64_t>(pitch) * ((static_cast<uint64_t>(height) + align - 1) / align * align) * bytes;
}

}  // namespace gcn
