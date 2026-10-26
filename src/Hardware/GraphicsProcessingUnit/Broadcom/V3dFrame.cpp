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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dFrame.hpp"

namespace hardware {

using namespace v3d_pkt;

namespace {
constexpr uint32_t kBufferRenderTarget0 = 0, kBufferNone = 8, kBufferZ = 9;
constexpr uint32_t kMemoryFormatRaster = memory_format::kRaster;

void emit_load(V3dControlList& cl, uint32_t buffer, const V3dSurface& surface, uint32_t format, bool swap_rb) {
  V3dPacketBuilder p = cl.emit(load_tile_buffer_general::kInfo);
  p.set(load_tile_buffer_general::kBufferToLoad, buffer);
  p.set_address(load_tile_buffer_general::kAddress, surface.address);
  p.set(load_tile_buffer_general::kMemoryFormat, kMemoryFormatRaster);
  p.set(load_tile_buffer_general::kInputImageFormat, format);
  p.set(load_tile_buffer_general::kRBSwap, swap_rb);
  p.set(load_tile_buffer_general::kHeightInUBOrStride, surface.stride_bytes);
  p.set(load_tile_buffer_general::kDecimateMode, decimate_mode::kSample0);
  if (!p.ok()) cl.mark_bad();
}

void emit_store(V3dControlList& cl, uint32_t buffer, const V3dSurface& surface, uint32_t format, bool swap_rb) {
  V3dPacketBuilder p = cl.emit(store_tile_buffer_general::kInfo);
  p.set(store_tile_buffer_general::kBufferToStore, buffer);
  if (buffer != kBufferNone) {
    p.set_address(store_tile_buffer_general::kAddress, surface.address);
    p.set(store_tile_buffer_general::kMemoryFormat, kMemoryFormatRaster);
    p.set(store_tile_buffer_general::kOutputImageFormat, format);
    p.set(store_tile_buffer_general::kRBSwap, swap_rb);
    p.set(store_tile_buffer_general::kHeightInUBOrStride, surface.stride_bytes);
    p.set(store_tile_buffer_general::kDecimateMode, decimate_mode::kSample0);
  }
  if (!p.ok()) cl.mark_bad();
}

void emit_tile_coordinates(V3dControlList& cl, uint32_t column, uint32_t row) {
  V3dPacketBuilder p = cl.emit(tile_coordinates::kInfo);
  p.set(tile_coordinates::kTileColumnNumber, column).set(tile_coordinates::kTileRowNumber, row);
  if (!p.ok()) cl.mark_bad();
}
}  // namespace

bool V3dFrameBuilder::geometry(uint32_t width, uint32_t height, V3dTileGeometry* out) {
  if (width == 0 || height == 0 || width > 4096 || height > 4096) return false;
  out->tile_width = out->tile_height = 64;
  out->tiles_x = (width + 63) / 64;
  out->tiles_y = (height + 63) / 64;
  return true;
}

uint64_t V3dFrameBuilder::tile_alloc_bytes(const V3dTileGeometry& g) {
  uint64_t bytes = uint64_t{g.tiles_x} * g.tiles_y * 64;
  bytes = (bytes + 4095) & ~uint64_t{4095};
  return bytes + 8192 + 512 * 1024;
}

uint64_t V3dFrameBuilder::tile_state_bytes(const V3dTileGeometry& g) { return uint64_t{g.tiles_x} * g.tiles_y * 256; }

void V3dFrameBuilder::begin_binning(V3dControlList& bcl, const V3dFrameDesc& frame) {
  {
    V3dPacketBuilder p = bcl.emit(tile_binning_mode_cfg::kInfo);
    p.set(tile_binning_mode_cfg::kWidthInPixels, frame.width).set(tile_binning_mode_cfg::kHeightInPixels, frame.height);
    p.set(tile_binning_mode_cfg::kNumberOfRenderTargets, 1);
    p.set(tile_binning_mode_cfg::kMaximumBPPOfAllRenderTargets, internal_bpp::k32);
    if (!p.ok()) bcl.mark_bad();
  }
  bcl.emit_simple(flush_vcd_cache::kInfo);
  bcl.emit_simple(occlusion_query_counter::kInfo);  // address 0: no occlusion query left over from another job
  bcl.emit_simple(start_tile_binning::kInfo);
}

void V3dFrameBuilder::end_binning(V3dControlList& bcl) { bcl.emit_simple(flush::kInfo); }

bool V3dFrameBuilder::build_render_list(V3dControlList& rcl, V3dControlList& generic, const V3dFrameDesc& f, const V3dTileGeometry& g, uint32_t tile_alloc_address) {
  if (f.width == 0 || f.height == 0 || g.tiles_x == 0 || g.tiles_y == 0 || (f.has_depth && f.depth.stride_bytes == 0) || f.color.stride_bytes == 0) return false;
  const bool early_zs_clear = false;  // early-Z stays off, so the Z buffer is cleared with the color buffers

  {
    V3dPacketBuilder p = rcl.emit(tile_rendering_mode_cfg_common::kInfo);
    p.set(tile_rendering_mode_cfg_common::kInternalDepthType, internal_depth_type::kDepth32f);
    p.set(tile_rendering_mode_cfg_common::kEarlyZDisable, true);
    p.set(tile_rendering_mode_cfg_common::kEarlyDepthStencilClear, early_zs_clear);
    p.set(tile_rendering_mode_cfg_common::kImageWidthPixels, f.width).set(tile_rendering_mode_cfg_common::kImageHeightPixels, f.height);
    p.set(tile_rendering_mode_cfg_common::kNumberOfRenderTargets, 1);
    p.set(tile_rendering_mode_cfg_common::kMaximumBPPOfAllRenderTargets, internal_bpp::k32);
    p.set(tile_rendering_mode_cfg_common::kSubId, tile_rendering_mode_cfg_common::kSubIdDefault);
    if (!p.ok()) rcl.mark_bad();
  }
  {
    V3dPacketBuilder p = rcl.emit(tile_rendering_mode_cfg_clear_colors_part1::kInfo);
    p.set(tile_rendering_mode_cfg_clear_colors_part1::kClearColorLow32Bits, f.clear_color_rgba8);
    p.set(tile_rendering_mode_cfg_clear_colors_part1::kRenderTargetNumber, 0);
    p.set(tile_rendering_mode_cfg_clear_colors_part1::kSubId, tile_rendering_mode_cfg_clear_colors_part1::kSubIdDefault);
    if (!p.ok()) rcl.mark_bad();
  }
  {
    V3dPacketBuilder p = rcl.emit(tile_rendering_mode_cfg_color::kInfo);
    p.set(tile_rendering_mode_cfg_color::kRenderTarget0InternalBPP, internal_bpp::k32);
    p.set(tile_rendering_mode_cfg_color::kRenderTarget0InternalType, internal_type::k8);
    p.set(tile_rendering_mode_cfg_color::kRenderTarget0Clamp, render_target_clamp::kNone);
    p.set(tile_rendering_mode_cfg_color::kSubId, tile_rendering_mode_cfg_color::kSubIdDefault);
    if (!p.ok()) rcl.mark_bad();
  }
  {
    V3dPacketBuilder p = rcl.emit(tile_rendering_mode_cfg_zs_clear_values::kInfo);
    p.set(tile_rendering_mode_cfg_zs_clear_values::kZClearValue, f.clear_z_bits);
    p.set(tile_rendering_mode_cfg_zs_clear_values::kSubId, tile_rendering_mode_cfg_zs_clear_values::kSubIdDefault);
    if (!p.ok()) rcl.mark_bad();
  }
  {
    // The first block size must match what the binning mode config asked for (64 bytes).
    V3dPacketBuilder p = rcl.emit(tile_list_initial_block_size::kInfo);
    p.set(tile_list_initial_block_size::kUseAutoChainedTileLists, true);
    p.set(tile_list_initial_block_size::kSizeOfFirstBlockInChainedTileLists, 0);
    if (!p.ok()) rcl.mark_bad();
  }
  {
    V3dPacketBuilder p = rcl.emit(multicore_rendering_tile_list_set_base::kInfo);
    p.set_address(multicore_rendering_tile_list_set_base::kAddress, tile_alloc_address);
    if (!p.ok()) rcl.mark_bad();
  }
  // Supertiles: grow them until the frame has fewer than 256.
  uint32_t supertile_w = 1, supertile_h = 1, frame_w_supertiles = 0, frame_h_supertiles = 0;
  for (;;) {
    frame_w_supertiles = (g.tiles_x + supertile_w - 1) / supertile_w;
    frame_h_supertiles = (g.tiles_y + supertile_h - 1) / supertile_h;
    if (frame_w_supertiles * frame_h_supertiles < 256) break;
    if (supertile_w < supertile_h) ++supertile_w;
    else ++supertile_h;
  }
  {
    V3dPacketBuilder p = rcl.emit(multicore_rendering_supertile_cfg::kInfo);
    p.set(multicore_rendering_supertile_cfg::kNumberOfBinTileLists, 1);
    p.set(multicore_rendering_supertile_cfg::kTotalFrameWidthInTiles, g.tiles_x).set(multicore_rendering_supertile_cfg::kTotalFrameHeightInTiles, g.tiles_y);
    p.set(multicore_rendering_supertile_cfg::kSupertileWidthInTiles, supertile_w).set(multicore_rendering_supertile_cfg::kSupertileHeightInTiles, supertile_h);
    p.set(multicore_rendering_supertile_cfg::kTotalFrameWidthInSupertiles, frame_w_supertiles);
    p.set(multicore_rendering_supertile_cfg::kTotalFrameHeightInSupertiles, frame_h_supertiles);
    if (!p.ok()) rcl.mark_bad();
  }
  // An initial clear of the tile buffers (also the GFXH-1742 workaround: two dummy stores after the internal type changes on 4.x).
  emit_tile_coordinates(rcl, 0, 0);
  for (int i = 0; i < 2; ++i) {
    if (i > 0) emit_tile_coordinates(rcl, 0, 0);
    rcl.emit_simple(end_of_loads::kInfo);
    emit_store(rcl, kBufferNone, V3dSurface(), 0, false);
    if (i == 0) {
      V3dPacketBuilder p = rcl.emit(clear_tile_buffers::kInfo);
      p.set(clear_tile_buffers::kClearZStencilBuffer, !early_zs_clear).set(clear_tile_buffers::kClearAllRenderTargets, true);
      if (!p.ok()) rcl.mark_bad();
    }
    rcl.emit_simple(end_of_tile_marker::kInfo);
  }
  rcl.emit_simple(flush_vcd_cache::kInfo);

  // The per-tile list.
  const uint32_t generic_start = generic.gpu_end();
  generic.emit_simple(tile_coordinates_implicit::kInfo);
  if (f.load_color) emit_load(generic, kBufferRenderTarget0, f.color, output_image_format::kRgba8, f.swap_rb);
  if (f.has_depth && f.load_depth) emit_load(generic, kBufferZ, f.depth, output_image_format::kD32f, false);
  generic.emit_simple(end_of_loads::kInfo);
  {
    V3dPacketBuilder p = generic.emit(prim_list_format::kInfo);  // the binner starts out writing triangles
    p.set(prim_list_format::kPrimitiveType, 2);
    if (!p.ok()) generic.mark_bad();
  }
  {
    V3dPacketBuilder p = generic.emit(set_instanceid::kInfo);
    p.set(set_instanceid::kInstanceID, 0);
    if (!p.ok()) generic.mark_bad();
  }
  generic.emit_simple(branch_to_implicit_tile_list::kInfo);
  if (f.store_color) emit_store(generic, kBufferRenderTarget0, f.color, output_image_format::kRgba8, f.swap_rb);
  if (f.has_depth && f.store_depth) emit_store(generic, kBufferZ, f.depth, output_image_format::kD32f, false);
  if (!f.store_color && !(f.has_depth && f.store_depth)) emit_store(generic, kBufferNone, V3dSurface(), 0, false);
  if (f.clear_color || f.clear_depth) {
    V3dPacketBuilder p = generic.emit(clear_tile_buffers::kInfo);
    p.set(clear_tile_buffers::kClearZStencilBuffer, !early_zs_clear).set(clear_tile_buffers::kClearAllRenderTargets, true);
    if (!p.ok()) generic.mark_bad();
  }
  generic.emit_simple(end_of_tile_marker::kInfo);
  generic.emit_simple(return_from_sub_list::kInfo);
  {
    V3dPacketBuilder p = rcl.emit(start_address_of_generic_tile_list::kInfo);
    p.set_address(start_address_of_generic_tile_list::kStart, generic_start).set_address(start_address_of_generic_tile_list::kEnd, generic.gpu_end());
    if (!p.ok()) rcl.mark_bad();
  }

  // Every supertile of the frame, row by row.
  const uint32_t supertile_w_pixels = g.tile_width * supertile_w, supertile_h_pixels = g.tile_height * supertile_h;
  const uint32_t max_x = (f.width - 1) / supertile_w_pixels, max_y = (f.height - 1) / supertile_h_pixels;
  for (uint32_t y = 0; y <= max_y; ++y)
    for (uint32_t x = 0; x <= max_x; ++x) {
      V3dPacketBuilder p = rcl.emit(supertile_coordinates::kInfo);
      p.set(supertile_coordinates::kColumnNumberInSupertiles, x).set(supertile_coordinates::kRowNumberInSupertiles, y);
      if (!p.ok()) rcl.mark_bad();
    }
  rcl.emit_simple(end_of_rendering::kInfo);
  return rcl.ok() && generic.ok();
}

}  // namespace hardware
