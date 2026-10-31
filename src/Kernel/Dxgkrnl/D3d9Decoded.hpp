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

// What a decoded Direct3D 9 shader (D3d9Program) is made of: the bytecode's opcode and register numbers and the instruction as D3d9Shader.cpp decodes it. Shared by the
// interpreter (D3d9Shader.cpp) and the TGSI emitter (D3d9Tgsi.cpp).
#pragma once
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"

namespace dxgk {

namespace d3d9 {

// --- Bytecode constants (d3d9types.h) ---------------------------------------------------------------------------------------------------------------------------

enum Opcode : uint16_t {
  kNop = 0, kMov, kAdd, kSub, kMad, kMul, kRcp, kRsq, kDp3, kDp4, kMin, kMax, kSlt, kSge, kExp, kLog, kLit, kDst, kLrp, kFrc, kM4x4, kM4x3, kM3x4, kM3x3, kM3x2,
  kCall, kCallNz, kLoop, kRet, kEndLoop, kLabel, kDcl, kPow, kCrs, kSgn, kAbs, kNrm, kSinCos, kRep, kEndRep, kIf, kIfc, kElse, kEndIf, kBreak, kBreakC, kMova, kDefB, kDefI,
  kTexKill = 65, kTexLd = 66, kDef = 81, kCmp = 88, kDp2Add = 90,
  kComment = 0xFFFE, kEnd = 0xFFFF,
  // Instructions of Shader Model 4 (DXBC, see D3d9Dxbc.cpp) that Direct3D 9 has no token for. Booleans are 1.0 / 0.0 here, not all-ones masks: the front end only takes shaders that use
  // them to select (movc, if, discard).
  kDiv = 0x1000, kSqrt, kSeq, kSne, kDp2, kFloor, kCeil, kTrunc, kRoundNe, kSin, kCos, kSelect, kIfNz, kIfZ, kDiscardNz, kDiscardZ,
  kDdx, kDdy, kDdxCoarse, kDdyCoarse,   // (derivatives over the 2 x 2 quad: fine takes the pair of the pixel's own row or column, coarse the quad's first)
  // Integer and bitwise instructions: the 32 bits of a register component are the integer (the interpreter keeps them in the float's bits), comparisons give all ones or zero.
  kIAdd = 0x1100, kINeg, kIMul, kIMad, kIMin, kIMax, kUMin, kUMax, kUMul, kUMad, kUDiv, kURem, kIShl, kIShr, kUShr, kAnd, kOr, kXor, kNot, kFtoI, kFtoU, kItoF, kUtoF, kIEq, kINe, kILt, kIGe, kULt, kUGe, kUMulHi, kIMulHi,   // (the high word of a 32 x 32 product)
  kIntegerFirst = kIAdd, kIntegerLast = kIMulHi,
  // Loops (Shader Model 4): `target` of a loop is the instruction after its endloop, of an endloop the one after its loop; break leaves, continue goes to the endloop.
  // Texture access of Shader Model 4: ld (src[0] the integer coordinates, x y in the first two components and the level in the last; src[1] the unit), sample_l (src[0] the
  // coordinate, src[1] the unit, src[2] the level), resinfo (src[0] the level, src[1] the unit; control: 0 floats, 1 reciprocals, 2 unsigned integers).
  kTexFetch = 0x1300, kTexLod, kTexSize, kTexFetchMs, kTexSamples, kTexGather, kTexGrad, kTexBias, kTexQueryLod, kTexCompare, kRetEarly,   // (kRetEarly: a ret inside an if or a loop ends the program there)
 
  // Double precision (Shader Model 5): a double is two components of a register, the low word first (a pair at x y or z w; the operand's swizzle positions 2k and 2k + 1 hold the pair k, and the
  // result goes to the destination's pair k - or, for the comparisons and the conversions to a 32-bit value, to its component k). The instructions to kDoubleLast read doubles.
  kDAdd, kDMul, kDDiv, kDFma, kDRcp, kDMin, kDMax, kDMov, kDMovc, kDEq, kDGe, kDLt, kDNe, kDToF, kDToI, kDToU, kFToD, kIToD, kUToD,
  kDoubleFirst = kDAdd, kDoubleLast = kDToU,
    // (kTexCompare: sample_c / sample_c_lz, src[2] the reference: the comparison of the four texels of the footprint, filtered bilinearly; passes where reference <= texel)
    // (kTexQueryLod: the lod instruction, x the level clamped to the texture's, y not)
    // (kTexFetchMs: ld_ms, src[2] the sample; kTexGather: gather4, src[0] the coordinate, src[1] the unit, control the component (0..3) of the four texels returned; kTexSamples: sampleinfo, a constant: control the count, tex_swizzle[0] 1 for an unsigned result)
  // Storage buffers (compute shaders, Shader Model 5): kBufLoad (dst; src[0] the byte address or the structure index, src[1] the byte offset in the structure; tex_swizzle picks the loaded
  // words), kBufStore (dst.mask: how many words; src[2] the value), kBufAtomic / kBufImmAtomic (src[0] the address, src[1] the operand; the second also writes the old value to dst). The
  // buffer is `uav`; `stride` is 0 for a raw buffer (byte address) and the structure size for a structured one; an atomic's kind is `control` (AtomicKind).
  kBufLoad = 0x1400, kBufStore, kBufAtomic, kBufImmAtomic,
  // Geometry shaders: emit appends the output registers' vertex to the strip, cut ends the strip.
  kGsEmit = 0x1500, kGsCut,
  kBarrier = 0x1600,   // sync_g..: the thread waits until every thread of its group is here (a group runner switches threads there)
  kLoopBegin = 0x1200, kLoopEnd, kBreak0, kBreakNz, kBreakZ, kContinue0, kContinueNz, kContinueZ,
};

enum RegType : uint8_t { kPatchOut = 21 /* a hull shader's patch constant output register (fork and join phases) */, kPatchIn = 22 /* a domain shader's patch constant input register */, kTemp = 0, kInput = 1, kConst = 2, kAddrTex = 3, kRastOut = 4, kAttrOut = 5, kTexCrdOut = 6, kColorOut = 8, kDepthOut = 9, kSampler = 10, kConstBool = 14,
               kSysValue = 20 /* (not in a token stream: a Shader Model 4 system value; number: kSysPosition ...) */ };
enum SysValue : uint8_t { kSysPosition = 0, kSysFrontFacing, kSysPrimitiveId, kSysVertexId, kSysInstanceId, kSysDispatchThreadId, kSysGroupId, kSysGroupThreadId, kSysGroupIndex, kSysOutputControlPointId, kSysForkInstanceId, kSysJoinInstanceId, kSysDomainLocation, kSysGsInstanceId, kSysValueCount };
enum AtomicKind : uint8_t { kAtomicAdd = 0, kAtomicAnd, kAtomicOr, kAtomicXor, kAtomicIMax, kAtomicIMin, kAtomicUMax, kAtomicUMin, kAtomicExchange };
enum Usage : uint8_t { kUsagePosition = 0, kUsageTexCoord = 5, kUsageColor = 10 };
enum Modifier : uint8_t { kModNone, kModNeg, kModBias, kModBiasNeg, kModSign, kModSignNeg, kModComp, kModX2, kModX2Neg, kModDz, kModDw, kModAbs, kModAbsNeg, kModNot };

constexpr uint32_t kMaxTemps = 32, kMaxInputs = 16, kMaxOutputs = 16;
constexpr int8_t kSlotNone = -1, kSlotPosition = -2;

}  // namespace d3d9

using namespace d3d9;

// Does the Direct3D 9 instruction write the register named by its first parameter?
bool d3d9_op_writes_register(uint16_t op);
// How many parameter tokens an instruction of a Shader Model 1 stream has (they carry no length): -1 for an opcode this decoder does not know.
int d3d9_op_parameter_tokens(uint16_t op);

// One decoded instruction: sources and destination with their register slot already worked out, so running does not look anything up.
struct D3d9Program::Instruction {
  struct Src {
    uint8_t type = 0;
    uint16_t number = 0;
    uint8_t vertex = 0;        // a geometry, hull or domain shader's input: which vertex or control point (v[vertex][register])
    bool vertex_from_id = false;   // ... the one the hull shader's control point phase is making (vOutputControlPointID)
    int8_t slot = kSlotNone;   // inputs: the attribute location / varying
    uint8_t swizzle[4] = {0, 1, 2, 3};
    uint8_t modifier = kModNone;
    bool relative = false;
    uint8_t relative_component = 0;
    int8_t relative_temp = -1;     // the temporary whose component (an integer) is added to the register number; -1: a0 (Direct3D 9)
    uint16_t relative_limit = 0;   // with a relative address: the register number it must stay below (a constant buffer's end); 0: no such check
  };
  struct Dst {
    uint8_t type = 0;
    uint16_t number = 0;
    int8_t slot = kSlotNone;   // outputs: kSlotPosition, a varying, or the colour (0)
    uint8_t mask = 0xF;
    bool saturate = false;
    int8_t shift = 0;
  };
  uint16_t op = kNop;
  uint8_t control = 0;
  bool mask_result = false;   // seq / sne / slt / sge of a Shader Model 4 shader: all ones for true (not 1.0), as its integer instructions and movc read them
  bool vector = false;   // rsq / exp / log of a Shader Model 4 shader: per component, not from x
  uint8_t uav = 0;       // kBufLoad ..: the storage buffer slot
  uint32_t stride = 0;
  uint8_t tex_swizzle[4] = {0, 1, 2, 3};   // texld: which texel component each result component takes
  Dst dst;
  Src src[4];
  uint8_t src_count = 0;
  uint32_t target = 0;   // if: where to go when the condition fails; else: where to go to leave the branch
};

}  // namespace dxgk
