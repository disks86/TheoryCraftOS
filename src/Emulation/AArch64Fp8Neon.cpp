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
#include "Emulation/AArch64Fp8.hpp"
#include "Emulation/AArch64NeonCommon.hpp"
#include "Emulation/AArch64SveCommon.hpp"

// FEAT_FP8 in Advanced SIMD: conversions between 8-bit and wider floating point, dot products, widening multiply-adds and matrix multiplies.
namespace emulation {

using namespace neon;
using namespace softfloat;

bool AArch64Cpu::fp8_neon(uint32_t insn, Step* result) {
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31;
  const bool upper = (insn >> 30) & 1;
  if (lut(insn)) { *result = Step::Ok; return true; }
  if ((insn & 0xBFE0FC00u) == 0x2EC03C00u || (insn & 0xBFA0FC00u) == 0x2EA0FC00u) {      // FSCALE (vector): each element times two to the power of the signed integer in the second source
    const bool halves = (insn & 0xBFE0FC00u) == 0x2EC03C00u, doubles = !halves && ((insn >> 22) & 1);
    if (doubles && !upper) { *result = undefined(); return true; }
    const uint32_t esz = halves ? 1 : doubles ? 3 : 2, rm = (insn >> 16) & 31;
    const Format f = halves ? Format::F16 : doubles ? Format::F64 : Format::F32;
    Env env = fp_env();
    Vec out;
    const uint32_t count = (upper ? 16u : 8u) >> esz;
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t x = lane(s_.v[rn], 8u << esz, i), y = lane(s_.v[rm], 8u << esz, i);
      set_lane(&out, 8u << esz, i, fp_default_nan_fix(f, sve::fscale_value(f, x, sext(y, 8u << esz), env)));
    }
    s_.v[rd] = out;
    fp_finish(env);
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xBF3FFC00u) == 0x2E217800u) {      // F1CVTL, F2CVTL, BF1CVTL, BF2CVTL (and the "2" forms): the lower or upper eight bytes to eight halves
    const uint32_t opc = bits_of(insn, 23, 22);
    const fp8::Fmt fmt = static_cast<fp8::Fmt>(s_.fpmr >> ((opc & 1) ? 3 : 0) & 1);
    const bool bfloat = opc >= 2;
    Env env = fp_env();
    const uint32_t lscale = (opc & 1) ? static_cast<uint32_t>((s_.fpmr >> 32) & 0x3F) : static_cast<uint32_t>((s_.fpmr >> 16) & (bfloat ? 0x3F : 0x7F));      // (the second format has its own scale)
    Vec out;
    const Vec src = s_.v[rn];
    for (uint32_t i = 0; i < 8; ++i) {
      const uint8_t b = static_cast<uint8_t>(lane(src, 8, (upper ? 8 : 0) + i));
      bool inexact;
      set_lane(&out, 16, i, fp8::widen(fmt, b, bfloat ? Format::BF16 : Format::F16, bfloat ? lscale : (lscale & 15), &inexact));
      if (fp8::is_signaling(fmt, b)) env.flags |= kInvalid;
      if (inexact) env.flags |= kUnderflow | kInexact;
    }
    s_.v[rd] = out;
    fp_finish(env);
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xBFA0FC00u) == 0x0E00F400u) {      // FCVTN, FCVTN2: two vectors of halves or singles to bytes
    const bool halves = (insn >> 22) & 1;
    const uint32_t rm = (insn >> 16) & 31;
    const fp8::Fmt fmt = static_cast<fp8::Fmt>((s_.fpmr >> 6) & 1);
    const int32_t nscale = halves ? static_cast<int32_t>((s_.fpmr >> 24) & 15) - static_cast<int32_t>((s_.fpmr >> 24) & 16) : static_cast<int8_t>(s_.fpmr >> 24);      // (a half precision source wraps the scale at five bits)
    const bool saturate = (s_.fpmr >> 15) & 1;
    Env env = fp_env();
    uint32_t flags = 0;
    Vec out;
    if (!halves && upper) out = s_.v[rd];
    const uint32_t per = halves ? (upper ? 8 : 4) : 4;      // (sources per register)
    const Format from = halves ? Format::F16 : Format::F32;
    for (uint32_t k = 0; k < 2; ++k)
      for (uint32_t i = 0; i < per; ++i) {
        const uint64_t v = lane(s_.v[k == 0 ? rn : rm], halves ? 16 : 32, i);
        set_lane(&out, 8, (halves ? 0 : (upper ? 8 : 0)) + k * per + i, fp8::narrow(fmt, from, v, nscale, saturate, &flags));
      }
    env.flags |= flags;
    s_.v[rd] = out;
    fp_finish(env);
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xBFE0FC00u) == 0x0EC0FC00u || (insn & 0xBFC0F400u) == 0x0FC00000u) {      // FMLALB, FMLALT (FP8 to half): even or odd bytes
    const bool by_element = ((insn >> 24) & 0xF) == 0xF;
    const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
    const uint32_t scale = static_cast<uint32_t>((s_.fpmr >> 16) & 15);
    const uint32_t rm = by_element ? bits_of(insn, 18, 16) : (insn >> 16) & 31;
    const uint32_t index = by_element ? (bits_of(insn, 21, 19) | (((insn >> 11) & 1) << 3)) : 0;
    Env env = fp_env();
    Vec out;
    const Vec n = s_.v[rn], m = s_.v[rm], acc = s_.v[rd];
    for (uint32_t i = 0; i < 8; ++i) {
      const uint8_t a = static_cast<uint8_t>(lane(n, 8, 2 * i + (upper ? 1 : 0)));
      const uint8_t b = static_cast<uint8_t>(by_element ? lane(m, 8, index) : lane(m, 8, 2 * i + (upper ? 1 : 0)));
      set_lane(&out, 16, i, fp8::multiply_add(fa, a, fb, b, scale, Format::F16, static_cast<uint32_t>(lane(acc, 16, i)), (s_.fpmr >> 14) & 1, env));
    }
    s_.v[rd] = out;
    fp_finish(env);
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xBFA0FC00u) == 0x0E00C400u || (insn & 0xBF80F400u) == 0x2F008000u) {      // FMLALLBB, BT, TB, TT (FP8 to single): the byte of each group of four
    const bool by_element = ((insn >> 24) & 0xF) == 0xF;
    const uint32_t part = ((insn >> 22) & 1) + (upper ? 2 : 0);
    const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
    const uint32_t scale = static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
    const uint32_t rm = by_element ? bits_of(insn, 18, 16) : (insn >> 16) & 31;
    const uint32_t index = by_element ? (bits_of(insn, 21, 19) | (((insn >> 11) & 1) << 3)) : 0;
    Env env = fp_env();
    Vec out;
    const Vec n = s_.v[rn], m = s_.v[rm], acc = s_.v[rd];
    for (uint32_t i = 0; i < 4; ++i) {
      const uint8_t a = static_cast<uint8_t>(lane(n, 8, 4 * i + part));
      const uint8_t b = static_cast<uint8_t>(by_element ? lane(m, 8, index) : lane(m, 8, 4 * i + part));
      set_lane(&out, 32, i, fp8::multiply_add(fa, a, fb, b, scale, Format::F32, static_cast<uint32_t>(lane(acc, 32, i)), (s_.fpmr >> 14) & 1, env));
    }
    s_.v[rd] = out;
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xBFA0FC00u) == 0x0E00FC00u || (insn & 0xBF80F400u) == 0x0F000000u) {      // FDOT (FP8 to half, or to single): the products of each group of two or four bytes
    const bool by_element = ((insn >> 24) & 0xF) == 0xF, to_half = (insn >> 22) & 1;
    const uint32_t group = to_half ? 2 : 4, lanes = (upper ? 8 : 4) / (to_half ? 1 : 2) / (to_half ? 1 : 1);
    (void)lanes;
    const uint32_t count = to_half ? (upper ? 8 : 4) : (upper ? 4 : 2);
    const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
    const uint32_t scale = to_half ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
    const uint32_t rm = by_element ? (to_half ? bits_of(insn, 19, 16) : bits_of(insn, 20, 16)) : (insn >> 16) & 31;      // (a half precision destination has four bits of register, a single one five)
    const uint32_t index = by_element ? (to_half ? (bits_of(insn, 21, 20) | (((insn >> 11) & 1) << 2)) : (((insn >> 21) & 1) | (((insn >> 11) & 1) << 1))) : 0;
    Vec out;
    const Vec n = s_.v[rn], m = s_.v[rm], acc = s_.v[rd];
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t a[4], b[4];
      for (uint32_t k = 0; k < group; ++k) {
        a[k] = static_cast<uint8_t>(lane(n, 8, group * i + k));
        b[k] = static_cast<uint8_t>(lane(m, 8, group * (by_element ? index : i) + k));
      }
      set_lane(&out, to_half ? 16 : 32, i, fp8::dot_add(fa, a, fb, b, group, scale, to_half ? Format::F16 : Format::F32, static_cast<uint32_t>(lane(acc, to_half ? 16 : 32, i)), (s_.fpmr >> 14) & 1));
    }
    s_.v[rd] = out;
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xFF60FC00u) == 0x6E00EC00u) {      // FMMLA (FP8 to single: 2x2 of eight-byte rows; to half: each lane sums four bytes of a row of each source)
    const bool to_single = (insn >> 23) & 1;
    const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
    const uint32_t scale = to_single ? static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F) : static_cast<uint32_t>((s_.fpmr >> 16) & 15);
    const uint32_t rm = (insn >> 16) & 31;
    const Vec n = s_.v[rn], m = s_.v[rm], acc = s_.v[rd];
    Vec out;
    const uint32_t count = to_single ? 4 : 8, group = to_single ? 8 : 4;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t row_n = to_single ? i >> 1 : i >> 1, row_m = to_single ? i & 1 : (i & 1) + 2 * (i >> 2);
      uint8_t a[8], b[8];
      for (uint32_t k = 0; k < group; ++k) {
        a[k] = static_cast<uint8_t>(lane(n, 8, group * row_n + k));
        b[k] = static_cast<uint8_t>(lane(m, 8, group * row_m + k));
      }
      set_lane(&out, to_single ? 32 : 16, i, fp8::dot_add(fa, a, fb, b, group, scale, to_single ? Format::F32 : Format::F16, static_cast<uint32_t>(lane(acc, to_single ? 32 : 16, i)), (s_.fpmr >> 14) & 1));
    }
    s_.v[rd] = out;
    *result = Step::Ok;
    return true;
  }
  return false;
}

}  // namespace emulation
