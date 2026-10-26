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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dControlList.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"

namespace hardware {

// One image the tile buffer is loaded from or stored to: where it is (GPU address) and the bytes between rows. Raster (linear) layout.
struct V3dSurface {
  uint32_t address = 0;
  uint32_t stride_bytes = 0;
};

// What one frame of rendering is: the size, the color target (8-bit RGBA in memory order R,G,B,A, or B,G,R,A when `swap_rb`), an optional 32-bit float depth target,
// what to clear, load and store. The clear color is the 8-bit R, G, B, A of the tile buffer packed with R in the lowest byte (Mesa never swaps it, whatever the memory
// layout is) and the clear depth is the bits of a float: nothing here uses floating point.
struct V3dFrameDesc {
  uint32_t width = 0, height = 0;
  V3dSurface color;
  bool swap_rb = false;
  bool clear_color = false, load_color = false, store_color = true;
  uint32_t clear_color_rgba8 = 0;
  bool has_depth = false;
  V3dSurface depth;
  bool clear_depth = false, load_depth = false, store_depth = false;
  uint32_t clear_z_bits = 0x3F800000;  // 1.0f
};

struct V3dTileGeometry {
  uint32_t tile_width = 0, tile_height = 0;
  uint32_t tiles_x = 0, tiles_y = 0;
};

// How the OS asks the binner and the renderer to draw: it builds the two control lists of a frame (the binner's, which sorts primitives into tiles, and the
// renderer's, which runs the per-tile list for each tile) the way Mesa's v3d driver does for V3D 4.2, with one 32-bit render target.
class V3dFrameBuilder {
 public:
  // The tile size the hardware uses for this frame (one color target of 32 bits per pixel, no multisampling: 64 x 64), and how many tiles that makes.
  static bool geometry(uint32_t width, uint32_t height, V3dTileGeometry* out);
  // The tile allocation memory the binner needs: the initial block of every tile, rounded to 4 KiB, the two first chunks it takes, and room to avoid blocking on
  // overflow handling. And the tile state array's size (256 bytes a tile).
  static uint64_t tile_alloc_bytes(const V3dTileGeometry& g);
  static uint64_t tile_state_bytes(const V3dTileGeometry& g);

  // The start of the binner's list: the tile binning mode, a cache flush, the occlusion counter off and "start tile binning".
  static void begin_binning(V3dControlList& bcl, const V3dFrameDesc& frame);
  // The end of the binner's list (a FLUSH, which caps the tile lists).
  static void end_binning(V3dControlList& bcl);

  // The renderer's list for the frame. `generic` receives the per-tile list that `rcl` points to (it must be readable by the GPU too and stay in place while the
  // frame runs). `tile_alloc_address` is where the binner was given tile allocation memory. False if a list was too small or the desc is not renderable.
  static bool build_render_list(V3dControlList& rcl, V3dControlList& generic, const V3dFrameDesc& frame, const V3dTileGeometry& g, uint32_t tile_alloc_address);
};

}  // namespace hardware
