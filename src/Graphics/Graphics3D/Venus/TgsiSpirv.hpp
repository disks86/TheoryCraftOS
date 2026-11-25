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

// TGSI (TgsiText.hpp) compiled to SPIR-V 1.5 for Vulkan: how the Venus device (VenusDevice.hpp) runs the shaders every hardware path of the kernel writes.
//
// The program is run as TGSI means it: registers are untyped vec4s of 32-bit words (each TEMP a variable of four uints, bit-cast to what an opcode reads),
// immediates keep their bits, comparisons give ~0 / 0 (the F*, U*, I* ones) or 1.0 / 0.0 (SLT and its kin), IF tests a float against 0.0 (NaN is true), UIF the
// bits. Structured TGSI control flow (IF / UIF / ELSE / ENDIF, BGNLOOP / ENDLOOP / BRK / CONT, RET) becomes SPIR-V's structured control flow; KILL is a demote
// (the invocation goes on as a helper, so derivatives after it work, as on virgl and the CPU machine).
//
// The resources are one descriptor set (set 0). Texture units are the same numbers in every stage; the storage the Direct3D layer numbers densely per shader
// (BUFFER[k], IMAGE[k], the typed buffer views SVIEW[16 + k]: IGraphicsDevice.hpp's TgsiBindings) has bindings of each stage's own (stage_slot):
//   kBindingConstants                     a uniform buffer: CONST[n] is its 16-byte row n (the push constants, then from row 256 the samplers' comparison rows)
//   kBindingTexture + u, u < 16           SVIEW[u], a sampled image (texture unit u)
//   kBindingSampler + u                   SAMP[u]
//   binding_texel(stage, k)               SVIEW[16 + k], a uniform texel buffer
//   binding_storage(stage, k)             BUFFER[k], a storage buffer of 32-bit words
//   binding_image(stage, k)               IMAGE[k], a storage image (a storage texel buffer for the BUFFER target)
// Stage interface: vertex attribute n is location n; GENERIC[i] is location i; PATCH[j] is location patch_base + j; the colour outputs COLOR[i] are location i
// (COLOR[1] is location 0 index 1 for dual-source blending); positions, clip distances, point sizes are a gl_PerVertex block, the same in every stage.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics3D/Venus/TgsiText.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/FixedString.hpp"

namespace graphics::tgsi {

constexpr uint32_t kMaxSviews = 32, kMaxUnits = 16, kMaxTexelViews = 16, kMaxStorage = 64, kMaxImages = 32, kMaxConstantRows = 4096, kMaxStages = 5;
constexpr uint32_t kBindingConstants = 0;
constexpr uint32_t kBindingTexture = 1;      // .. 16
constexpr uint32_t kBindingSampler = 17;     // .. 32
constexpr uint32_t binding_texel(uint32_t stage, uint32_t k) { return 40 + stage * kMaxTexelViews + k; }      // 40 .. 119
constexpr uint32_t binding_storage(uint32_t stage, uint32_t k) { return 120 + stage * kMaxStorage + k; }      // 120 .. 439
constexpr uint32_t binding_image(uint32_t stage, uint32_t k) { return 440 + stage * kMaxImages + k; }         // 440 .. 599
constexpr uint32_t kMaxStreamOutput = 64;

// The type a texture, an image or an attribute holds (what the shader's sampled type must be: Vulkan wants it to match the format).
enum class ValueType : uint8_t { Float, Sint, Uint };

// One stream output declaration (Direct3D's SO entry, as VirglDevice reads them): output register `reg` components first .. first + count - 1 go to buffer
// `buffer` at dword `offset` of each record; the buffer's record is `stride_dwords[buffer]` dwords.
struct StreamOutputEntry {
  uint32_t reg = 0;
  uint8_t first = 0, count = 0, buffer = 0, stream = 0;
  uint32_t offset = 0;   // dwords
};

struct CompileOptions {
  // Which of the pipeline's stages this is (0 .. 4: the bindings of its storage, binding_storage and the others).
  uint32_t stage_slot = 0;
  // The constant buffer's rows (the declared CONST range is checked against it).
  uint32_t constant_rows = 272;
  // SVIEW / IMAGE / vertex attribute / colour output value types (from what is bound or the pipeline's formats).
  ValueType sview_type[kMaxSviews] = {};
  ValueType image_type[kMaxImages] = {};
  ValueType attribute_type[16] = {};
  // Vertex attributes whose fourth component the shader sees as 1 whatever the buffer holds (a format of three components read through a four-component one, Bgrx8).
  uint16_t attribute_one_w = 0;
  ValueType color_type[8] = {};
  // Image formats when the declaration has none (the ImageFormat of what is bound; None: the format-less access).
  ImageFormat image_format[kMaxImages] = {};
  // The last stage before the rasterizer writes PointSize = 1.0 (for point primitives, as Vulkan needs a size; TGSI producers never write one).
  bool write_point_size = false;
  // The clip distances of the gl_PerVertex block (0 or 8, the same in every stage of the pipeline); clip_mask: those that clip (the others are written 1.0).
  uint32_t clip_distances = 0;
  uint8_t clip_mask = 0xFF;
  // A fragment shader with dual-source blending: COLOR[1] is location 0, index 1.
  bool dual_source = false;
  // FS_COLOR0_WRITES_ALL_CBUFS: colour 0 goes to this many targets.
  uint32_t color_targets = 1;
  // PATCH[j] is location patch_base + j (after the per-vertex locations of the pipeline's boundary).
  uint32_t patch_base = 16;
  // Tessellation control: the patch's input control points (the input arrays are this long; 0: 32, the most).
  uint32_t patch_vertices = 0;
  // Compute: the group-shared memory MEMORY[0] (bytes).
  uint32_t shared_bytes = 0;
  // Stream output (the last stage before the rasterizer): entries and each buffer's record size.
  const StreamOutputEntry* stream_output = nullptr;
  uint32_t stream_output_count = 0;
  uint32_t stream_output_stride[4] = {};
  // Geometry shader: stream numbers 0 and `swap_stream` are exchanged (the rasterized stream must be stream 0, as on virgl); 0: none.
  uint32_t swap_stream = 0;
  // Tessellation evaluation: the tessellator's settings instead of the text's TES_VERTEX_ORDER_CW / TES_POINT_MODE / TES_SPACING (0 fractional odd, 1 even, 2 equal).
  bool tess_override = false, tess_cw = false, tess_points = false;
  uint32_t tess_spacing = 2;
  // TGSI's DDY runs up the image's rows (the GL translator's images are GL's rows bottom first): DDY is the negated Vulkan dPdy. The Direct3D layer's run down.
  bool ddy_up = false;
  // The device has shaderOutputLayer / shaderOutputViewportIndex (a layer or viewport from a vertex or domain shader).
  bool output_layer_any_stage = false;
  // The constant buffer's system row (0: none): x the view instance (SV VIEWINDEX), y and z what view instancing adds to the primitives' viewport and layer.
  uint32_t system_row = 0;
  // The last stage before the rasterizer adds the system row's y and z to the viewport index and the layer it writes (and writes them when the text does not).
  bool view_offsets = false;
  // Texture units whose sampler takes coordinates in texels (bit u; Direct3D 12's non-normalized samplers): the 2D coordinates of a lookup are divided by the
  // size of the view's first level, and the lookups read that level (the sampler's levels are 0 .. 0).
  uint32_t unnormalized_units = 0;
};

struct CompileResult {
  explicit CompileResult(allocators::IAllocator& a) : allocator(a), words(a) {}
  allocators::IAllocator& allocator;
  List<uint32_t> words;
  uint32_t line = 0;                  // a failure's TGSI line
  FixedString<96> error;
  // What the shader uses (for the pipeline's descriptor layout).
  uint32_t sview_mask = 0, sampler_mask = 0;   // bit u: SVIEW[u] / SAMP[u]
  uint64_t storage_mask = 0, image_mask = 0;   // bit k: BUFFER[k] / IMAGE[k]
  bool constants = false;
  bool image_buffer[kMaxImages] = {};          // IMAGE[k] is a texel buffer
  bool sview_buffer[kMaxSviews] = {};          // SVIEW[u] is a texel buffer
  bool sview_multisampled[kMaxSviews] = {};
};

// Compiles `program` (parsed TGSI). False with result->error set when it uses something this compiler cannot express exactly.
[[nodiscard]] bool compile_spirv(const Program& program, const CompileOptions& options, CompileResult* result);

}  // namespace graphics::tgsi
