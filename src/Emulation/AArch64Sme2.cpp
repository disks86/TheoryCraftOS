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
#include "Emulation/SoftFloat.hpp"

// SME2 at a streaming vector length of 128 bits: the ZA array vector-group instructions (ADD / SUB / FADD / FSUB, FMLA / FMLS, the dot products, the vertical dot products and the widening multiply-accumulates
// with two or four ZA vectors per source vector). The encodings are matched through a table generated from LLVM's (AArch64Sme2Forms.inc): the fixed bits, the group size, the operand shape and where the
// offset and the element index sit, which differ from form to form.
namespace emulation {

using namespace sve;
using namespace softfloat;

#include "Emulation/AArch64Sme2Forms.inc"
#include "Emulation/AArch64Sme2VecForms.inc"
#include "Emulation/AArch64Sme2MiscForms.inc"
#include "Emulation/AArch64Sme2Mop4Forms.inc"

namespace {

uint32_t gather(uint32_t insn, const uint8_t* bits, uint32_t count) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < count; ++i) v |= ((insn >> bits[i]) & 1u) << i;
  return v;
}

// The ZA arithmetic runs with the default NaN: any NaN result is the quiet NaN with an empty payload.
uint64_t default_nan(Format f, uint64_t r) { return is_nan(f, r) ? (f == Format::F32 ? 0x7FC00000ull : f == Format::F64 ? 0x7FF8000000000000ull : f == Format::BF16 ? 0x7FC0ull : 0x7E00ull) : r; }

void to_bytes(const AArch64Cpu::Vec& v, uint8_t* b) {
  for (uint32_t i = 0; i < 8; ++i) { b[i] = static_cast<uint8_t>(v.lo >> (8 * i)); b[8 + i] = static_cast<uint8_t>(v.hi >> (8 * i)); }
}
AArch64Cpu::Vec from_bytes(const uint8_t* b) {
  AArch64Cpu::Vec v;
  for (uint32_t i = 0; i < 8; ++i) { v.lo |= static_cast<uint64_t>(b[i]) << (8 * i); v.hi |= static_cast<uint64_t>(b[8 + i]) << (8 * i); }
  return v;
}

}  // namespace

AArch64Cpu::Step AArch64Cpu::sme2_array(uint32_t form, uint32_t insn) {
  const Sme2Form& f = kSme2Forms[form];
  const uint32_t n = f.group, rv = static_cast<uint32_t>(reg(8 + bits_of(insn, 14, 13), false));
  const uint32_t off = gather(insn, f.off_bits, f.off_count), idx = gather(insn, f.idx_bits, f.idx_count);
  const uint32_t zn = n == 1 ? bits_of(insn, 9, 5) : n == 2 ? (bits_of(insn, 9, 6) << 1) : (bits_of(insn, 9, 7) << 2);
  uint32_t zm = bits_of(insn, 19, 16);
  if (f.shape == 2) zm = n == 2 ? (bits_of(insn, 20, 17) << 1) : (bits_of(insn, 20, 18) << 2);
  const uint32_t scale = f.off_scale, stride = 16 / n;
  const uint32_t src = f.src, dst = f.dst;
  const uint32_t lanes = 16u >> dst;
  Env env = fp_env();
  const Sme2Op op = f.op;
  auto row_of = [&](uint32_t r, uint32_t j) { return (((rv - rv % scale) + off * scale + j) % stride) + r * stride; };      // (the selector is aligned down to the number of vectors per group)
  auto zn_vec = [&](uint32_t r) -> const Vec& { return s_.v[(zn + r) & 31]; };
  auto zm_vec = [&](uint32_t r) -> const Vec& { return s_.v[f.shape == 2 ? ((zm + r) & 31) : zm]; };
  auto ext = [&](uint64_t v, uint32_t esz, bool is_signed) -> int64_t { return is_signed ? sext(v, 8u << esz) : static_cast<int64_t>(v); };
  const bool bf_op = op == k_bfadd || op == k_bfsub || op == k_bfmla || op == k_bfmls;
  const Format fmt = dst == 2 ? Format::F32 : dst == 3 ? Format::F64 : bf_op ? Format::BF16 : Format::F16;

  switch (op) {
    case k_add: case k_sub: case k_fadd: case k_fsub: case k_fmla: case k_fmls: case k_bfadd: case k_bfsub: case k_bfmla: case k_bfmls: {
      const bool fp = op != k_add && op != k_sub;
      const bool sub = op == k_sub || op == k_fsub || op == k_fmls || op == k_bfsub || op == k_bfmls;
      const bool fma = op == k_fmla || op == k_fmls || op == k_bfmla || op == k_bfmls;
      for (uint32_t r = 0; r < n; ++r) {
        Vec& row = s_.za[row_of(r, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          const uint64_t a = get(zn_vec(r), dst, l), acc = get(row, dst, l);
          uint64_t b = 0;
          if (f.shape == 1 || f.shape == 2) b = get(zm_vec(r), dst, l);
          else if (f.shape == 3) b = get(s_.v[zm], dst, idx);
          uint64_t result;
          if (fma) result = default_nan(fmt, fused_multiply_add(fmt, sub ? negate(fmt, a) : a, b, acc, env));
          else if (fp) result = default_nan(fmt, add(fmt, acc, sub ? negate(fmt, a) : a, env));
          else if (f.shape == 0) result = sub ? acc - a : acc + a;
          else result = sub ? a - b : a + b;      // (the vector forms write the sum or difference of the two operands, they do not accumulate)
          put(&row, dst, l, result & element_ones(dst));
        }
      }
      return Step::Ok;      // (the exception flags of the ZA arithmetic are not reported through FPSR)
    }
    case k_sdot: case k_udot: case k_usdot: case k_sudot: case k_fdot: case k_bfdot: {
      const uint32_t c = (src == 0 || dst == 3) ? 4 : 2;
      const bool n_signed = op == k_sdot || op == k_sudot, m_signed = op == k_sdot || op == k_usdot;
      for (uint32_t r = 0; r < n; ++r) {
        Vec& row = s_.za[row_of(r, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          auto m_at = [&](uint32_t i) { return get(f.shape == 3 ? s_.v[zm] : zm_vec(r), src, (f.shape == 3 ? c * idx : c * l) + i); };
          if (op == k_fdot || op == k_bfdot) {
            uint64_t result;
            if (op == k_bfdot) {
              result = bfloat_dot_add(get(row, 2, l), get(zn_vec(r), 1, 2 * l), get(zn_vec(r), 1, 2 * l + 1), m_at(0), m_at(1), env);
            } else {
              auto w = [&](uint64_t v) { return convert(Format::F16, Format::F32, v, env); };
              const uint64_t t = add(Format::F32, mul(Format::F32, w(get(zn_vec(r), 1, 2 * l)), w(m_at(0)), env), mul(Format::F32, w(get(zn_vec(r), 1, 2 * l + 1)), w(m_at(1)), env), env);
              result = default_nan(Format::F32, add(Format::F32, get(row, 2, l), t, env));
            }
            put(&row, 2, l, result);
            continue;
          }
          uint64_t sum = get(row, dst, l);
          for (uint32_t i = 0; i < c; ++i) sum += static_cast<uint64_t>(ext(get(zn_vec(r), src, c * l + i), src, n_signed) * ext(m_at(i), src, m_signed));
          put(&row, dst, l, sum & element_ones(dst));
        }
      }
      return Step::Ok;
    }
    case k_svdot: case k_uvdot: case k_suvdot: case k_usvdot: case k_fvdot: case k_bfvdot: {
      const uint32_t c = n;      // (the number of source vectors, and of ZA vectors written)
      const bool n_signed = op == k_svdot || op == k_suvdot, m_signed = op == k_svdot || op == k_usvdot;
      Vec result[4];
      for (uint32_t i = 0; i < c; ++i) {
        result[i] = s_.za[row_of(i, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          const uint32_t e = (dst == 3) ? 4 * l + i : c * l + i;
          if (op == k_fvdot || op == k_bfvdot) {
            const uint64_t n0 = get(zn_vec(0), 1, e), n1 = get(zn_vec(1), 1, e), m0 = get(s_.v[zm], 1, 2 * idx), m1 = get(s_.v[zm], 1, 2 * idx + 1);
            uint64_t r;
            if (op == k_bfvdot) {
              r = bfloat_dot_add(get(result[i], 2, l), n0, n1, m0, m1, env);
            } else {
              auto w = [&](uint64_t v) { return convert(Format::F16, Format::F32, v, env); };
              const uint64_t t = add(Format::F32, mul(Format::F32, w(n0), w(m0), env), mul(Format::F32, w(n1), w(m1), env), env);
              r = default_nan(Format::F32, add(Format::F32, get(result[i], 2, l), t, env));
            }
            put(&result[i], 2, l, r);
            continue;
          }
          uint64_t sum = get(result[i], dst, l);
          for (uint32_t j = 0; j < c; ++j) sum += static_cast<uint64_t>(ext(get(zn_vec(j), src, e), src, n_signed) * ext(get(s_.v[zm], src, c * idx + j), src, m_signed));
          put(&result[i], dst, l, sum & element_ones(dst));
        }
      }
      for (uint32_t i = 0; i < c; ++i) s_.za[row_of(i, 0)] = result[i];
      return Step::Ok;
    }
    case k_smlal: case k_umlal: case k_smlsl: case k_umlsl: case k_fmlal: case k_fmlsl: case k_bfmlal: case k_bfmlsl:
    case k_smlall: case k_umlall: case k_smlsll: case k_umlsll: case k_sumlall: case k_usmlall: {
      const bool sub = op == k_smlsl || op == k_umlsl || op == k_fmlsl || op == k_bfmlsl || op == k_smlsll || op == k_umlsll;
      const bool n_signed = op == k_smlal || op == k_smlsl || op == k_smlall || op == k_smlsll || op == k_sumlall;
      const bool m_signed = op == k_smlal || op == k_smlsl || op == k_smlall || op == k_smlsll || op == k_usmlall;
      const bool fp = op == k_fmlal || op == k_fmlsl || op == k_bfmlal || op == k_bfmlsl, bf = op == k_bfmlal || op == k_bfmlsl;
      for (uint32_t r = 0; r < n; ++r)
        for (uint32_t j = 0; j < scale; ++j) {
          Vec& row = s_.za[row_of(r, j)];
          for (uint32_t l = 0; l < lanes; ++l) {
            const uint64_t a = get(zn_vec(r), src, scale * l + j);
            const uint64_t b = (f.shape == 3 || f.shape == 5) ? get(s_.v[zm], src, idx) : get(zm_vec(r), src, scale * l + j);
            if (fp) {
              auto w = [&](uint64_t v) { return bf ? (v & 0xFFFF) << 16 : convert(Format::F16, Format::F32, v, env); };
              const uint64_t x = sub ? negate(Format::F32, w(a)) : w(a);
              put(&row, 2, l, default_nan(Format::F32, fused_multiply_add(Format::F32, x, w(b), get(row, 2, l), env)));
              continue;
            }
            const uint64_t product = static_cast<uint64_t>(ext(a, src, n_signed) * ext(b, src, m_signed));
            put(&row, dst, l, (sub ? get(row, dst, l) - product : get(row, dst, l) + product) & element_ones(dst));
          }
        }
      return Step::Ok;
    }
    case k_fdot8: {      // FDOT (FP8 to half or single)
      const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
      const uint32_t group = dst == 1 ? 2 : 4, scale_amount = dst == 1 ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
      for (uint32_t r = 0; r < n; ++r) {
        Vec& row = s_.za[row_of(r, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          uint8_t a[4], b[4];
          for (uint32_t k = 0; k < group; ++k) {
            a[k] = static_cast<uint8_t>(get(zn_vec(r), 0, group * l + k));
            b[k] = static_cast<uint8_t>(get(f.shape == 3 ? s_.v[zm] : zm_vec(r), 0, f.shape == 3 ? group * idx + k : group * l + k));
          }
          put(&row, dst, l, fp8::dot_add(fa, a, fb, b, group, scale_amount, dst == 1 ? Format::F16 : Format::F32, static_cast<uint32_t>(get(row, dst, l)), (s_.fpmr >> 14) & 1));
        }
      }
      return Step::Ok;
    }
    case k_fvdot8: case k_fvdotb8: case k_fvdott8: {      // FVDOT, FVDOTB, FVDOTT: vertical dot products of a sliding window of two source vectors with a pair of bytes of Zm
      const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
      const bool to_half = op == k_fvdot8;
      const uint32_t scale_amount = to_half ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
      const uint32_t slots = to_half ? 2 : 1, vectors = to_half ? 2 : 4, stride_za = to_half ? 8 : 4, element = to_half ? 1 : 2;
      const uint32_t base = to_half ? ((rv & ~1u) + off) : (rv + off), msel = to_half ? 2 * idx : 4 * idx + (op == k_fvdott8 ? 2 : 0);
      for (uint32_t sl = 0; sl < slots; ++sl)
        for (uint32_t v = 0; v < vectors; ++v) {
          const uint32_t row_index = (base % stride_za) + sl + v * stride_za;
          if (row_index >= 16) continue;      // (the second slot does not wrap past the array)
          Vec& row = s_.za[row_index];
          for (uint32_t l = 0; l < lanes; ++l) {      // (the upper four half precision lanes see zeros, so only a NaN in Zm changes them)
            uint8_t a[2], b[2];
            for (uint32_t j = 0; j < 2; ++j) {
              a[j] = (l < 4 && zn + v + j < 32) ? static_cast<uint8_t>(get(s_.v[zn + v + j], 0, 4 * l + (to_half ? sl : 0))) : 0;      // (the window does not wrap: a register past z31 reads as zero)
              b[j] = static_cast<uint8_t>(get(s_.v[zm], 0, msel + j));
            }
            put(&row, element, l, fp8::dot_add(fa, a, fb, b, 2, scale_amount, to_half ? Format::F16 : Format::F32, static_cast<uint32_t>(get(row, element, l)), (s_.fpmr >> 14) & 1));
          }
        }
      return Step::Ok;
    }
    case k_fmlal8: case k_fmlall8: {      // FMLAL (FP8 into half) and FMLALL (FP8 into single) of two or four ZA vectors per source vector
      const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
      const uint32_t scale_amount = dst == 1 ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
      Env quiet = fp_env();
      for (uint32_t r = 0; r < n; ++r)
        for (uint32_t j = 0; j < scale; ++j) {
          Vec& row = s_.za[row_of(r, j)];
          for (uint32_t l = 0; l < lanes; ++l) {
            const uint8_t a = static_cast<uint8_t>(get(zn_vec(r), 0, scale * l + j));
            const uint8_t b = static_cast<uint8_t>((f.shape == 3 || f.shape == 5) ? get(s_.v[zm], 0, idx) : get(zm_vec(r), 0, scale * l + j));
            put(&row, dst, l, fp8::multiply_add(fa, a, fb, b, scale_amount, dst == 1 ? Format::F16 : Format::F32, static_cast<uint32_t>(get(row, dst, l)), (s_.fpmr >> 14) & 1, quiet));
          }
        }
      return Step::Ok;
    }
    default: break;
  }
  return undefined();
}

// Two or four vectors processed as that many single-vector SVE operations (the SME2 forms are the same arithmetic): the word of the SVE instruction is rebuilt for each vector with the registers of the
// group, with p7 all true for the predicated ones. Everything is read before anything is written.
AArch64Cpu::Step AArch64Cpu::sme2_vector(GuestMemory& m, uint32_t form, uint32_t insn) {
  const Sme2VecForm& f = kSme2VecForms[form];
  const uint32_t n = f.group;
  const uint32_t dn = n == 2 ? (bits_of(insn, 4, 1) << 1) : (bits_of(insn, 4, 2) << 2);
  const State saved = s_;
  Vec result[4];
  uint32_t fpsr = saved.fpsr;
  for (uint32_t r = 0; r < n; ++r) {
    s_ = saved;
    uint32_t zm;
    if (f.shape == 0) zm = bits_of(insn, 19, 16);
    else if (f.shape == 1) zm = (n == 2 ? (bits_of(insn, 20, 17) << 1) : (bits_of(insn, 20, 18) << 2)) + r;
    else zm = bits_of(insn, 20, 16);
    uint32_t word = f.sve_word;
    const uint32_t d = (dn + r) & 31;
    if (f.mode == 0) word = (word & ~0x3FFu) | (zm << 5) | d | (7u << 10);
    else if (f.mode == 1) word = (word & ~0x1F03FFu) | (zm << 16) | (d << 5) | d;
    else word = (word & ~0x1F03FFu) | (zm << 16) | (bits_of(insn, 9, 5) << 5) | d;
    s_.p[7] = 0xFFFF;
    const Step step = sve(m, word);
    if (step != Step::Ok) { s_ = saved; return step; }
    result[r] = s_.v[d];
    fpsr |= s_.fpsr;
  }
  s_ = saved;
  s_.fpsr = fpsr;
  for (uint32_t r = 0; r < n; ++r) s_.v[(dn + r) & 31] = result[r];
  return Step::Ok;
}

// Zip, unzip, unpack, narrowing conversions and shifts, FCVT, and SEL of two or four vectors (and the element-wise ones that reuse the SVE instruction).
AArch64Cpu::Step AArch64Cpu::sme2_misc(GuestMemory& m, uint32_t form, uint32_t insn) {
  const Sme2MiscForm& f = kSme2MiscForms[form];
  const uint32_t zd2 = bits_of(insn, 4, 1) << 1, zd4 = bits_of(insn, 4, 2) << 2, zn2 = bits_of(insn, 9, 6) << 1, zn4 = bits_of(insn, 9, 7) << 2;
  switch (f.kind) {
    case km_ZIP: case km_UZP: {
      const uint32_t n = f.b, esz = f.a, bytes = 1u << esz, per = 16u >> esz;
      const uint32_t dn = n == 2 ? zd2 : zd4;
      uint8_t in[4][16], out[4][16] = {};
      if (n == 2) { to_bytes(s_.v[bits_of(insn, 9, 5)], in[0]); to_bytes(s_.v[bits_of(insn, 20, 16)], in[1]); }
      else for (uint32_t k = 0; k < 4; ++k) to_bytes(s_.v[(zn4 + k) & 31], in[k]);
      for (uint32_t t = 0; t < n * per; ++t) {
        if (f.kind == km_ZIP) {
          const uint32_t src = t % n, e = t / n;
          for (uint32_t b = 0; b < bytes; ++b) out[t / per][(t % per) * bytes + b] = in[src][e * bytes + b];
        } else {      // out[j][e] = stream[n * e + j]
          const uint32_t j = t / per, e = t % per, s = n * e + j, src = s / per;
          for (uint32_t b = 0; b < bytes; ++b) out[j][e * bytes + b] = in[src][(s % per) * bytes + b];
        }
      }
      for (uint32_t k = 0; k < n; ++k) s_.v[(dn + k) & 31] = from_bytes(out[k]);
      return Step::Ok;
    }
    case km_UNPK: {
      const uint32_t n = f.d, desz = f.a, sesz = f.b;
      const bool is_signed = f.c;
      const uint32_t dn = n == 2 ? zd2 : zd4, sn = n == 2 ? bits_of(insn, 9, 5) : zn2;
      Vec out[4];
      for (uint32_t i = 0; i < n / 2; ++i) {
        const Vec src = s_.v[(sn + i) & 31];
        for (uint32_t half = 0; half < 2; ++half)
          for (uint32_t e = 0; e < (16u >> desz); ++e) {
            const uint64_t v = get(src, sesz, half * (16u >> desz) + e);
            put(&out[2 * i + half], desz, e, (is_signed ? static_cast<uint64_t>(sext(v, 8u << sesz)) : v) & element_ones(desz));
          }
      }
      for (uint32_t k = 0; k < n; ++k) s_.v[(dn + k) & 31] = out[k];
      return Step::Ok;
    }
    case km_FCVTW: {
      Env env = fp_env();
      const Vec src = s_.v[bits_of(insn, 9, 5)];
      Vec out[2];
      for (uint32_t half = 0; half < 2; ++half)
        for (uint32_t e = 0; e < 4; ++e) put(&out[half], 2, e, convert(Format::F16, Format::F32, get(src, 1, half * 4 + e), env));
      const uint32_t dn = zd2;
      s_.v[dn] = out[0];
      s_.v[(dn + 1) & 31] = out[1];
      fp_finish(env);
      return Step::Ok;
    }
    case km_FCVTN: {      // FCVT / FCVTN / BFCVT / BFCVTN: two single precision vectors to one of halves, one after the other or interleaved
      Env env = fp_env();
      const Format to = f.a ? Format::BF16 : Format::F16;
      Vec out;
      for (uint32_t k = 0; k < 2; ++k)
        for (uint32_t i = 0; i < 4; ++i) put(&out, 1, f.b ? 2 * i + k : 4 * k + i, convert(Format::F32, to, get(s_.v[(zn2 + k) & 31], 2, i), env));
      s_.v[bits_of(insn, 4, 0)] = out;
      fp_finish(env);
      return Step::Ok;
    }
    case km_NARROW: case km_RSHR: {
      const uint32_t mode = f.a, desz = f.b, nsrc = f.c, sesz = f.sve_word, lanes = 16u >> sesz;
      const bool interleave = f.d;
      const uint32_t sn = nsrc == 2 ? zn2 : zn4, dst = bits_of(insn, 4, 0);
      uint32_t shift = 0;
      if (f.kind == km_RSHR) {
        const uint32_t nbits = desz == 0 ? 32 : (sesz == 3 ? 64 : 16);
        const uint32_t field = nsrc == 2 ? bits_of(insn, 19, 16) : (sesz == 3 ? ((bits_of(insn, 22, 22) << 5) | bits_of(insn, 20, 16)) : bits_of(insn, 20, 16));
        shift = nbits - field;
      }
      const bool src_signed = mode != 1, dst_signed = mode == 0;
      Vec out;
      for (uint32_t k = 0; k < nsrc; ++k)
        for (uint32_t i = 0; i < lanes; ++i) {
          const uint64_t raw = get(s_.v[(sn + k) & 31], sesz, i);
          I128 v = src_signed ? static_cast<I128>(sext(raw, 8u << sesz)) : static_cast<I128>(raw);
          if (f.kind == km_RSHR) v = (v + (static_cast<I128>(1) << (shift - 1))) >> shift;
          const uint32_t bits = 8u << desz;
          const I128 hi = dst_signed ? (static_cast<I128>(1) << (bits - 1)) - 1 : (static_cast<I128>(1) << bits) - 1, lo = dst_signed ? -(static_cast<I128>(1) << (bits - 1)) : 0;
          if (v > hi) v = hi;
          if (v < lo) v = lo;
          put(&out, desz, interleave ? i * nsrc + k : k * lanes + i, static_cast<uint64_t>(v) & element_ones(desz));
        }
      s_.v[dst] = out;
      return Step::Ok;
    }
    case km_SEL: {
      const uint32_t n = f.b, esz = f.a, pn = 8 + bits_of(insn, 12, 10);
      const uint32_t dn = n == 2 ? zd2 : zd4, sn = n == 2 ? zn2 : zn4, mn = n == 2 ? (bits_of(insn, 20, 17) << 1) : (bits_of(insn, 20, 18) << 2);
      Vec out[4];
      for (uint32_t r = 0; r < n; ++r) {
        const uint16_t gp = counter_to_predicate(s_.p[pn], esz, r);
        for (uint32_t i = 0; i < (16u >> esz); ++i) put(&out[r], esz, i, active(gp, i, esz) ? get(s_.v[(sn + r) & 31], esz, i) : get(s_.v[(mn + r) & 31], esz, i));
      }
      for (uint32_t r = 0; r < n; ++r) s_.v[(dn + r) & 31] = out[r];
      return Step::Ok;
    }
    case km_SVEMAP: {
      const uint32_t n = f.a, dn = n == 2 ? zd2 : zd4, sn = n == 2 ? zn2 : zn4;
      const State saved = s_;
      Vec out[4];
      uint32_t fpsr = saved.fpsr;
      for (uint32_t r = 0; r < n; ++r) {
        s_ = saved;
        s_.p[7] = 0xFFFF;
        const Step step = sve(m, (f.sve_word & ~0x3FFu) | (((sn + r) & 31) << 5) | ((dn + r) & 31) | (7u << 10));
        if (step != Step::Ok) { s_ = saved; return step; }
        out[r] = s_.v[(dn + r) & 31];
        fpsr |= s_.fpsr;
      }
      s_ = saved;
      s_.fpsr = fpsr;
      for (uint32_t r = 0; r < n; ++r) s_.v[(dn + r) & 31] = out[r];
      return Step::Ok;
    }
  }
  return undefined();
}

bool AArch64Cpu::sme2_mop4_match(uint32_t insn, uint32_t* form) const {
  for (uint32_t i = 0; i < sizeof(kSme2Mop4Forms) / sizeof(kSme2Mop4Forms[0]); ++i)
    if ((insn & kSme2Mop4Forms[i].mask) == kSme2Mop4Forms[i].value) { *form = i; return true; }
  return false;
}

// MOP4: outer products without predicates whose operands are one or two vectors.
AArch64Cpu::Step AArch64Cpu::sme2_mop4(uint32_t form, uint32_t insn) {
  const Sme2Mop4Form& f = kSme2Mop4Forms[form];
  const uint32_t zn = bits_of(insn, 8, 6) << 1, zm = 16 + (bits_of(insn, 19, 17) << 1);
  const uint32_t src = f.src, dst = f.dst, rows = 16u >> dst, tile = insn & (dst == 3 ? 7u : dst == 2 ? 3u : 1u), tiles = 1u << dst;
  const bool sub = f.subtract;
  Env env = fp_env();
  for (uint32_t i = 0; i < rows; ++i)
    for (uint32_t j = 0; j < rows; ++j) {
      Vec& row = s_.za[i * tiles + tile];
      const uint64_t acc = get(row, dst, j);
      uint64_t result;
      const Vec a = s_.v[(zn + (f.nn == 2 && j >= rows / 2 ? 1 : 0)) & 31], b = s_.v[(zm + (f.nm == 2 && i >= rows / 2 ? 1 : 0)) & 31];      // (a pair is split by column half for the first operand and by row half for the second)
      if (f.op == mo_fmop48) {      // FP8 into half or single: groups of two or four bytes
        const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
        const uint32_t group = dst == 1 ? 2 : 4;
        uint8_t x[4], y[4];
        for (uint32_t k = 0; k < group; ++k) {
          x[k] = static_cast<uint8_t>(get(a, 0, group * i + k)) ^ (sub ? 0x80 : 0);
          y[k] = static_cast<uint8_t>(get(b, 0, group * j + k));
        }
        result = fp8::dot_add(fa, x, fb, y, group, dst == 1 ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F), dst == 1 ? Format::F16 : Format::F32, static_cast<uint32_t>(acc), (s_.fpmr >> 14) & 1);
      } else if (f.op == mo_fmop4 || f.op == mo_bfmop4) {
        const bool bf = f.op == mo_bfmop4;
        if (src == dst) {      // single, double and half precision (BFloat16 for BFMOP4 into half)
          const Format fmt = dst == 2 ? Format::F32 : dst == 3 ? Format::F64 : bf ? Format::BF16 : Format::F16;
          const uint64_t x = sub ? negate(fmt, get(a, src, i)) : get(a, src, i);
          result = default_nan(fmt, fused_multiply_add(fmt, x, get(b, src, j), acc, env));
        } else {      // half to single, two products per element
          uint64_t a0 = get(a, 1, 2 * i), a1 = get(a, 1, 2 * i + 1);
          const uint64_t b0 = get(b, 1, 2 * j), b1 = get(b, 1, 2 * j + 1);
          if (sub) { a0 ^= 0x8000; a1 ^= 0x8000; }
          if (bf) {
            Env quiet = fp_env();
            result = bfloat_dot_add(acc, a0, a1, b0, b1, quiet);
          } else {
            auto widen = [&](uint64_t v) { return convert(Format::F16, Format::F32, v & 0xFFFF, env); };
            const uint64_t t = fused_multiply_add(Format::F32, widen(a1), widen(b1), mul(Format::F32, widen(a0), widen(b0), env), env);
            result = default_nan(Format::F32, add(Format::F32, acc, default_nan(Format::F32, t), env));
          }
        }
      } else {
        const uint32_t c = dst == 2 && src == 1 ? 2 : 4;
        const bool a_signed = f.op == mo_smop4 || f.op == mo_sumop4, b_signed = f.op == mo_smop4 || f.op == mo_usmop4;
        uint64_t sum = acc;
        for (uint32_t k = 0; k < c; ++k) {
          const uint64_t x = get(a, src, c * i + k), y = get(b, src, c * j + k);
          const uint64_t product = static_cast<uint64_t>((a_signed ? sext(x, 8u << src) : static_cast<int64_t>(x)) * (b_signed ? sext(y, 8u << src) : static_cast<int64_t>(y)));
          sum = sub ? sum - product : sum + product;
        }
        result = sum & element_ones(dst);
      }
      put(&row, dst, j, result);
    }
  return Step::Ok;
}

// FP8 conversions of vectors: bytes to two vectors of halves (consecutive, or even and odd bytes with the "L" forms), and halves or singles to bytes.
AArch64Cpu::Step AArch64Cpu::sme2_fp8_convert(uint32_t insn) {
  if ((insn & 0xFF3FFC00u) == 0xC126E000u) {      // F1CVT, F2CVT, BF1CVT, BF2CVT and the L forms
    const uint32_t opc = bits_of(insn, 23, 22), second = (opc >> 1) & 1;      // (bit 23: the second format; bit 22: BFloat16)
    const bool bfloat = opc & 1, interleave = insn & 1;
    const fp8::Fmt fmt = static_cast<fp8::Fmt>((s_.fpmr >> (second ? 3 : 0)) & 1);
    const uint32_t lscale = second ? static_cast<uint32_t>((s_.fpmr >> 32) & 0x3F) : static_cast<uint32_t>((s_.fpmr >> 16) & (bfloat ? 0x3F : 0x7F));
    const uint32_t zd = bits_of(insn, 4, 1) << 1;
    const Vec src = s_.v[bits_of(insn, 9, 5)];
    Vec out[2];
    for (uint32_t k = 0; k < 2; ++k)
      for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t b = static_cast<uint8_t>(get(src, 0, interleave ? 2 * i + k : 8 * k + i));
        bool inexact;
        put(&out[k], 1, i, fp8::widen(fmt, b, bfloat ? Format::BF16 : Format::F16, bfloat ? lscale : (lscale & 15), &inexact));
      }
    s_.v[zd] = out[0];
    s_.v[(zd + 1) & 31] = out[1];
    return Step::Ok;
  }
  const bool halves = (insn & 0xFFBFFC20u) == 0xC124E000u, singles = (insn & 0xFFFFFC40u) == 0xC134E000u;
  if (halves || singles) {
    const fp8::Fmt fd = static_cast<fp8::Fmt>((s_.fpmr >> 6) & 1);
    const bool saturate = (s_.fpmr >> 15) & 1, flag = halves ? (insn >> 22) & 1 : (insn >> 5) & 1;      // (bfloat for the halves, interleaved for the singles)
    uint32_t flags = 0;
    Vec out;
    if (halves) {
      const uint32_t zn = bits_of(insn, 9, 6) << 1;
      const int32_t nscale = flag ? static_cast<int32_t>(static_cast<int8_t>(s_.fpmr >> 24)) : static_cast<int32_t>((s_.fpmr >> 24) & 15) - static_cast<int32_t>((s_.fpmr >> 24) & 16);
      for (uint32_t k = 0; k < 2; ++k)
        for (uint32_t i = 0; i < 8; ++i) put(&out, 0, 8 * k + i, fp8::narrow(fd, flag ? Format::BF16 : Format::F16, get(s_.v[(zn + k) & 31], 1, i), nscale, saturate, &flags));
    } else {
      const uint32_t zn = bits_of(insn, 9, 7) << 2;
      const int32_t nscale = static_cast<int8_t>(s_.fpmr >> 24);
      for (uint32_t k = 0; k < 4; ++k)
        for (uint32_t i = 0; i < 4; ++i) put(&out, 0, flag ? 4 * i + k : 4 * k + i, fp8::narrow(fd, Format::F32, get(s_.v[(zn + k) & 31], 2, i), nscale, saturate, &flags));
    }
    s_.v[insn & 31] = out;
    return Step::Ok;
  }
  return undefined();
}

AArch64Cpu::Step AArch64Cpu::sme2(GuestMemory& m, uint32_t insn) {
  if ((insn & 0xFF3FFC00u) == 0xC126E000u || (insn & 0xFFBFFC20u) == 0xC124E000u || (insn & 0xFFFFFC40u) == 0xC134E000u) return sme2_fp8_convert(insn);
  for (uint32_t i = 0; i < sizeof(kSme2MiscForms) / sizeof(kSme2MiscForms[0]); ++i)
    if ((insn & kSme2MiscForms[i].mask) == kSme2MiscForms[i].value) return sme2_misc(m, i, insn);
  for (uint32_t i = 0; i < sizeof(kSme2VecForms) / sizeof(kSme2VecForms[0]); ++i)
    if ((insn & kSme2VecForms[i].mask) == kSme2VecForms[i].value) return sme2_vector(m, i, insn);
  for (uint32_t i = 0; i < sizeof(kSme2Forms) / sizeof(kSme2Forms[0]); ++i)
    if ((insn & kSme2Forms[i].mask) == kSme2Forms[i].value) return sme2_array(i, insn);
  return undefined();
}

}  // namespace emulation
