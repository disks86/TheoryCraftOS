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

// Turns a vertex and a fragment shader (GLSL ES 1.00/3.00 modules from Glsl.cpp), and the geometry and tessellation stages between them (desktop GLSL: see
// translate_program_to_tgsi), into TGSI text for a 3D device that runs shaders itself (virgl).
//
// What is handled: float, integer and boolean scalars and vectors and float matrices (integers and booleans are kept as float values, exact below 2^24); arithmetic,
// comparisons, `if`, `for` / `while` / `do` loops with break and continue, user functions (inlined at each call, with in / out / inout parameters and returns
// anywhere), local arrays and uniform arrays indexed by constants or variables, discard, the common built-in functions; uniforms, attributes and (also flat)
// varyings of those types, gl_FragCoord and gl_FrontFacing; and in a fragment shader texture2D()/texture() of a uniform sampler2D. Anything else (bitwise
// integer operators, switch, other textures, derivatives, uniform blocks, structures in GLSL arrays, recursion ...) makes the translation fail and the draw then
// runs on the CPU rasterizer.
//
// The output follows the conventions of graphics::ShaderModuleDesc::tgsi: attributes are IN[0..], varyings GENERIC[0..], and the uniforms of both stages share one
// block of CONST rows (at most TgsiLimits::const_rows, the device's push constants). Clip space is converted from GL's (-w <= z <= w) to the device's (0 <= z <= w) at
// the end of the vertex shader; x and y are left alone because the device renders GL's bottom-up window rows as its own rows.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "WebGl/Shader.hpp"

namespace webgl {

// The most any device takes (array sizes); what one device takes is a TgsiLimits.
constexpr uint32_t kTgsiMaxConstRows = 256;
constexpr uint32_t kTgsiMaxAttributes = 16;
constexpr uint32_t kTgsiMaxVaryings = 16;
constexpr uint32_t kTgsiMaxSamplers = 16;

// What the device the shaders are for takes: CONST rows (its push constants), vertex inputs, varyings and fragment samplers. The defaults are what every device has.
struct TgsiLimits {
  uint32_t const_rows = 8, attributes = 4, varyings = 4, samplers = 2;
};

// A varying the vertex shader writes and the fragment shader reads: their private slots and the number of floats.
struct TgsiLink {
  uint32_t vs_slot, fs_slot, count;
  bool saturate = false;   // written clamped to [0, 1] (a clamped vertex colour)
  bool flat = false;       // not interpolated: the provoking (last) vertex's value
};

// How the values of a uniform or attribute become the floats the device's registers hold (integers and booleans are kept as float values on the device).
enum : uint8_t { kTgsiAsIs = 0, kTgsiFromInt = 1, kTgsiFromUint = 2, kTgsiFromBool = 3 };

// Where a CONST row gets its values: `comps` floats from `slot` of the shared (uniform) region of a stage.
struct TgsiConstant {
  uint8_t stage;   // 0 vertex, 1 fragment, 2 geometry, 3 tessellation control, 4 evaluation (ProgramObj::stage_shared's numbers), kTgsiPatchLevels
  uint16_t row;
  uint8_t comps;
  uint32_t slot;
  uint8_t convert = kTgsiAsIs;   // int, uint or bool bits made float values
};

// Which vertex input a program attribute is: the private slot of the vertex shader's global (GLSL) or the shader location (WGSL), and its component count (the
// input is always a vec4 on the device).
// TgsiAttribute::slot of the input a program without attributes is given (the device draws with one at least): it reads nothing, and zeros are sent.
constexpr uint32_t kTgsiNoAttribute = 0xFFFFFFFFu;

struct TgsiAttribute {
  uint32_t slot;   // (the key)
  uint8_t comps;
  uint8_t convert = kTgsiAsIs;   // an int / uint input: read as integers, sent as their float values
};

// What the fragment shader samples: SAMP[i] is samplers[i]. GLSL: a sampler2D uniform, and the texture unit it names is the value in `slot` of the fragment shader's
// shared region. WGSL: a texture and a sampler binding, the handles in `slot` (texture) and `sampler_slot` (sampler) of the region.
struct TgsiSampler {
  uint32_t slot;
  uint32_t sampler_slot;
  bool wgsl;
};

// TgsiConstant::stage of the rows that hold the default tessellation levels (glPatchParameterfv) a program without a control stage uses: slots 0 .. 3 the outer levels,
// 4 and 5 the inner ones.
constexpr uint8_t kTgsiPatchLevels = 5;
// ... and of the user clip planes (glClipPlane, eye space) a program with clip distances compares its gl_ClipVertex or gl_Position with: slot 4 i is plane i.
constexpr uint8_t kTgsiClipPlanes = 6;

// The stages of a desktop GL program between its vertex and fragment shaders (nullptr: not there), and the patch size its draws use (GL_PATCH_VERTICES: what the
// control stage takes, or the evaluation stage without one).
struct TgsiStages {
  const shader::Module* gs = nullptr;
  const shader::Module* tcs = nullptr;
  const shader::Module* tes = nullptr;
  uint32_t patch_vertices = 0;
  // The last stage before the rasterizer puts out eight clip distances (CLIPDIST[0], CLIPDIST[1]), as the CPU path has them: its gl_ClipDistance, or the user planes
  // against its gl_ClipVertex, or (compatibility) its gl_Position; +1 (inside) when it has none of them.
  bool clip = false;
};

struct TgsiProgram {
  explicit TgsiProgram(allocators::IAllocator& a) : vs(a), fs(a), gs(a), tcs(a), tes(a), constants(a), attributes(a), samplers(a) {}
  String vs, fs;
  String gs, tcs, tes;              // the other stages (empty: not there); a program with an evaluation stage always has a control one (made when the GLSL has none)
  List<TgsiConstant> constants;
  List<TgsiAttribute> attributes;   // IN[i] of the vertex shader is attributes[i]
  List<TgsiSampler> samplers;       // SAMP[i] of the fragment shader (WGSL: textureSample)
  uint32_t const_rows = 0;
  uint32_t varyings = 0;
  bool uses_frag_coord = false;   // the fragment shader reads gl_FragCoord (in the device's pixels: the target must not be supersampled)
  // What the device must know of the other stages, as graphics::GeometryPrimitive, GeometryTopology, TessDomain, TessPartitioning and TessOutput numbers.
  uint8_t gs_input = 2, gs_output = 2;
  uint32_t gs_max_vertices = 0, gs_invocations = 1;
  uint32_t patch_vertices = 0, tcs_vertices = 0;
  uint8_t tess_domain = 1, tess_partitioning = 0, tess_output = 1;
};

// False when the pair cannot run on the device (see the file comment); `why` (optional) then says what stopped it.
bool translate_to_tgsi(allocators::IAllocator& a, const shader::Module& vs, const shader::Module& fs, ArrayView<const TgsiLink> links, TgsiProgram* out,
                       const char** why = nullptr, const TgsiLimits& limits = TgsiLimits());

// The same for a program with geometry or tessellation stages (desktop GLSL 1.50 and later): `links` are then the varyings of the last stage before the rasterizer
// (geometry, else evaluation, else vertex: vs_slot is that stage's) that the fragment shader reads. The stages pass their varyings to each other as GENERIC (per
// vertex) and PATCH (per patch) registers, the positions as POSITION and the tessellation levels as TESSOUTER / TESSINNER; the clip space conversion is the last
// stage's. In the geometry stage EmitVertex() writes the outputs and EMITs, EndPrimitive() is ENDPRIM. A control stage's invocation writes its own control
// point's outputs at the end (an output of another invocation must not be read: such a shader is refused, as are reads of the patch outputs), and its patch outputs
// when it has written them. Not taken: several vertex streams, gl_Layer / gl_ViewportIndex, the built-in inputs and outputs other than gl_Position, arrays of
// arrays per vertex, and anything the vertex and fragment translation refuses.
bool translate_program_to_tgsi(allocators::IAllocator& a, const shader::Module& vs, const shader::Module& fs, const TgsiStages& stages, ArrayView<const TgsiLink> links,
                               TgsiProgram* out, const char** why = nullptr, const TgsiLimits& limits = TgsiLimits());

// The same for a WGSL render pipeline: the vertex and fragment entry points (function indexes in their modules). Attributes are keyed by @location; the vertex
// outputs with a @location become the varyings, the fragment shader's inputs are matched to them by location, and the vertex shader's @builtin(position) and the
// fragment shader's @location(0) vec4 are the only built-in output and color. textureSample of a texture_2d<f32> with a (non-comparison) sampler is translated like texture2D. Clip space is WebGPU's (0 <= z <= w) with y negated for the device.
bool translate_wgsl_to_tgsi(allocators::IAllocator& a, const shader::Module& vs, int32_t vs_fn, const shader::Module& fs, int32_t fs_fn, TgsiProgram* out,
                            const char** why = nullptr, const TgsiLimits& limits = TgsiLimits());

}  // namespace webgl
