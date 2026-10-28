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
#include "Emulation/X86_64EvexInternal.hpp"
#include "Emulation/SoftFloat.hpp"

// AVX-512 FP16: the instruction rows (maps 3, 5 and 6; the arithmetic, comparison, rounding and conversion handlers are the ones in X86_64EvexFp.cpp with 16-bit elements) and the
// complex-number instructions.
namespace emulation {

using namespace softfloat;

// A conversion's `sub` is src | dst << 3 | 0x40 (truncate) | 0x80 (the 16-bit integer side is unsigned), with the types of X86_64EvexFp.cpp (6: half, 7: 16-bit integer); `elem` is the
// width of the memory operand's element (what a broadcast loads).
const EvexOp kEvexFp16Ops[] = {
    {5, 0, 0x58, 0, kFpBin, 0x58, 16, kFull}, {5, 2, 0x58, 0, kFpScalar, 0x58, 16, kS2}, {5, 0, 0x59, 0, kFpBin, 0x59, 16, kFull},
    {5, 2, 0x59, 0, kFpScalar, 0x59, 16, kS2}, {5, 0, 0x5C, 0, kFpBin, 0x5C, 16, kFull}, {5, 2, 0x5C, 0, kFpScalar, 0x5C, 16, kS2},
    {5, 0, 0x5D, 0, kFpBin, 0x5D, 16, kFull}, {5, 2, 0x5D, 0, kFpScalar, 0x5D, 16, kS2}, {5, 0, 0x5E, 0, kFpBin, 0x5E, 16, kFull},
    {5, 2, 0x5E, 0, kFpScalar, 0x5E, 16, kS2}, {5, 0, 0x5F, 0, kFpBin, 0x5F, 16, kFull}, {5, 2, 0x5F, 0, kFpScalar, 0x5F, 16, kS2},
    {5, 0, 0x51, 0, kFpUnary, 0x51, 16, kFull}, {5, 2, 0x51, 0, kFpScalar, 0x51, 16, kS2}, {6, 1, 0x96, 0, kFma, 0x96, 16, kFull},
    {6, 1, 0x97, 0, kFma, 0x97, 16, kFull}, {6, 1, 0x98, 0, kFma, 0x98, 16, kFull}, {6, 1, 0x99, 0, kFma, 0x99, 16, kS2},
    {6, 1, 0x9A, 0, kFma, 0x9A, 16, kFull}, {6, 1, 0x9B, 0, kFma, 0x9B, 16, kS2}, {6, 1, 0x9C, 0, kFma, 0x9C, 16, kFull},
    {6, 1, 0x9D, 0, kFma, 0x9D, 16, kS2}, {6, 1, 0x9E, 0, kFma, 0x9E, 16, kFull}, {6, 1, 0x9F, 0, kFma, 0x9F, 16, kS2},
    {6, 1, 0xA6, 0, kFma, 0xA6, 16, kFull}, {6, 1, 0xA7, 0, kFma, 0xA7, 16, kFull}, {6, 1, 0xA8, 0, kFma, 0xA8, 16, kFull},
    {6, 1, 0xA9, 0, kFma, 0xA9, 16, kS2}, {6, 1, 0xAA, 0, kFma, 0xAA, 16, kFull}, {6, 1, 0xAB, 0, kFma, 0xAB, 16, kS2},
    {6, 1, 0xAC, 0, kFma, 0xAC, 16, kFull}, {6, 1, 0xAD, 0, kFma, 0xAD, 16, kS2}, {6, 1, 0xAE, 0, kFma, 0xAE, 16, kFull},
    {6, 1, 0xAF, 0, kFma, 0xAF, 16, kS2}, {6, 1, 0xB6, 0, kFma, 0xB6, 16, kFull}, {6, 1, 0xB7, 0, kFma, 0xB7, 16, kFull},
    {6, 1, 0xB8, 0, kFma, 0xB8, 16, kFull}, {6, 1, 0xB9, 0, kFma, 0xB9, 16, kS2}, {6, 1, 0xBA, 0, kFma, 0xBA, 16, kFull},
    {6, 1, 0xBB, 0, kFma, 0xBB, 16, kS2}, {6, 1, 0xBC, 0, kFma, 0xBC, 16, kFull}, {6, 1, 0xBD, 0, kFma, 0xBD, 16, kS2},
    {6, 1, 0xBE, 0, kFma, 0xBE, 16, kFull}, {6, 1, 0xBF, 0, kFma, 0xBF, 16, kS2}, {3, 0, 0xC2, 0, kFpCmp, 0, 16, kFull},
    {3, 2, 0xC2, 0, kFpCmp, 1, 16, kS2}, {5, 0, 0x2E, 0, kComi, 0, 16, kS2}, {5, 0, 0x2F, 0, kComi, 1, 16, kS2},
    {3, 0, 0x08, 0, kRound, 0, 16, kFull}, {3, 0, 0x0A, 0, kRound, 1, 16, kS2}, {6, 1, 0x42, 0, kGetExp, 0, 16, kFull},
    {6, 1, 0x43, 0, kGetExp, 1, 16, kS2}, {6, 1, 0x2C, 0, kScalef, 0, 16, kFull}, {6, 1, 0x2D, 0, kScalef, 1, 16, kS2},
    {3, 0, 0x26, 0, kGetMant, 0, 16, kFull}, {3, 0, 0x27, 0, kGetMant, 1, 16, kS2}, {3, 0, 0x56, 0, kReduce, 0, 16, kFull},
    {3, 0, 0x57, 0, kReduce, 1, 16, kS2}, {3, 0, 0x66, 0, kFpClass, 0, 16, kFull}, {3, 0, 0x67, 0, kFpClass, 1, 16, kS2},
    {6, 1, 0x4C, 0, kRcp14, 0, 16, kFull}, {6, 1, 0x4D, 0, kRcp14, 1, 16, kS2}, {6, 1, 0x4E, 0, kRsqrt14, 0, 16, kFull},
    {6, 1, 0x4F, 0, kRsqrt14, 1, 16, kS2}, {5, 0, 0x5A, 0, kCvt, 14, 16, kQuarter}, {5, 1, 0x5A, 1, kCvt, 49, 64, kFull},
    {5, 2, 0x5A, 0, kCvtScalar, 14, 16, kS2}, {5, 3, 0x5A, 1, kCvtScalar, 49, 64, kS8}, {5, 0, 0x5B, 0, kCvt, 50, 32, kFull},
    {5, 0, 0x5B, 1, kCvt, 52, 64, kFull}, {5, 1, 0x5B, 0, kCvt, 22, 16, kHalf}, {5, 2, 0x5B, 0, kCvt, 86, 16, kHalf},
    {5, 1, 0x1D, 0, kCvt, 48, 32, kFull}, {5, 0, 0x1D, 0, kCvtScalar, 48, 32, kS4}, {6, 1, 0x13, 0, kCvt, 6, 16, kHalf},
    {6, 0, 0x13, 0, kCvtScalar, 6, 16, kS2}, {5, 2, 0x2A, 0, kCvtFromGpr, 6, 32, kS4}, {5, 2, 0x7B, 0, kCvtFromGpr, 134, 32, kS4},
    {5, 2, 0x2A, 1, kCvtFromGpr, 6, 32, kS8}, {5, 2, 0x7B, 1, kCvtFromGpr, 134, 32, kS8}, {5, 2, 0x2D, 0, kCvtToGpr, 6, 16, kS2},
    {5, 2, 0x2C, 0, kCvtToGpr, 70, 16, kS2}, {5, 2, 0x79, 0, kCvtToGpr, 134, 16, kS2}, {5, 2, 0x78, 0, kCvtToGpr, 198, 16, kS2},
    {5, 2, 0x2D, 1, kCvtToGpr, 6, 16, kS2}, {5, 2, 0x2C, 1, kCvtToGpr, 70, 16, kS2}, {5, 2, 0x79, 1, kCvtToGpr, 134, 16, kS2},
    {5, 2, 0x78, 1, kCvtToGpr, 198, 16, kS2}, {5, 0, 0x79, 0, kCvt, 30, 16, kHalf}, {5, 0, 0x78, 0, kCvt, 94, 16, kHalf},
    {5, 1, 0x79, 0, kCvt, 46, 16, kQuarter}, {5, 1, 0x78, 0, kCvt, 110, 16, kQuarter}, {5, 1, 0x7B, 0, kCvt, 38, 16, kQuarter},
    {5, 1, 0x7A, 0, kCvt, 102, 16, kQuarter}, {5, 3, 0x7A, 0, kCvt, 51, 32, kFull}, {5, 3, 0x7A, 1, kCvt, 53, 64, kFull},
    {5, 0, 0x7D, 0, kCvt, 190, 16, kFull}, {5, 0, 0x7C, 0, kCvt, 254, 16, kFull}, {5, 1, 0x7D, 0, kCvt, 62, 16, kFull},
    {5, 1, 0x7C, 0, kCvt, 126, 16, kFull}, {5, 3, 0x7D, 0, kCvt, 183, 16, kFull}, {5, 2, 0x7D, 0, kCvt, 55, 16, kFull},
    {5, 2, 0x10, 0, kMoveScalar, 0, 16, kS2}, {5, 2, 0x11, 0, kMoveScalar, 1, 16, kS2}, {5, 1, 0x6E, -1, kMoveGpr, 0, 16, kS2},
    {5, 1, 0x7E, -1, kMoveGpr, 1, 16, kS2}, {5, 2, 0x6E, -1, kMoveGpr, 2, 16, kS2}, {6, 2, 0xD6, 0, kComplex, 0, 32, kFull},
    {6, 3, 0xD6, 0, kComplex, 1, 32, kFull}, {6, 2, 0x56, 0, kComplex, 2, 32, kFull}, {6, 3, 0x56, 0, kComplex, 3, 32, kFull},
    {6, 2, 0xD7, 0, kComplex, 4, 32, kS4}, {6, 3, 0xD7, 0, kComplex, 5, 32, kS4}, {6, 2, 0x57, 0, kComplex, 6, 32, kS4},
    {6, 3, 0x57, 0, kComplex, 7, 32, kS4},
};
const uint32_t kEvexFp16OpCount = sizeof(kEvexFp16Ops) / sizeof(kEvexFp16Ops[0]);

// VFMULCPH, VFCMULCPH, VFMADDCPH, VFCMADDCPH and their scalar forms: complex numbers are pairs of halves (real, imaginary) in a dword, and the mask has one bit per complex number.
// sub: bit 0 conjugates the second operand, bit 1 accumulates into the destination, bit 2 is the scalar form. The destination register must differ from both sources.
// The products are rounded once each way a chained fused multiply-add rounds (a*b+c, then -a*b added to that); the hardware's order of the intermediate roundings is not documented.
X86_64Cpu::Step X86_64Cpu::evex_fp16(GuestMemory& m, const EvexOp& def) {
  const bool conj = (def.sub & 1) != 0, accumulate = (def.sub & 2) != 0, scalar = (def.sub & 4) != 0;
  const uint32_t reg = ev_.reg, vvvv = ev_.vvvv;
  if (reg == vvvv || (!ev_.memory && reg == ev_.rm)) return fail(6);
  const uint32_t vl = scalar ? 128 : ev_.vl;
  Env env;
  env.mode = ev_.er ? static_cast<Rounding>(ev_.rc) : static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  const Vec a = get_vec(vvvv);
  Vec b;
  if (scalar) {
    if (ev_.memory) {
      uint64_t v;
      if (!load(m, memory_address(), 4, &v)) return Step::Fault;
      b.q[0] = v;
    } else {
      b = get_vec(ev_.rm);
    }
  } else if (!ev_read_rm(m, &b, 32, true, true)) {
    return Step::Fault;
  }
  Vec result = scalar ? a : Vec();
  const Vec dest = ev_.old_dest;
  const Format h = Format::F16;
  for (uint32_t i = 0; i < (scalar ? 1u : vl / 32); ++i) {
    if (ev_.aaa != 0 && !ev_active(i)) {
      set_element(result, 32, i, ev_.z ? 0 : element(dest, 32, i));
      continue;
    }
    const uint64_t are = element(a, 16, 2 * i), aim = element(a, 16, 2 * i + 1), bre = element(b, 16, 2 * i), bim = element(b, 16, 2 * i + 1);
    const uint64_t cre = element(dest, 16, 2 * i), cim = element(dest, 16, 2 * i + 1);
    Env le = env;
    le.flags = 0;
    // real: a.re*b.re -/+ a.im*b.im; imaginary: a.im*b.re +/- a.re*b.im (the conjugate flips the second sign of each)
    uint64_t t = accumulate ? fused_multiply_add(h, are, bre, cre, le) : mul(h, are, bre, le);
    const uint64_t re = fused_multiply_add(h, conj ? aim : negate(h, aim), bim, t, le);
    t = accumulate ? fused_multiply_add(h, aim, bre, cim, le) : mul(h, aim, bre, le);
    const uint64_t im = fused_multiply_add(h, conj ? negate(h, are) : are, bim, t, le);
    le.flags &= ~kDenormal;
    env.flags |= le.flags;
    set_element(result, 16, 2 * i, re);
    set_element(result, 16, 2 * i + 1, im);
  }
  set_vec(reg, result, vl);
  if (!ev_.er) s_.mxcsr |= env.flags;
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
