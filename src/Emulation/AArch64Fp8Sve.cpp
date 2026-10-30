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
#include "Emulation/AArch64SveCommon.hpp"

// FEAT_FP8 in SVE at a 128-bit vector length: conversions, dot products, widening multiply-adds and matrix multiplies. None of them has a predicate.
namespace emulation {

using namespace sve;
using namespace softfloat;

bool AArch64Cpu::fp8_sve(uint32_t insn, Step* result) {
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31;
  const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
  const bool osm = (s_.fpmr >> 14) & 1;
  if (lut(insn)) { *result = Step::Ok; return true; }
  auto finish = [&](Env& env) { fp_finish(env); *result = Step::Ok; return true; };
  if ((insn & 0xFFFEF000u) == 0x65083000u) {      // F1CVT, F2CVT, BF1CVT, BF2CVT (even bytes), and the LT forms (odd bytes)
    const uint32_t opc = bits_of(insn, 11, 10);
    const bool bfloat = opc >= 2, second = opc & 1, top = (insn >> 16) & 1;
    const fp8::Fmt fmt = second ? fb : fa;
    const uint32_t lscale = second ? static_cast<uint32_t>((s_.fpmr >> 32) & 0x3F) : static_cast<uint32_t>((s_.fpmr >> 16) & (bfloat ? 0x3F : 0x7F));
    Env env = fp_env();
    Vec out;
    const Vec src = s_.v[rn];
    for (uint32_t i = 0; i < 8; ++i) {
      const uint8_t b = static_cast<uint8_t>(get(src, 0, 2 * i + (top ? 1 : 0)));
      bool inexact;
      put(&out, 1, i, fp8::widen(fmt, b, bfloat ? Format::BF16 : Format::F16, bfloat ? lscale : (lscale & 15), &inexact));
      if (fp8::is_signaling(fmt, b)) env.flags |= kInvalid;
      if (inexact) env.flags |= kUnderflow | kInexact;
    }
    s_.v[rd] = out;
    return finish(env);
  }
  if ((insn & 0xFFFFF020u) == 0x650A3000u) {      // FCVTN, BFCVTN (halves to bytes, interleaved) and FCVTNB, FCVTNT (singles to bytes)
    const fp8::Fmt fd = static_cast<fp8::Fmt>((s_.fpmr >> 6) & 1);
    const bool saturate = (s_.fpmr >> 15) & 1;
    const uint32_t zn = bits_of(insn, 9, 6) << 1;
    const bool singles = (insn >> 10) & 1, flag = (insn >> 11) & 1;
    uint32_t flags = 0;
    Env env = fp_env();
    Vec out = s_.v[rd];
    if (!singles) {
      const int32_t nscale = flag ? static_cast<int32_t>(static_cast<int8_t>(s_.fpmr >> 24)) : static_cast<int32_t>((s_.fpmr >> 24) & 15) - static_cast<int32_t>((s_.fpmr >> 24) & 16);
      const Format from = flag ? Format::BF16 : Format::F16;
      for (uint32_t k = 0; k < 2; ++k)
        for (uint32_t i = 0; i < 8; ++i) put(&out, 0, 2 * i + k, fp8::narrow(fd, from, get(s_.v[(zn + k) & 31], 1, i), nscale, saturate, &flags));
    } else {
      const int32_t nscale = static_cast<int8_t>(s_.fpmr >> 24);
      for (uint32_t k = 0; k < 2; ++k)
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t byte = 4 * i + 2 * k + (flag ? 1 : 0);
          put(&out, 0, byte, fp8::narrow(fd, Format::F32, get(s_.v[(zn + k) & 31], 2, i), nscale, saturate, &flags));
          if (!flag) put(&out, 0, 4 * i + 2 * k + 1, 0);      // (the bottom form clears the odd bytes)
        }
    }
    env.flags |= flags;
    s_.v[rd] = out;
    return finish(env);
  }
  const uint32_t zm = (insn >> 16) & 31;
  // FDOT (FP8 to half or single), vector and indexed
  const bool dot_vector = (insn & 0xFFA0FC00u) == 0x64208400u, dot_half_index = (insn & 0xFFE0F400u) == 0x64204400u, dot_single_index = (insn & 0xFFE0FC00u) == 0x64604400u;
  if (dot_vector || dot_half_index || dot_single_index) {
    const bool to_half = dot_vector ? !((insn >> 22) & 1) : dot_half_index;
    const uint32_t group = to_half ? 2 : 4, count = to_half ? 8 : 4;
    const uint32_t scale = to_half ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
    const bool indexed = !dot_vector;
    const uint32_t mreg = indexed ? bits_of(insn, 18, 16) : zm;
    const uint32_t index = !indexed ? 0 : to_half ? (((insn >> 11) & 1) | (((insn >> 19) & 1) << 1) | (((insn >> 20) & 1) << 2)) : (((insn >> 19) & 1) | (((insn >> 20) & 1) << 1));
    Vec out;
    const Vec n = s_.v[rn], m = s_.v[mreg], acc = s_.v[rd];
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t a[4], b[4];
      for (uint32_t k = 0; k < group; ++k) {
        a[k] = static_cast<uint8_t>(get(n, 0, group * i + k));
        b[k] = static_cast<uint8_t>(get(m, 0, group * (indexed ? index : i) + k));
      }
      put(&out, to_half ? 1 : 2, i, fp8::dot_add(fa, a, fb, b, group, scale, to_half ? Format::F16 : Format::F32, static_cast<uint32_t>(get(acc, to_half ? 1 : 2, i)), osm));
    }
    s_.v[rd] = out;
    *result = Step::Ok;
    return true;
  }
  // FMLALB / FMLALT (FP8 to half) and FMLALL.. (FP8 to single)
  const bool lal_vector = (insn & 0xFFE0EC00u) == 0x64A08800u, lal_index = (insn & 0xFF60F000u) == 0x64205000u;
  const bool lall_vector = (insn & 0xFFE0CC00u) == 0x64208800u, lall_index = (insn & 0xFF20F000u) == 0x6420C000u;
  if (lal_vector || lal_index || lall_vector || lall_index) {
    const bool wide = lall_vector || lall_index, indexed = lal_index || lall_index;
    const uint32_t part = lal_vector ? ((insn >> 12) & 1) : lal_index ? ((insn >> 23) & 1) : lall_vector ? bits_of(insn, 13, 12) : bits_of(insn, 23, 22);
    const uint32_t group = wide ? 4 : 2, count = wide ? 4 : 8;
    const uint32_t scale = wide ? static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F) : static_cast<uint32_t>((s_.fpmr >> 16) & 15);
    const uint32_t mreg = indexed ? bits_of(insn, 18, 16) : zm;
    const uint32_t index = !indexed ? 0 : (((insn >> 10) & 1) | (((insn >> 11) & 1) << 1) | (((insn >> 19) & 1) << 2) | (((insn >> 20) & 1) << 3));
    Env env = fp_env();
    Vec out;
    const Vec n = s_.v[rn], m = s_.v[mreg], acc = s_.v[rd];
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t a = static_cast<uint8_t>(get(n, 0, group * i + part));
      const uint8_t b = static_cast<uint8_t>(get(m, 0, indexed ? index : group * i + part));
      put(&out, wide ? 2 : 1, i, fp8::multiply_add(fa, a, fb, b, scale, wide ? Format::F32 : Format::F16, static_cast<uint32_t>(get(acc, wide ? 2 : 1, i)), osm, env));
    }
    s_.v[rd] = out;
    *result = Step::Ok;
    return true;
  }
  if ((insn & 0xFFA0FC00u) == 0x6420E000u) {      // FMMLA (FP8 to single or half)
    const bool to_single = !((insn >> 22) & 1);
    const uint32_t scale = to_single ? static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F) : static_cast<uint32_t>((s_.fpmr >> 16) & 15);
    const Vec n = s_.v[rn], m = s_.v[zm], acc = s_.v[rd];
    Vec out;
    const uint32_t count = to_single ? 4 : 8, group = to_single ? 8 : 4;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t row_n = i >> 1, row_m = to_single ? i & 1 : (i & 1) + 2 * (i >> 2);
      uint8_t a[8], b[8];
      for (uint32_t k = 0; k < group; ++k) { a[k] = static_cast<uint8_t>(get(n, 0, group * row_n + k)); b[k] = static_cast<uint8_t>(get(m, 0, group * row_m + k)); }
      put(&out, to_single ? 2 : 1, i, fp8::dot_add(fa, a, fb, b, group, scale, to_single ? Format::F32 : Format::F16, static_cast<uint32_t>(get(acc, to_single ? 2 : 1, i)), osm));
    }
    s_.v[rd] = out;
    *result = Step::Ok;
    return true;
  }
  return false;
}

}  // namespace emulation
