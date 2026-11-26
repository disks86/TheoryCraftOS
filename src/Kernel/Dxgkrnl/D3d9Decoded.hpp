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
  // Shader Model 5's bit operations, as Direct3D defines them: countbits; firstbit_hi / firstbit_shi the place of the first bit (set, or other than the sign) counted from the top,
  // firstbit_lo from the bottom, all ones when there is none; ubfe / ibfe (src[0] the width, src[1] the offset, src[2] the value: the low five bits of each count, the field ends at
  // bit 31) extract unsigned / sign extended; bfi (width, offset, insert, base) puts the insert's low bits at the offset into the base; bfrev reverses the bits.
  kCountBits, kFirstBitHi, kFirstBitLo, kFirstBitSHi, kUBfe, kIBfe, kBfi, kBfrev,
  kIntegerFirst = kIAdd, kIntegerLast = kBfrev,
  // Loops (Shader Model 4): `target` of a loop is the instruction after its endloop, of an endloop the one after its loop; break leaves, continue goes to the endloop.
  // Texture access of Shader Model 4: ld (src[0] the integer coordinates, x y in the first two components and the level in the last; src[1] the unit), sample_l (src[0] the
  // coordinate, src[1] the unit, src[2] the level), resinfo (src[0] the level, src[1] the unit; control: 0 floats, 1 reciprocals, 2 unsigned integers).
  kTexFetch = 0x1300, kTexLod, kTexSize, kTexFetchMs, kTexSamples, kTexGather, kTexGrad, kTexBias, kTexQueryLod, kTexCompare, kF32ToF16, kF16ToF32,   // (the half in the low 16 bits of the 32-bit result, and back)
  kRetEarly,   // (kRetEarly: a ret inside an if or a loop ends the program there)
 
  // Double precision (Shader Model 5): a double is two components of a register, the low word first (a pair at x y or z w; the operand's swizzle positions 2k and 2k + 1 hold the pair k, and the
  // result goes to the destination's pair k - or, for the comparisons and the conversions to a 32-bit value, to its component k). The instructions to kDoubleLast read doubles.
  kDAdd, kDMul, kDDiv, kDFma, kDRcp, kDMin, kDMax, kDMov, kDMovc, kDEq, kDGe, kDLt, kDNe, kDToF, kDToI, kDToU, kFToD, kIToD, kUToD,
  kDoubleFirst = kDAdd, kDoubleLast = kDToU,
    // (kTexCompare: sample_c / sample_c_lz, src[2] the reference: the sampler's comparison of the reference with the texels of the footprint, filtered as it filters (ITextureSet::sample_compare); control 1: sample_c_lz, level zero.
    //  A 2D texture, an array (the coordinate's z the layer) or a cube (the direction))
    // (kTexQueryLod: the lod instruction, x the level clamped to the texture's, y not)
    // (kTexFetchMs: ld_ms, src[0] the integer x y (and z, the layer of a Texture2DMSArray), src[2] the sample; kTexGather: gather4, src[0] the coordinate, src[1] the unit, control the
    //  component (0..3) of the four texels returned; kTexSamples: sampleinfo, (count, 0, 0, 0) swizzled by tex_swizzle - control 0: of the texture of unit src[0].number (`stride` the count
    //  it was declared with, 0 for none), 1: of the rasterizer -, `vector` set for an unsigned result)
  // Storage buffers (compute shaders, Shader Model 5): kBufLoad (dst; src[0] the byte address or the structure index, src[1] the byte offset in the structure; tex_swizzle picks the loaded
  // words), kBufStore (dst.mask: how many words; src[2] the value), kBufAtomic / kBufImmAtomic (src[0] the address, src[1] the operand; the second also writes the old value to dst). The
  // buffer is `uav`; `stride` is 0 for a raw buffer (byte address) and the structure size for a structured one; an atomic's kind is `control` (AtomicKind).
  kBufLoad = 0x1400, kBufStore, kBufAtomic, kBufImmAtomic,
  // The hidden counter of an append, consume or counter view (imm_atomic_alloc / imm_atomic_consume): `uav` is its storage slot (graphics::kCounterSlotBase + n), control 0 increments and
  // gives the value before, 1 decrements and gives the value after. kBufInfo (bufinfo): the size of a buffer view - the bytes of a raw one, the structures of a structured one (control 0:
  // `uav` a storage slot), the elements of a typed one (control 1: `uav` an image slot).
  kBufCounter, kBufInfo,
  // Storage images (typed unordered access views of textures): kImgLoad (dst; src[0] the integer coordinate as the image's dimension has it - x, x and layer, x y, x y and layer or slice;
  // tex_swizzle picks the components), kImgStore (src[0] the coordinate, src[1] the value), kImgAtomic / kImgImmAtomic (src[0] the coordinate, src[1] the operand, src[2] the compare
  // value of a compare-exchange; the second writes the old value to dst; `control` the AtomicKind), kImgSize (resinfo of a u#: dst, src[0] the level; control as kTexSize's). The image
  // is `uav` (its slot).
  kImgLoad = 0x1700, kImgStore, kImgAtomic, kImgImmAtomic, kImgSize,
  // Geometry shaders: emit appends the output registers' vertex to the strip, cut ends the strip.
  kGsEmit = 0x1500, kGsCut,
  kBarrier = 0x1600,   // sync_g..: the thread waits until every thread of its group is here (a group runner switches threads there)
  // Multisampling (Shader Model 4.1 / 5): kSamplePos (samplepos: dst; src[0] the sample index; control 0: of the texture of unit src[1].number - its standard pattern -, 1: of the
  // rasterizer - the pass's positions -; (x, y, 0, 0) in pixels from the centre, swizzled by tex_swizzle, (0, 0, 0, 0) for an index past the count). The evaluations of a pixel shader's input
  // src[0] (an input register: its varying, swizzle and modifier) at a point of the pixel: kEvalSample at the sample src[1] names, kEvalCentroid at the centroid, kEvalSnapped at the
  // offset src[1] gives (integers x y, sixteenths of a pixel from the centre, -8 .. 7: the low four bits, signed).
  kSamplePos = 0x1900, kEvalSample, kEvalCentroid, kEvalSnapped,
  kLoopBegin = 0x1200, kLoopEnd, kBreak0, kBreakNz, kBreakZ, kContinue0, kContinueNz, kContinueZ,
  // Wave operations (Shader Model 6, from the DXIL translator: D3d9Dxbc.cpp's private opcodes kDxbcWaveFirst ..). They read the lanes of the wave that are at the same instruction (see
  // WaveRunner in D3d9Shader.cpp): the active lanes, which are those lanes but a pixel shader's helpers, and for the quad operations the helpers too. `stride` is the site: the DXIL
  // instruction the operation came from (copies of one instruction - a return block the translator puts in each branch - are the same operation). A component of the result is made
  // from the same component of the sources (a 64-bit value: the pair 2k, 2k + 1); a condition is a source's x, true when not zero; a lane mask (ballot, match) is the result's x y z w,
  // lanes 0 - 31 in x. `control` of the typed ones: (WaveOp << 4) | WaveType.
  //   kWaveIsFirstLane (all ones in the first active lane), kWaveLaneIndex, kWaveLaneCount (the wave's lanes), kWaveAnyTrue / kWaveAllTrue (src[0] the condition; all ones or zero),
  //   kWaveBallot (the active lanes whose condition holds), kWaveAllEqual (all ones when every active lane has the value), kWaveReadLaneAt (src[1] the lane), kWaveReadFirst (the first
  //   active lane's value), kWaveActive / kWavePrefix (the reduction over the active lanes / over those before this one: control's WaveOp), kWaveCountBits / kWavePrefixCountBits (the
  //   active lanes / those before this one whose condition holds), kQuadReadLaneAt (src[1] the lane of the quad, 0 - 3), kQuadReadAcross (control: the lane of the quad it is
  //   exclusive-or'd with: 1 across X, 2 across Y, 3 the diagonal), kWaveMatch (the active lanes with this lane's value), kWaveMultiPrefix / kWaveMultiPrefixCountBits (as kWavePrefix /
  //   kWavePrefixCountBits over the lanes before this one that are in the mask src[1] - x y z w as kWaveBallot's), kQuadAny / kQuadAll (over the lanes of the quad).
  kWaveIsFirstLane = 0x1800, kWaveLaneIndex, kWaveLaneCount, kWaveAnyTrue, kWaveAllTrue, kWaveBallot, kWaveAllEqual, kWaveReadLaneAt, kWaveReadFirst, kWaveActive, kWavePrefix,
  kWaveCountBits, kWavePrefixCountBits, kQuadReadLaneAt, kQuadReadAcross, kWaveMatch, kWaveMultiPrefix, kWaveMultiPrefixCountBits, kQuadAny, kQuadAll,
  kWaveFirst = kWaveIsFirstLane, kWaveLast = kQuadAll,
};
static_assert(kSamplePos > kWaveLast || kEvalSnapped < kWaveFirst, "the multisampling operations and the wave operations share numbers");
static_assert(kIntegerLast < kLoopBegin && kIntegerLast < kWaveFirst && kIntegerLast < kSamplePos && kBfrev <= kIntegerLast,
              "the integer and bit operations run into the loop, wave or multisampling operations");
// The value type (low four bits of `control`) and the operation (high four) of a typed wave operation.
enum WaveType : uint8_t { kWaveF32 = 0, kWaveI32, kWaveU32, kWaveF64, kWaveI64, kWaveU64 };
enum WaveOp : uint8_t { kWaveSum = 0, kWaveProduct, kWaveMin, kWaveMax, kWaveAnd, kWaveOr, kWaveXor };

enum RegType : uint8_t { kPatchOut = 21 /* a hull shader's patch constant output register (fork and join phases) */, kPatchIn = 22 /* a domain shader's patch constant input register */, kTemp = 0, kInput = 1, kConst = 2, kAddrTex = 3, kRastOut = 4, kAttrOut = 5, kTexCrdOut = 6, kColorOut = 8, kDepthOut = 9, kSampler = 10, kConstBool = 14, kCoverageOut = 23 /* (a pixel shader's SV_Coverage output, oMask: the integer in x) */,
               kSysValue = 20 /* (not in a token stream: a Shader Model 4 system value; number: kSysPosition ...) */ };
enum SysValue : uint8_t { kSysPosition = 0, kSysFrontFacing, kSysPrimitiveId, kSysVertexId, kSysInstanceId, kSysDispatchThreadId, kSysGroupId, kSysGroupThreadId, kSysGroupIndex, kSysOutputControlPointId, kSysForkInstanceId, kSysJoinInstanceId, kSysDomainLocation, kSysGsInstanceId,
                        // (Shader Model 6.1+, from the DXIL translator: SV_ViewID of every stage, a pixel shader's SV_Barycentrics - perspective-correct and noperspective - and SV_ShadingRate)
                        kSysViewId, kSysBarycentrics, kSysBarycentricsLinear, kSysShadingRate, kSysHelperLane /* (IsHelperLane: all ones or zero) */,
                        // (a pixel shader's SV_SampleIndex - it then runs for each sample -, SV_Coverage and SV_InnerCoverage: integers)
                        kSysSampleIndex, kSysCoverage, kSysInnerCoverage, kSysValueCount };
enum AtomicKind : uint8_t { kAtomicAdd = 0, kAtomicAnd, kAtomicOr, kAtomicXor, kAtomicIMax, kAtomicIMin, kAtomicUMax, kAtomicUMin, kAtomicExchange, kAtomicCompareExchange };
enum Usage : uint8_t { kUsagePosition = 0, kUsageTexCoord = 5, kUsageColor = 10 };
enum Modifier : uint8_t { kModNone, kModNeg, kModBias, kModBiasNeg, kModSign, kModSignNeg, kModComp, kModX2, kModX2Neg, kModDz, kModDw, kModAbs, kModAbsNeg, kModNot };

// Temporaries: Direct3D 9 programs have 32 at most; a program made from DXBC (Direct3D 10 and later, the Vulkan driver's translated SPIR-V) up to kMaxTemps. A run sets to zero only
// as many as its program uses (D3d9Program::temp_count_), so the size of the file costs nothing per vertex or pixel.
constexpr uint32_t kMaxTemps = 256, kMaxInputs = 16, kMaxOutputs = 16;
constexpr uint32_t kMaxD3d9Temps = 32;   // r0 .. r31 of a Direct3D 9 token stream
constexpr int8_t kSlotNone = -1, kSlotPosition = -2, kSlotLayer = -3;   // (kSlotLayer: a geometry shader's SV_RenderTargetArrayIndex output, a kRastOut register)
constexpr int8_t kSlotViewport = -4;   // (SV_ViewportArrayIndex, the same way)
constexpr int8_t kSlotShadingRate = -5;   // (SV_ShadingRate of a vertex or geometry shader, the same way)

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
    int16_t vertex_temp = -1;      // ... (the DXBC front end, until emit gathers it) the one a temporary's component picks, `vertex` added: v[r0.x + 1][2]
    uint8_t vertex_component = 0;
    bool at_vertex = false;        // a pixel shader's input at vertex `vertex` of the primitive (GetAttributeAtVertex), not interpolated
    int8_t slot = kSlotNone;   // inputs: the attribute location / varying
    uint8_t swizzle[4] = {0, 1, 2, 3};
    uint8_t modifier = kModNone;
    bool relative = false;
    uint8_t relative_component = 0;
    int16_t relative_temp = -1;    // the temporary whose component (an integer) is added to the register number; -1: a0 (Direct3D 9)
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
  uint16_t uav = 0;      // kBufLoad ..: the storage buffer slot (kImgLoad ..: the image slot)
  uint32_t stride = 0;
  uint8_t tex_swizzle[4] = {0, 1, 2, 3};   // texld: which texel component each result component takes
  int8_t texel_offset[2] = {0, 0};          // gather4: the static offset of the footprint, in texels (the extended opcode token's)
  // The residency feedback forms of the texture lookups (Shader Model 5's tiled resources: sample_cl_s, ld_s, gather4_s, ...): the lookup reports into `status` (one component;
  // mask 0: the status is not wanted) 1 when every texel it read was mapped, 0 when one was not (ITextureSet::begin_feedback / end_feedback), and `clamp` (when has_clamp) is the
  // finest level of detail it may use (sample_cl_s, sample_b_cl_s, sample_d_cl_s, sample_c_cl_s).
  bool feedback = false, has_clamp = false;
  Dst status;
  Src clamp;
  Dst dst;
  Src src[4];
  uint8_t src_count = 0;
  uint32_t target = 0;   // if: where to go when the condition fails; else: where to go to leave the branch
  // The copies the DXIL translator makes of a block that ends the program (one in each branch that returns, D3d9Dxbc.cpp kDxbcTailBegin): `tail` is the block's number + 1 (0: not
  // in one) and `tail_at` the instruction's place in the copy, so the lane model (WaveRunner) sees one instruction wherever its copies are.
  uint32_t tail = 0, tail_at = 0;
};

}  // namespace dxgk
