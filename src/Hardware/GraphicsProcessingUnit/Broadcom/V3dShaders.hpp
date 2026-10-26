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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// QPU programs for the OS's built-in coloured-vertex pipeline (BuiltinShaders: position and colour attributes, one MVP matrix, the colour passed through
// smoothly). Each is written the way Mesa's v3d compiler lays out the same GLSL for V3D 4.2: single-threaded (no thread switches but the program end), the
// vertex data read from the VPM with `ldvpmv_in`, the shaded vertex written back with `stvpmv` in the order the fixed-function stage reads it, uniforms
// consumed in program order, and the colour written to the tile buffer as packed half floats.
//
//   Coordinate shader (runs when binning): reads position (VPM values 0..2), writes the clip coordinates Xc Yc Zc Wc and the screen position Xs Ys
//     (VPM slots 0..5; Xs and Ys are integers in 1/256 pixel after the viewport scale and the divide by W).
//   Vertex shader (runs when rendering): reads position and colour (values 0..6), writes Xs Ys Zs 1/Wc and the colour (slots 0..7).
//   Fragment shader: interpolates the 4 colour components and writes them to render target 0; the variant with Z writes also sends the rasteriser's Z to the
//     tile buffer (needed with early Z off when a depth buffer is in use).
//
// Uniform streams (32-bit words, in this order): coordinate shader = the 16 matrix floats (column major, as Mat4), then viewport X and Y scale times 256; vertex
// shader = the same 16, then X and Y scale times 256, Z scale, Z offset; fragment shader = TLB configuration words.
namespace v3d_shader {

constexpr uint32_t kCoordinateUniformWords = 18;
constexpr uint32_t kVertexUniformWords = 20;
constexpr uint32_t kFragmentUniformWords = 1;
constexpr uint32_t kFragmentZUniformWords = 2;
constexpr uint32_t kFragmentVaryings = 4;  // colour components

// Values of the TLB configuration words: render target 0 as four half floats per pixel (0xFFFFFFFF), and the Z write whose value comes from the rasteriser.
constexpr uint32_t kTlbColorConfig = 0xFFFFFFFF;
constexpr uint32_t kTlbDepthConfig = 0xFFFFFF84;

// Vertex data layout (VPM values per vertex) the programs expect.
constexpr uint32_t kVertexPositionValues = 3;
constexpr uint32_t kVertexColorValues = 4;

// Where each program starts and how long it is, in instructions, inside the code buffer.
struct Program {
  uint32_t first = 0;
  uint32_t instructions = 0;
  uint32_t offset_bytes() const { return first * 8; }
};
struct ShaderCode {
  Program coordinate, vertex, fragment, fragment_z;
};

// Assembles all four into `storage`. False if it does not fit (about 250 instructions are needed) or an instruction cannot be encoded.
bool build_colored_pipeline(ArrayView<uint64_t> storage, ShaderCode* out);

// The bits of the float that is the integer `n` (exact below 2^24, truncated above): the viewport scales are whole numbers of 1/256 pixel, so no floating point is
// needed to make them.
uint32_t float_bits(uint32_t n);

// Fills a uniform stream from float bit patterns. `mvp` is the 16 floats of the matrix, column major; `x_scale` and `y_scale` are half the viewport's size times 256
// (float_bits(width * 128)), the translation is not needed. Z: window z = clip z / w * z_scale + z_offset.
bool coordinate_uniforms(ArrayView<uint32_t> out, ArrayView<const uint32_t> mvp, uint32_t x_scale, uint32_t y_scale);
bool vertex_uniforms(ArrayView<uint32_t> out, ArrayView<const uint32_t> mvp, uint32_t x_scale, uint32_t y_scale, uint32_t z_scale, uint32_t z_offset);

}  // namespace v3d_shader
}  // namespace hardware
