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

// Turns a vertex and a fragment shader (GLSL ES 1.00/3.00 modules from Glsl.cpp) into TGSI text for a 3D device that runs shaders itself (virgl).
//
// Only the part of the language that maps straight onto a GPU without a real compiler is handled: float scalars, vectors and matrices; arithmetic, comparisons,
// `if`; the common built-in functions; uniforms, attributes and varyings of those types; and in a fragment shader texture2D()/texture() of a uniform sampler2D. Anything else (loops, integers, user functions, other textures, derivatives,
// uniform blocks, flat varyings, gl_FragCoord, discard in a loop ...) makes the translation fail and the draw then runs on the CPU rasterizer.
//
// The output follows the conventions of graphics::ShaderModuleDesc::tgsi: attributes are IN[0..], varyings GENERIC[0..], and the uniforms of both stages share one
// block of CONST rows (at most kTgsiMaxConstRows, the device's push constants). Clip space is converted from GL's (-w <= z <= w) to the device's (0 <= z <= w) at
// the end of the vertex shader; x and y are left alone because the device renders GL's bottom-up window rows as its own rows.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "WebGl/Shader.hpp"

namespace webgl {

constexpr uint32_t kTgsiMaxConstRows = 8;
constexpr uint32_t kTgsiMaxAttributes = 4;
constexpr uint32_t kTgsiMaxVaryings = 4;
constexpr uint32_t kTgsiMaxSamplers = 2;

// A varying the vertex shader writes and the fragment shader reads: their private slots and the number of floats.
struct TgsiLink {
  uint32_t vs_slot, fs_slot, count;
};

// Where a CONST row gets its values: `comps` floats from `slot` of the shared (uniform) region of a stage.
struct TgsiConstant {
  uint8_t stage;   // 0 vertex, 1 fragment
  uint8_t row;
  uint8_t comps;
  uint32_t slot;
};

// Which vertex input a program attribute is: the private slot of the vertex shader's global (GLSL) or the shader location (WGSL), and its component count (the
// input is always a vec4 on the device).
struct TgsiAttribute {
  uint32_t slot;   // (the key)
  uint8_t comps;
};

// What the fragment shader samples: SAMP[i] is samplers[i]. GLSL: a sampler2D uniform, and the texture unit it names is the value in `slot` of the fragment shader's
// shared region. WGSL: a texture and a sampler binding, the handles in `slot` (texture) and `sampler_slot` (sampler) of the region.
struct TgsiSampler {
  uint32_t slot;
  uint32_t sampler_slot;
  bool wgsl;
};

struct TgsiProgram {
  explicit TgsiProgram(allocators::IAllocator& a) : vs(a), fs(a), constants(a), attributes(a), samplers(a) {}
  String vs, fs;
  List<TgsiConstant> constants;
  List<TgsiAttribute> attributes;   // IN[i] of the vertex shader is attributes[i]
  List<TgsiSampler> samplers;       // SAMP[i] of the fragment shader (WGSL: textureSample)
  uint32_t const_rows = 0;
  uint32_t varyings = 0;
};

// False when the pair cannot run on the device (see the file comment); `why` (optional) then says what stopped it.
bool translate_to_tgsi(allocators::IAllocator& a, const shader::Module& vs, const shader::Module& fs, ArrayView<const TgsiLink> links, TgsiProgram* out,
                       const char** why = nullptr);

// The same for a WGSL render pipeline: the vertex and fragment entry points (function indexes in their modules). Attributes are keyed by @location; the vertex
// outputs with a @location become the varyings, the fragment shader's inputs are matched to them by location, and the vertex shader's @builtin(position) and the
// fragment shader's @location(0) vec4 are the only built-in output and color. textureSample of a texture_2d<f32> with a (non-comparison) sampler is translated like texture2D. Clip space is WebGPU's (0 <= z <= w) with y negated for the device.
bool translate_wgsl_to_tgsi(allocators::IAllocator& a, const shader::Module& vs, int32_t vs_fn, const shader::Module& fs, int32_t fs_fn, TgsiProgram* out,
                            const char** why = nullptr);

}  // namespace webgl
