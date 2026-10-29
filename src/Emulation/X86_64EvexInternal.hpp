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
#pragma once
#include "Emulation/X86_64Cpu.hpp"

// Shared by the AVX-512 translation units (X86_64Evex.cpp, X86_64EvexFp.cpp, X86_64EvexMisc.cpp): the instruction table and the element helpers.
namespace emulation {

using Vec = X86_64Cpu::Vec512;

inline uint64_t element(const Vec& v, uint32_t w, uint32_t i) {
  switch (w) {
    case 64: return v.q[i];
    case 32: return (v.q[i / 2] >> (32 * (i & 1))) & 0xFFFFFFFFu;
    case 16: return (v.q[i / 4] >> (16 * (i & 3))) & 0xFFFF;
    default: return (v.q[i / 8] >> (8 * (i & 7))) & 0xFF;
  }
}

inline void set_element(Vec& v, uint32_t w, uint32_t i, uint64_t x) {
  if (w == 64) {
    v.q[i] = x;
    return;
  }
  const uint32_t per = 64 / w, shift = w * (i % per);
  const uint64_t mask = ((uint64_t{1} << w) - 1) << shift;
  v.q[i / per] = (v.q[i / per] & ~mask) | ((x << shift) & mask);
}

inline int64_t signed_of(uint64_t v, uint32_t w) { return w == 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - w)) >> (64 - w); }
inline uint64_t width_mask(uint32_t w) { return w == 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1; }

inline void lane_bytes(const Vec& v, uint32_t lane, uint8_t out[16]) {
  for (uint32_t i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(v.q[lane * 2 + i / 8] >> (8 * (i & 7)));
}

inline void set_lane_bytes(Vec& v, uint32_t lane, const uint8_t in[16]) {
  v.q[lane * 2] = v.q[lane * 2 + 1] = 0;
  for (uint32_t i = 0; i < 16; ++i) v.q[lane * 2 + i / 8] |= uint64_t{in[i]} << (8 * (i & 7));
}

enum Kind : uint8_t {
  kMove, kMoveScalar, kMoveGpr, kBroadcast, kBroadcastGpr, kIntBin, kMulDq, kLane, kShiftCount, kShiftImm, kShiftVar, kRotVar, kCmpEq, kCmpImm, kTest, kTernlog,
  kShufImm, kPermute, kPermute2, kInsert, kExtract, kBlend, kAlign, kShuf128, kIntUnary, kExtend, kNarrow, kMaskMove, kCompress, kExpand,
  kFpBin, kFpScalar, kFpUnary, kFma, kFpCmp, kCvt, kCvtScalar, kCvtToGpr, kCvtFromGpr, kRound, kGetExp, kGetMant, kScalef, kFixup, kReduce, kRange, kFpClass, kRcp14, kRsqrt14,
  kGather, kScatter, kMiscInt, kDup, kMovHL, kExtractElem, kInsertElem, kHalfConv,
  kVnni, kMadd52, kMultishift, kShufBit, kShift2, kShift2Var, kCompressBW, kExpandBW, kGfAffine, kGfMul, kBf16, kAesNi, kClmul, kBroadcastMask, kDbpsadbw, kComi, kComplex, kVp2Intersect, kFma4, kVnni4, kDot, kEr, kBfloat
};

enum IntOp : uint8_t { kAdd, kSub, kAdds, kAddus, kSubs, kSubus, kMullo, kMulhs, kMulhu, kMulhrs, kAvg, kMins, kMinu, kMaxs, kMaxu, kAnd, kAndn, kOr, kXor };
enum LaneOp : uint8_t { kUnpackLo, kUnpackHi, kPacksswb, kPackuswb, kPackssdw, kPackusdw, kPshufb, kMaddwd, kMaddubsw, kSadbw, kUnpcklps, kUnpckhps, kShufps, kShufpd, kPalignr };
enum Tuple : uint8_t { kFull, kHalf, kQuarter, kEighth, kScalar, kMem128, kTuple2, kTuple4, kTuple8, kFullMem, kS1, kS2, kS4, kS8, kS16, kS32 };

struct EvexOp {
  uint8_t map, pp, opcode;
  int8_t w;      // 0, 1, or -1 for either
  Kind kind;
  uint8_t sub;
  uint8_t elem;      // element width in bits (0: from W: 32 or 64)
  Tuple tuple;
};

extern const EvexOp kEvexOps[];
extern const uint32_t kEvexOpCount;
extern const EvexOp kEvexFp16Ops[];      // X86_64EvexFp16.cpp
extern const uint32_t kEvexFp16OpCount;
extern const EvexOp kEvexBf16Ops[];      // X86_64EvexBf16.cpp
extern const uint32_t kEvexBf16OpCount;

uint64_t evex_integer_op(uint32_t sub, uint32_t w, uint64_t a, uint64_t b);
uint64_t evex_saturate_signed(int64_t v, uint32_t w);
uint64_t evex_saturate_unsigned(int64_t v, uint32_t w);

}  // namespace emulation

namespace emulation {
// The crypto primitives (X86_64Crypto.cpp) the 512-bit VAES, VPCLMULQDQ and GFNI forms reuse, one 128-bit lane at a time.
X86_64Cpu::Xmm crypto_aes_round(const X86_64Cpu::Xmm& state, const X86_64Cpu::Xmm& key, bool inverse, bool last);
X86_64Cpu::Xmm crypto_carryless_multiply(uint64_t a, uint64_t b);
uint8_t crypto_gf2p8_multiply(uint8_t a, uint8_t b);
uint8_t crypto_gf2p8_inverse(uint8_t a);
}  // namespace emulation
