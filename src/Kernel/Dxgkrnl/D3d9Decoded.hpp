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
};

enum RegType : uint8_t { kTemp = 0, kInput = 1, kConst = 2, kAddrTex = 3, kRastOut = 4, kAttrOut = 5, kTexCrdOut = 6, kColorOut = 8, kDepthOut = 9, kSampler = 10, kConstBool = 14 };
enum Usage : uint8_t { kUsagePosition = 0, kUsageTexCoord = 5, kUsageColor = 10 };
enum Modifier : uint8_t { kModNone, kModNeg, kModBias, kModBiasNeg, kModSign, kModSignNeg, kModComp, kModX2, kModX2Neg, kModDz, kModDw, kModAbs, kModAbsNeg, kModNot };

constexpr uint32_t kMaxTemps = 32, kMaxInputs = 16, kMaxOutputs = 16;
constexpr int8_t kSlotNone = -1, kSlotPosition = -2;

}  // namespace d3d9

using namespace d3d9;

// Does the Direct3D 9 instruction write the register named by its first parameter?
bool d3d9_op_writes_register(uint16_t op);

// One decoded instruction: sources and destination with their register slot already worked out, so running does not look anything up.
struct D3d9Program::Instruction {
  struct Src {
    uint8_t type = 0;
    uint16_t number = 0;
    int8_t slot = kSlotNone;   // inputs: the attribute location / varying
    uint8_t swizzle[4] = {0, 1, 2, 3};
    uint8_t modifier = kModNone;
    bool relative = false;
    uint8_t relative_component = 0;
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
  bool vector = false;   // rsq / exp / log of a Shader Model 4 shader: per component, not from x
  uint8_t tex_swizzle[4] = {0, 1, 2, 3};   // texld: which texel component each result component takes
  Dst dst;
  Src src[3];
  uint8_t src_count = 0;
  uint32_t target = 0;   // if: where to go when the condition fails; else: where to go to leave the branch
};

}  // namespace dxgk
