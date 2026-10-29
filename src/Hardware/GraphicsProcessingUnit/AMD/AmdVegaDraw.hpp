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

// A draw call on the Vega M's graphics engine: the shaders, constants, descriptors, vertices and indices are staged in system memory, copied to the framebuffer area of the card's
// block by the SDMA engines (AmdVegaM::upload), the draw is submitted as an indirect buffer on the graphics ring (GcnState's packets), and the colour buffer is copied back. Compile
// the shaders with GcnCompiler (TGSI in, the GCN3 code and its ShaderInfo out); GcnCp models what the ring does with the stream.
//
// The back end: the colour buffer may be linear or 1D tiled (tile mode index 8 or 9), the draw may blend, and may test and write a depth plane (32 bit float) and a stencil plane (8 bit),
// which are cleared to the request's values before the draw and handed back after it, in their layout (`pixel_offset` and friends say where a pixel is). Up to eight textures are given
// as tightly packed rows of texels (with their mip levels and slices); they are put in the area in their tile mode (linear or 1D tiled) and the image and sampler descriptors go in the descriptor table.
//
// The area is laid out from `area_offset` in this order, each piece 256 byte aligned: vertex code, pixel code, vertex constants, pixel constants, the descriptor table, the vertices, the
// indices, the colour buffer, the depth plane, the stencil plane, the textures. Not validated on a real card (see GcnState.hpp for what the stream leaves out).
#pragma once

#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaM.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnDraw.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnState.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTexture.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"

namespace hardware {

struct VegaTexture {
  gcn::TexelFormat format = gcn::TexelFormat::Rgba8Unorm;
  uint32_t width = 0, height = 0;
  uint32_t tile_index = 8;                                          // 8 linear, 9 1D tiled (display); more than one level or any slices need 9
  gcn::ImageType type = gcn::ImageType::Image2D;
  uint32_t depth = 1;                                               // 3D: the depth; array: the layers; cube: 6
  uint32_t levels = 1;
  // The texels: for each level from the base, for each slice, the rows of that level tightly packed (width >> level, at least 1, by height >> level; a 3D image has depth >> level slices
  // in the level).
  ConstByteView texels;
  gcn::SamplerDescriptor sampler;
};

struct VegaDrawRequest {
  ArrayView<const uint32_t> vertex_code, pixel_code;
  gcn::ShaderInfo vertex_info, pixel_info;
  ArrayView<const float> vertex_constants, pixel_constants;          // rows of four floats
  ArrayView<const uint32_t> vertex_data;
  uint32_t vertex_stride = 0;                                       // bytes
  gcn::VertexAttribute attributes[gcn::abi::kMaxAttributes];
  ArrayView<const uint32_t> indices;                                // a triangle list; empty: `vertex_count` vertices drawn in order
  uint32_t vertex_count = 0;
  uint8_t ps_param[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  uint32_t width = 0, height = 0;
  float clear[4] = {0, 0, 0, 0};                                    // what the colour buffer holds before the draw
  uint32_t target_tile_index = 8;                                   // 8 linear, 9 1D tiled (display), 10 2D tiled (display) ... any entry of the golden table GcnTiling implements
  uint32_t depth_tile_index = 13, stencil_tile_index = 13;          // the depth plane's and the stencil plane's entries (13: 1D thin; 2 / 0: the 2D depth entries for 32 bit depth / 8 bit stencil)
  gcn::BlendState blend;
  gcn::DepthStencilState depth_stencil;                             // a depth plane is used when depth_test or depth_write is set, a stencil plane when stencil_test is
  float clear_depth = 1.0f;
  uint8_t clear_stencil = 0;
  VegaTexture textures[gcn::abi::kMaxSamplers];                     // texture i is the shader's SAMP[i] / SVIEW[i]; one with no texels is not bound
};

class AmdVegaDraw {
 public:
  static constexpr uint64_t kAlignment = 256;
  static uint32_t pitch_pixels(uint32_t width) { return (width + 7) / 8 * 8; }
  // The pitch, in pixels, of the colour buffer and of the planes (the width rounded up to what their layouts need; 0 if a layout is not implemented)
  static uint32_t target_pitch(const VegaDrawRequest& r);
  static uint32_t plane_pitch(const VegaDrawRequest& r);
  // Bytes of the colour buffer as AmdVegaDraw::draw() returns them: float RGBA in the target's layout, rows of target_pitch() pixels (a tiled one is padded to whole tiles or macro tiles).
  static uint64_t target_bytes(const VegaDrawRequest& r);
  // The depth plane (4 bytes a pixel) and the stencil plane (1 byte a pixel) in their layout; 0 if the request does not use the plane.
  static bool uses_depth(const VegaDrawRequest& r) { return r.depth_stencil.depth_test || r.depth_stencil.depth_write || r.depth_stencil.depth_bounds; }
  static bool uses_stencil(const VegaDrawRequest& r) { return r.depth_stencil.stencil_test; }
  static uint64_t depth_bytes(const VegaDrawRequest& r);
  static uint64_t stencil_bytes(const VegaDrawRequest& r);
  // Where pixel (x, y) is in the buffers draw() returns (byte offsets; 0 for a layout that is not implemented)
  static uint64_t pixel_offset(const VegaDrawRequest& r, uint32_t x, uint32_t y);
  static uint64_t depth_offset(const VegaDrawRequest& r, uint32_t x, uint32_t y);
  static uint64_t stencil_offset(const VegaDrawRequest& r, uint32_t x, uint32_t y);
  // The bytes the area needs for the request (0 if it is not expressible), and so the size `staging` must have.
  static uint64_t area_bytes(const VegaDrawRequest& r);
  // What `area_offset` must be a multiple of for the request (256 unless a 2D tiled layout wants more; 0 if it is not expressible)
  static uint64_t area_alignment(const VegaDrawRequest& r);

  // Runs the draw. `staging` is system memory the card can reach (the memory upload() copies from), at least area_bytes() long; `pixels` receives the colour buffer (target_bytes() long;
  // system memory the card can reach), `depth` and `stencil` the planes the request uses (depth_bytes() and stencil_bytes() long; an empty view is not filled). `area_offset` is a
  // multiple of 256 in the framebuffer area. False if the request is not expressible, does not fit the area, a transfer or the graphics ring fails; the buffers are then not valid.
  static bool draw(AmdVegaM& card, const VegaDrawRequest& request, uint64_t area_offset, ByteView staging, ByteView pixels, ByteView depth = ByteView(), ByteView stencil = ByteView());
};

}  // namespace hardware
