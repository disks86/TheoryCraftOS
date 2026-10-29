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


// A compiler from TGSI (TgsiParse.hpp) to GCN3 machine code for the Polaris graphics pipeline: vertex shaders (the VS hardware stage) and pixel shaders. The code follows an ABI of
// this OS's own, since the OS programs the registers that feed it:
//
//   user SGPRs (both stages): s[0:1] the address of the constants (rows of four floats, row r at 16 * r), s[2:3] the address of the descriptor table (below).
//   vertex shader: v0 the vertex index; the attributes are fetched with buffer_load_format_xyzw through the V# at table offset 16 * i (attribute i = TGSI IN[i]); the exports are the
//   position (pos0) and one parameter (param n) per other output, in the order of the outputs.
//   pixel shader: s4 the primitive mask (M0 for the interpolation); v0, v1 the barycentric coordinates i and j (SPI_PS_INPUT_ENA with PERSP_CENTER only); input i is attribute i, in
//   the order of the inputs; the colour goes out as export mrt0 with the valid mask bit; a texture is the T# at table offset kImageOffset + 32 * i and the sampler the S# at kSamplerOffset + 16 * i.
//
// The TGSI registers live in vector registers, four per register (a temporary, an input, an output); constants in scalar registers loaded at the start. Each instruction is done per
// written component, with the operands legalized for the GFX8 encodings (one scalar operand per vector instruction, no literals in the 64 bit forms).
#pragma once

#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnIsa.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/TgsiParse.hpp"

namespace gcn {

namespace abi {
constexpr uint32_t kUserSgprs = 4;
constexpr uint32_t kMaxAttributes = 8, kVertexBufferBytes = 16;
constexpr uint32_t kImageOffset = kMaxAttributes * kVertexBufferBytes, kImageBytes = 32;
constexpr uint32_t kMaxSamplers = 8;
constexpr uint32_t kSamplerOffset = kImageOffset + kMaxSamplers * kImageBytes, kSamplerBytes = 16;
constexpr uint32_t kTableBytes = kSamplerOffset + kMaxSamplers * kSamplerBytes;
constexpr uint32_t kPsInputEnablePerspCenter = 1u << 1;          // SPI_PS_INPUT_ENA.PERSP_CENTER_ENA
}  // namespace abi

struct ShaderInfo {
  bool fragment = false;
  uint32_t vgprs = 0, sgprs = 0;               // the registers used (the counts the resource registers want)
  uint32_t user_sgprs = abi::kUserSgprs;
  uint32_t param_exports = 0;                  // vertex shader: the number of parameter exports
  uint32_t output_param[TgsiShader::kMaxOutputs] = {};      // vertex shader: the parameter an output is exported as (0xFF: not a parameter)
  uint32_t ps_inputs = 0;                      // pixel shader: the number of interpolated attributes
  uint32_t input_attribute[TgsiShader::kMaxInputs] = {};    // pixel shader: the attribute an input reads (0xFF: unused)
  uint32_t ps_input_enable = 0;                // pixel shader: SPI_PS_INPUT_ENA
  uint32_t attributes_used = 0;                // vertex shader: bit i when attribute i is fetched
  uint32_t samplers_used = 0;                  // pixel shader: bit i when sampler i is used
  uint32_t color_outputs = 1;                  // pixel shader: the colours it exports, 1 (COLOR[0]) or 2 (COLOR[1] too: the second source of dual source blending)
  uint32_t const_rows = 0;                     // the constant rows read (the highest + 1)
  bool kills = false;
  uint32_t code_dwords = 0;
};

// Compiles; false with a static message in *error. `code` receives the instructions (ending in s_endpgm).
bool compile_tgsi(const TgsiShader& shader, Code& code, ShaderInfo* info, const char** error);

}  // namespace gcn
