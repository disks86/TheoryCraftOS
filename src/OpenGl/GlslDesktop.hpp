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

// Desktop GLSL (#version 110 ... 330, compatibility built-ins) to the GLSL ES dialects the shader front end compiles (src/WebGl/Glsl.cpp). Until the front end speaks desktop GLSL
// itself, programs written for desktop OpenGL go through here:
//  - the version: 110/120 (or none) -> 100, 130/140/150/330 -> 300 es, with highp default precisions; `core`/`compatibility` dropped;
//  - the compatibility built-ins become declared variables named tcos_* that the context fills: attributes (gl_Vertex, gl_Normal, gl_Color, gl_SecondaryColor, gl_FogCoord,
//    gl_MultiTexCoord0..7), uniforms (the matrices, gl_NormalScale, gl_DepthRange.near/far/diff are not), varyings shared by the two stages (gl_FrontColor = the fragment
//    shader's gl_Color, gl_FrontSecondaryColor, gl_TexCoord[], gl_FogFragCoord), and ftransform();
//  - gl_FragColor / gl_FragData in a 300 es translation become declared outputs.
//  - the uniform state (gl_LightSource[], gl_FrontMaterial, gl_Fog, gl_DepthRange, gl_ClipPlane[], the transposed and inverted matrices ...) as uniforms of GLSL's own structure
//    types, named tcos_*, which GlApi::load_compat_uniforms fills from the fixed-function state.
#pragma once
#include <stdint.h>
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace opengl {

enum CompatUse : uint32_t {
  kUseModelView = 1, kUseProjection = 2, kUseMvp = 4, kUseNormalMatrix = 8, kUseTextureMatrix = 16, kUseModelViewInverse = 32, kUseNormalScale = 64, kUseState = 128,
};

// `vertex`: the vertex stage (gl_Color is an attribute there and a varying in the fragment stage). False if the text is not something to translate (an ES shader: kept as it is).
bool translate_desktop_glsl(StringView source, bool vertex, String* out, uint32_t* uses);

}  // namespace opengl
