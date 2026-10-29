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


// TGSI text (the shader language of Gallium, and of this OS's GLSL / WGSL translator, src/WebGl/TgsiTranslator.hpp) read into a list of instructions the GCN compiler works on. The
// dialect is what the translator and the built-in shaders write: VERT / FRAG, PROPERTY lines (ignored), DCL of IN / OUT / CONST / TEMP / SAMP / SVIEW, IMM[n] FLT32 {a, b, c, d}, and the
// instructions "[n:] OP dst.mask, [-]file[i].swizzle ..." with an optional |abs| and a .SAT suffix, ending in END. Anything else is an error with a message and a line number.
#pragma once

#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/StringView.hpp"

namespace gcn {

enum class TgsiFile : uint8_t { None, Temp, In, Out, Const, Imm };

enum class TgsiOp : uint8_t {
  Mov, Add, Sub, Mul, Mad, Div, Lrp, Max, Min, Abs, Neg, Frc, Flr, Ceil, Trunc, Round, Ssg,
  Rcp, Rsq, Sqrt, Ex2, Lg2, Pow, Sin, Cos, Dp2, Dp3, Dp4, Xpd, Cmp,
  Seq, Sne, Sge, Slt, Sgt, Sle,
  Tex, Txb, Txl, Kill, KillIf, If, Else, EndIf, End,
};

struct TgsiSrc {
  TgsiFile file = TgsiFile::None;
  uint16_t index = 0;
  uint8_t swizzle[4] = {0, 1, 2, 3};      // 0 x, 1 y, 2 z, 3 w
  bool negate = false, absolute = false;
};

struct TgsiDst {
  TgsiFile file = TgsiFile::None;
  uint16_t index = 0;
  uint8_t mask = 0xF;                      // bit n: component n written
  bool saturate = false;
};

struct TgsiInst {
  TgsiOp op = TgsiOp::End;
  TgsiDst dst;
  TgsiSrc src[3];
  uint8_t src_count = 0;
  uint8_t sampler = 0;                     // Tex: the SAMP index
  uint32_t line = 0;
};

enum class TgsiSemantic : uint8_t { None, Position, Color, Generic, Fog, PointSize, Face, Instance, VertexId };

struct TgsiImmediate {
  float value[4] = {0, 0, 0, 0};
};

struct TgsiShader {
  explicit TgsiShader(allocators::IAllocator& allocator) : instructions(allocator), immediates(allocator) {}
  static constexpr uint32_t kMaxInputs = 16, kMaxOutputs = 16, kMaxConsts = 64, kMaxTemps = 128, kMaxSamplers = 16, kMaxImmediates = 128;
  bool fragment = false;
  uint32_t inputs = 0, outputs = 0, consts = 0, temps = 0, samplers = 0;           // how many (the highest index declared + 1)
  TgsiSemantic input_semantic[kMaxInputs] = {};
  uint8_t input_semantic_index[kMaxInputs] = {};
  TgsiSemantic output_semantic[kMaxOutputs] = {};
  uint8_t output_semantic_index[kMaxOutputs] = {};
  bool input_flat[kMaxInputs] = {};
  List<TgsiInst> instructions;
  List<TgsiImmediate> immediates;
};

// Parses `text`. False with `*error` (a static message) and `*line` on a problem; the shader is then not complete. Limits: the kMax constants above.
bool parse_tgsi(allocators::IAllocator& allocator, StringView text, TgsiShader* out, const char** error, uint32_t* line);

}  // namespace gcn
