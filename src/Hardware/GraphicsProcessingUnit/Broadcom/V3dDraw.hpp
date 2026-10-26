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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dFrame.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.hpp"

namespace hardware {

// The fixed-function state of a draw, in terms of what the 3D device's pipelines describe.
struct V3dDrawState {
  enum class Cull : uint8_t { None, Back, Front };
  Cull cull = Cull::Back;
  bool front_clockwise = false;  // front faces wind clockwise as they appear on screen (row 0 on top)
  bool depth_test = false;       // pass if closer or equal; needs a depth buffer in the frame
  bool depth_write = false;
  bool alpha_blend = false;      // source over destination
};

// A viewport in whole window pixels (row 0 on top) with its depth mapping as float bit patterns: window z = clip z / w * z_scale + z_offset, and depth beyond
// [z_offset, z_max] is clipped. (No floating point here, so this runs in the AArch64 image; the device rounds a Vulkan viewport to pixels.)
struct V3dViewport {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  uint32_t z_scale_bits = 0x3F800000, z_offset_bits = 0, z_max_bits = 0x3F800000;  // 1.0f, 0.0f, 1.0f
};

// A rectangle of the frame (pixels) the draw is limited to, on top of the viewport.
struct V3dClipRect {
  uint32_t x = 0, y = 0, width = 0, height = 0;
};

// One vertex attribute: where its data is (GPU address of the first vertex's value), the bytes between vertices, its float components (1..4) and how many of
// them the coordinate and the vertex shader read (the VPM holds the values of all attributes one after the other, in attribute order).
struct V3dAttribute {
  uint32_t address = 0, stride = 0;
  uint32_t components = 0;
  uint32_t coordinate_shader_values = 0, vertex_shader_values = 0;
};

// Everything one draw call needs. The shader programs are in the GPU's memory at `code_address` (see V3dShaders); the uniform streams are copied into the list
// memory by the emitter; `defaults_address` is 256 bytes of (0, 0, 0, 1.0f) per four attributes the hardware uses for attribute components a vertex lacks.
struct V3dDrawCall {
  uint32_t code_address = 0;
  v3d_shader::ShaderCode code;
  bool with_z_write = false;  // use the fragment shader that writes Z (a depth buffer is in use)
  uint32_t defaults_address = 0;
  V3dAttribute attributes[4];
  uint32_t attribute_count = 0;
  uint32_t first_vertex = 0, vertex_count = 0;
  // Indexed drawing: the index buffer (GPU address and size in bytes), the index width (1, 2 or 4 bytes) and where in it this draw starts.
  bool indexed = false;
  uint32_t index_address = 0, index_buffer_bytes = 0, index_bytes = 2, index_offset_bytes = 0;
  int32_t base_vertex = 0;  // added to every index (sent as the base vertex packet when it is not zero)
  ArrayView<const uint32_t> coordinate_uniforms, vertex_uniforms, fragment_uniforms;
};

class V3dDrawEmitter {
 public:
  // The 256 bytes of default attribute values the shader record points at.
  static void fill_attribute_defaults(ArrayView<uint32_t> out);

  // The state packets that follow "start tile binning" and precede the first draw: clip window, configuration bits, viewport scaling, blend, write masks, varying
  // flags (all smooth), sample state. False if the viewport does not fit the frame or is empty.
  // `clip`, if given, narrows the clip window to the part of the viewport inside it (False if they do not overlap).
  static bool emit_state(V3dControlList& bcl, const V3dFrameDesc& frame, const V3dViewport& viewport, const V3dDrawState& state, const V3dClipRect* clip = nullptr);

  // Writes the uniform streams, the shader state record and the attribute records into `indirect` (memory the GPU reads; it must stay in place while the frame runs)
  // and the draw into `bcl`. False if a list is too small or the call is malformed.
  static bool emit_draw(V3dControlList& bcl, V3dControlList& indirect, const V3dDrawCall& draw);
};

}  // namespace hardware
