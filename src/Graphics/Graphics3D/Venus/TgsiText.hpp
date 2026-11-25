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

// TGSI text read into a program (the shader language every hardware path of the kernel writes: the Direct3D layer's D3d9Tgsi.cpp, the GLSL / WGSL translator
// src/WebGl/TgsiTranslator.cpp, the built-in shaders and the devices' own small shaders). This is the reader the Venus device's SPIR-V compiler (TgsiSpirv.hpp)
// works from; it reads the whole language those producers write, the way Gallium's tgsi_text.c does:
//
//   VERT | FRAG | GEOM | TESS_CTRL | TESS_EVAL | COMP
//   PROPERTY <NAME> <value>...
//   DCL <FILE>[a] | [a..b] | [][a] | [][a..b] | [a][b..c] [, <semantic>[<index>]] [, <interpolation>] [, <target>, <return type> | <format>, WR] [, ARRAY(n)] [, SHARED]
//   IMM[n] FLT32 | UINT32 | INT32 | FLT64 {v, v, v, v}
//   [n:] <OP>[_SAT] [dst][, src]... [, <texture target>][, <offset or format>]...
//   END
//
// An operand is [-][|]FILE[index][index][(array)][.swizzle][|], where an index is a number, ADDR[a].c, ADDR[a].c+n, TEMP[t].c+n (any register file) or n+ADDR[a].c.
// A swizzle of one to four letters repeats its last letter. Registers are untyped 32-bit words: an immediate keeps the bits its text stands for, whatever its type word.
// The reader takes what the producers write and nothing it cannot say exactly; anything else is an error with its line.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/StringView.hpp"

namespace graphics::tgsi {

enum class Stage : uint8_t { Vertex, Fragment, Geometry, TessControl, TessEval, Compute };

enum class File : uint8_t { None, Temp, In, Out, Const, Imm, Sv, Samp, Sview, Addr, Image, Buffer, Memory };

// Opcodes (TGSI's names; the text table is in TgsiText.cpp).
enum class Op : uint8_t {
  // float
  Mov, Add, Sub, Mul, Mad, Fma, Div, Lrp, Min, Max, Abs, Ssg, Frc, Flr, Ceil, Trunc, Round, Dp2, Dp3, Dp4, Dph, Dst, Lit, Rcp, Rsq, Sqrt, Ex2, Lg2, Exp, Log, Pow,
  Sin, Cos, Xpd, Cmp, Slt, Sge, Seq, Sne, Sgt, Sle, Fslt, Fsge, Fseq, Fsne, Ddx, Ddy, DdxFine, DdyFine, Kill, KillIf,
  // integer
  Uadd, Ineg, Iabs, Isgn, Umul, UmulHi, ImulHi, Umad, Imin, Imax, Umin, Umax, Udiv, Idiv, Umod, Mod, And, Or, Xor, Not, Shl, Ishr, Ushr, F2i, F2u, I2f, U2f,
  Useq, Usne, Islt, Isge, Uslt, Usge, Ucmp,
  // double (register pairs)
  Dadd, Dmul, Ddiv, Dfma, Dmad, Drcp, Dsqrt, Drsq, Dmin, Dmax, Dmov, Dneg, Dabs, Dseq, Dsge, Dslt, Dsne, D2f, D2i, D2u, F2d, I2d, U2d,
  // address
  Arl, Arr, Uarl,
  // textures
  Tex, Txp, Txb, Txd, Txl, Txl2, Txb2, Tex2, Txf, Txq, Tg4, Lodq,
  // memory
  Load, Store, Resq, AtomUadd, AtomAnd, AtomOr, AtomXor, AtomImax, AtomImin, AtomUmax, AtomUmin, AtomXchg, AtomCas,
  // control
  If, Uif, Else, EndIf, BgnLoop, EndLoop, Brk, Cont, Ret, End, Emit, EndPrim, Membar, Barrier, Nop,
  kCount,
};

// What an opcode's operands are: destinations, then sources.
struct OpInfo {
  const char* name;
  uint8_t dsts;
  uint8_t srcs;   // the most sources (texture and memory ops take the target and format words besides)
};
const OpInfo& op_info(Op op);

enum class Target : uint8_t { None, Buffer, T1D, T2D, T3D, Cube, Rect, T1DArray, T2DArray, CubeArray, T2DMsaa, T2DArrayMsaa, Shadow1D, Shadow2D, ShadowRect,
                              Shadow1DArray, Shadow2DArray, ShadowCube, ShadowCubeArray };

enum class ReturnType : uint8_t { Float, Unorm, Snorm, Uint, Sint };

enum class Semantic : uint8_t { None, Position, Color, BColor, Fog, Psize, Generic, Normal, Face, EdgeFlag, PrimId, InstanceId, VertexId, StencilRef, CullDist, ClipDist,
                                ClipVertex, GridSize, BlockId, BlockSize, ThreadId, TexCoord, Pcoord, ViewportIndex, Layer, SampleId, SamplePos, SampleMask,
                                InvocationId, VertexIdNoBase, BaseVertex, PrimitiveId, TessCoord, VerticesIn, Patch, TessOuter, TessInner, VertexOut, ViewportMask,
                                HelperInvocation, BaseInstance, DrawId, WorkDim, SubgroupSize, SubgroupInvocation };

enum class Interpolation : uint8_t { Default, Constant, Linear, Perspective, Color };
enum class InterpolationLocation : uint8_t { Center, Centroid, Sample };

// An image format of an IMAGE declaration or a LOAD / STORE / atomic (Gallium's PIPE_FORMAT_* name: the ones the producers write).
enum class ImageFormat : uint8_t { None, R8G8B8A8Unorm, R8G8B8A8Snorm, R8G8B8A8Uint, R8G8B8A8Sint, B8G8R8A8Unorm, R16G16B16A16Float, R16G16B16A16Unorm, R16G16B16A16Snorm,
                                   R16G16B16A16Uint, R16G16B16A16Sint, R32G32B32A32Float, R32G32B32A32Uint, R32G32B32A32Sint, R32G32Float, R32G32Uint, R32G32Sint,
                                   R16G16Float, R16G16Unorm, R16G16Snorm, R16G16Uint, R16G16Sint, R8G8Unorm, R8G8Snorm, R8G8Uint, R8G8Sint, R32Float, R32Uint, R32Sint,
                                   R16Float, R16Unorm, R16Snorm, R16Uint, R16Sint, R8Unorm, R8Snorm, R8Uint, R8Sint, R10G10B10A2Unorm, R10G10B10A2Uint, R11G11B10Float };
ImageFormat image_format_by_name(StringView name);

// A register index: `value`, plus the register named by `file`[`index`].`component` when indirect.
struct Index {
  int32_t value = 0;
  bool indirect = false;
  File file = File::None;
  uint32_t index = 0;
  uint8_t component = 0;
};

struct Operand {
  File file = File::None;
  bool two_dimensional = false;   // FILE[a][b]: dimension = a (a vertex, a constant buffer), index = b
  Index dimension;
  Index index;
  uint32_t array = 0;             // (n): the array the index is inside (0: none)
  uint8_t swizzle[4] = {0, 1, 2, 3};
  uint8_t mask = 0xF;             // a destination's write mask
  bool negate = false, absolute = false;
  bool empty_dimension = false;   // FILE[][a] (declarations only)
};

struct Instruction {
  Op op = Op::Nop;
  bool saturate = false;
  uint8_t dst_count = 0, src_count = 0;
  Operand dst[2];
  Operand src[6];
  Target target = Target::None;       // a texture or image operation's target word
  ImageFormat format = ImageFormat::None;
  uint8_t offsets = 0;                // the sources after the target (texture offsets): src[src_count - offsets ..]
  uint32_t line = 0;
};

struct Declaration {
  File file = File::None;
  uint32_t first = 0, last = 0;
  bool two_dimensional = false;       // IN[][a] / OUT[][a]: one per vertex or control point
  uint32_t dimension = 0;             // FILE[d][a..b] (a constant buffer's number)
  Semantic semantic = Semantic::None;
  uint32_t semantic_index = 0;
  Interpolation interpolation = Interpolation::Default;
  InterpolationLocation location = InterpolationLocation::Center;
  Target target = Target::None;       // SVIEW / IMAGE
  ReturnType return_type = ReturnType::Float;
  ImageFormat format = ImageFormat::None;
  bool writable = false;              // IMAGE ..., WR
  bool shared = false;                // MEMORY ..., SHARED
  bool atomic = false;                // BUFFER ..., ATOMIC
  uint32_t array = 0;                 // TEMP / IN / OUT ..., ARRAY(n)
  uint32_t line = 0;
};

enum class ImmType : uint8_t { Flt32, Uint32, Int32, Flt64 };

struct Immediate {
  uint32_t bits[4] = {0, 0, 0, 0};    // (FLT64: two doubles, low word first)
  ImmType type = ImmType::Flt32;
};

enum class Property : uint8_t { GsInputPrimitive, GsOutputPrimitive, GsMaxOutputVertices, GsInvocations, FsColor0WritesAllCbufs, FsCoordOrigin, FsCoordPixelCenter,
                                 FsDepthLayout, FsEarlyDepthStencil, VsProhibitUcps, VsWindowSpacePosition, TcsVerticesOut, TesPrimMode, TesSpacing, TesVertexOrderCw,
                                 TesPointMode, CsFixedBlockWidth, CsFixedBlockHeight, CsFixedBlockDepth, NumClipdistEnabled, NumCulldistEnabled, NextShader, kCount };

class Program {
 public:
  explicit Program(allocators::IAllocator& allocator) : declarations(allocator), immediates(allocator), instructions(allocator) {}

  Stage stage = Stage::Vertex;
  List<Declaration> declarations;
  List<Immediate> immediates;
  List<Instruction> instructions;
  // PROPERTY values (primitive names are their Gallium numbers: POINTS 0, LINES 1, LINE_STRIP 3, TRIANGLES 4, TRIANGLE_STRIP 5, LINES_ADJACENCY 10,
  // TRIANGLES_ADJACENCY 12, QUADS 7); has_property says which were given.
  uint32_t property[static_cast<uint32_t>(Property::kCount)] = {};
  bool has_property[static_cast<uint32_t>(Property::kCount)] = {};

  uint32_t get(Property p) const { return property[static_cast<uint32_t>(p)]; }
  bool has(Property p) const { return has_property[static_cast<uint32_t>(p)]; }
};

struct ParseError {
  uint32_t line = 0;
  FixedString<96> message;
};

// Reads `text` into `out` (empty before). False with `error` set when the text is not TGSI this reader takes.
[[nodiscard]] bool parse(StringView text, Program* out, ParseError* error);

}  // namespace graphics::tgsi
