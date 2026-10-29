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

// The state of a draw call as registers and packets for the graphics ring of a Polaris-class card (VEGAM): the shader programs and their resources (SPI_SHADER_PGM_*, the user data
// that carries the ABI of GcnCompiler.hpp), the interpolation between the stages (SPI_VS_OUT_CONFIG, SPI_PS_INPUT_*), the colour buffer (CB_COLOR0_*), the viewport and scissors, the
// stages in use (VS and PS only), and the draw. Register and field numbers come from the kernel's headers (GcnRegisters.inc, scripts/gen_gcn_registers.py). Everything the stream does
// not set keeps the value the ring's clear state gave it (AmdGfx8::build_clear_state).
//
// Not done: depth and stencil, blending (the colour goes to the target as the pixel shader exports it), multisampling, geometry / tessellation, instancing beyond one, textures (the
// descriptors of images and samplers), tiled targets (the colour buffer is linear), and none of it has run on a card.
#pragma once

#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnPm4.hpp"
#include "Utility/ArrayView.hpp"

namespace gcn {

struct ShaderProgram {
  uint64_t address = 0;               // GPU address of the code: 256 byte aligned, in the 40 bit space
  ShaderInfo info;
};

// A float RGBA colour target (CB format 32_32_32_32), linear (tile mode index 8 of the VI table: ARRAY_LINEAR_ALIGNED).
struct ColorTarget {
  uint64_t address = 0;               // 256 byte aligned
  uint32_t width = 0, height = 0;
  uint32_t pitch_pixels = 0;          // a multiple of 8 (the pitch is given to the CB in units of 8 pixels)
  static constexpr uint32_t kPixelBytes = 16;
  static constexpr uint32_t kLinearAlignedTileIndex = 8;
};

struct DrawState {
  ShaderProgram vertex, pixel;
  uint64_t vertex_constants = 0;      // GPU addresses of the constant rows each stage reads (user data 0, 1) and of the descriptor table (user data 2, 3)
  uint64_t pixel_constants = 0;
  uint64_t descriptor_table = 0;
  ColorTarget target;
  uint8_t ps_param[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};      // SPI_PS_INPUT_CNTL_k.OFFSET: the parameter pixel shader input k reads
  // The draw: `index_count` vertices (a triangle list) numbered 0, 1, 2 ... (DRAW_INDEX_AUTO), or when `index_address` is not 0 the 32 bit indices at that address (DRAW_INDEX_2).
  uint32_t index_count = 0;
  uint64_t index_address = 0;
  uint32_t index_max = 0;             // indexed: the number of indices the buffer holds
  uint64_t fence_address = 0;         // when not 0 an end-of-pipe event writes `fence_value` there (8 byte aligned) after the draw's caches are written back
  uint64_t fence_value = 0;
};

// Registers and the draw into `out`. False (and `out.ok()` false) if the state is not expressible: a misaligned address, an address beyond 40 bits, a target that is not a multiple of 8
// pixels wide or does not fit the registers, a shader that is not the right stage or uses more registers than exist, more than 16 interpolants.
bool emit_draw(Pm4Writer& out, const DrawState& state);

// The resource descriptor (V#) of a vertex attribute of 1 to 4 floats: `base` the address of attribute 0 of vertex 0, `stride` bytes (up to 16383), `num_records` the vertices.
// Writes the four dwords into `out`; false if it does not describe a buffer.
bool make_vertex_descriptor(ArrayView<uint32_t> out, uint64_t base, uint32_t stride, uint32_t num_records, uint32_t components);

// The values of the registers (for the command processor model and for tests)
uint32_t shader_resource1(const ShaderInfo& info);          // SPI_SHADER_PGM_RSRC1_VS / _PS
uint32_t shader_resource2(const ShaderInfo& info);          // SPI_SHADER_PGM_RSRC2_VS / _PS

}  // namespace gcn
